#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ring_host.h"

static fsim_shared_t *S;

enum { ACT_BOOT, ACT_ENQ, ACT_PEEK_POP, ACT_PEEK_ERROR, ACT_SUITE, ACT_LOSS_PREP,
       ACT_BOUNDARY_PREP };

static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); exit(1);
}

static host_master_record_t rec(uint32_t uid, uint8_t type)
{
    host_master_record_t r; memset(&r, 0, sizeof(r));
    r.type = type; r.unit_id = type == REC_TYPE_NODE ? 7 : 0;
    r.seq = uid; r.boot_id = 0xB0070000u ^ uid; r.rx_ms = uid;
    r.rain_tips_total = uid & 0xffu; r.temp_c = 20.0f + (float)(uid % 10u);
    return r;
}

static void journal(uint32_t op, uint32_t uid, int result)
{
    if (S->jcount >= FSIM_JMAX) _exit(FSIM_EXIT_JFULL);
    fsim_jentry_t *j = &S->journal[S->jcount++];
    *j = (fsim_jentry_t){ .op=op, .uid=uid, .result=result,
        .a=prod_head(), .b=prod_tail(), .c=prod_count(), .d=prod_write_seq() };
}

static void assert_child(bool ok, const char *what)
{ if (!ok) { fprintf(stderr, "child FAIL: %s\n", what); _exit(90); } }

static void child_suite(void)
{
    prod_reset_and_init();
    assert_child(prod_enabled(), "backup disabled");
    const uint32_t cap = prod_slot_count() - 1u;

    for (uint32_t i = 1; i <= cap; ++i) {
        host_master_record_t r = rec(i, REC_TYPE_NODE);
        assert_child(prod_enqueue(&r), "fill enqueue");
    }
    assert_child(prod_count() == cap, "capacity count");
    host_master_record_t extra = rec(cap + 1u, REC_TYPE_NODE);
    assert_child(!prod_enqueue(&extra), "full ring accepted extra record");

    for (uint32_t i = 1; i <= cap; ++i) {
        host_master_record_t a, b;
        assert_child(prod_peek(&a) && prod_peek(&b), "peek during full drain");
        assert_child(a.rx_ms == i && memcmp(&a, &b, sizeof(a)) == 0, "peek order/idempotence");
        assert_child(prod_pop(), "pop during full drain");
    }
    assert_child(prod_count() == 0, "not empty after drain");

    /* Egy teljes ring-lap után szektorhatárokon stale erase következik. */
    for (uint32_t i = 0; i < 80; ++i) {
        host_master_record_t r = rec(100000u + i, REC_TYPE_NODE);
        assert_child(prod_enqueue(&r), "post-wrap enqueue");
        host_master_record_t p; assert_child(prod_peek(&p), "post-wrap peek");
        assert_child(p.rx_ms == r.rx_ms, "post-wrap order");
        assert_child(prod_pop(), "post-wrap pop");
    }
    assert_child(S->program_violations == 0, "NOR 0->1 program attempt");
    assert_child(S->oob_accesses == 0 && S->erase_misaligned == 0, "flash geometry violation");
    journal(JOP_DONE, cap, 1);
    _exit(0);
}

static void child_action(int act, host_master_record_t r)
{
    prod_reset_and_init();
    if (!prod_enabled()) _exit(FSIM_EXIT_DISABLED);
    journal(JOP_BOOT, prod_master_seq(), 1);
    if (act == ACT_ENQ) {
        bool ok = prod_enqueue(&r); journal(JOP_ENQ, r.rx_ms, ok);
        S->journal[S->jcount-1].a = r.seq;
        _exit(0);
    }
    if (act == ACT_PEEK_POP) {
        host_master_record_t p; bool ok = prod_peek(&p);
        journal(JOP_PEEK, ok ? p.rx_ms : 0, ok);
        if (ok) { bool popped = prod_pop(); journal(JOP_POP, p.rx_ms, popped); }
        _exit(0);
    }
    if (act == ACT_PEEK_ERROR) {
        uint32_t h = prod_head(), t = prod_tail(), c = prod_count();
        fsim_fail_next_read(); host_master_record_t p;
        bool ok = prod_peek(&p);
        assert_child(!ok && h == prod_head() && t == prod_tail() && c == prod_count(),
                     "transient peek read moved tail");
        journal(JOP_PEEK, 0, 0); _exit(0);
    }
    if (act == ACT_LOSS_PREP) {
        host_master_record_t first = rec(700001, REC_TYPE_MASTER);
        assert_child(prod_enqueue(&first) && first.seq == 1, "loss prep first master");
        assert_child(prod_pop(), "loss prep first pop");
        for (uint32_t i=0; i<FSIM_SLOTS; ++i) {
            host_master_record_t n = rec(800000u+i, REC_TYPE_NODE);
            assert_child(prod_enqueue(&n), "loss prep wrap enqueue");
            assert_child(prod_pop(), "loss prep wrap pop");
        }
        journal(JOP_DONE, first.seq, 1); _exit(0);
    }
    if (act == ACT_BOUNDARY_PREP) {
        for (uint32_t i=0;i<32;++i) {
            host_master_record_t n=rec(1000+i,REC_TYPE_NODE);
            assert_child(prod_enqueue(&n),"boundary seed enqueue");
        }
        for (uint32_t i=0;i<32;++i) {
            host_master_record_t p; assert_child(prod_peek(&p) && p.rx_ms==1000+i,"boundary seed drain");
            assert_child(prod_pop(),"boundary seed pop");
        }
        for (uint32_t i=32;i<FSIM_SLOTS;++i) {
            host_master_record_t n=rec(1000+i,REC_TYPE_NODE),p;
            assert_child(prod_enqueue(&n),"boundary lap enqueue");
            assert_child(prod_peek(&p) && p.rx_ms==1000+i,"boundary lap peek");
            assert_child(prod_pop(),"boundary lap pop");
        }
        for (uint32_t i=0;i<16;++i) {
            host_master_record_t n=rec(50000+i,REC_TYPE_NODE);
            assert_child(prod_enqueue(&n),"boundary pending enqueue");
        }
        journal(JOP_DONE,50000,1); _exit(0);
    }
    _exit(0);
}

static int run_fork(int act, host_master_record_t r, int64_t cut, uint64_t rng)
{
    S->jcount = 0; S->cut_countdown = cut; S->rng = rng ? rng : 1;
    pid_t pid = fork(); if (pid < 0) die("fork failed");
    if (pid == 0) {
        fsim_attach(S);
        if (act == ACT_SUITE) child_suite(); else child_action(act, r);
    }
    int st; if (waitpid(pid, &st, 0) < 0) die("waitpid failed");
    if (WIFSIGNALED(st)) die("child signal %d", WTERMSIG(st));
    return WEXITSTATUS(st);
}

static void fresh(void)
{
    memset(S, 0, sizeof(*S)); memset(S->flash, 0xff, sizeof(S->flash));
    S->cut_countdown = -1; S->next_uid = 1;
}

static fsim_jentry_t *last(uint32_t op)
{
    for (uint32_t i = S->jcount; i > 0; --i) if (S->journal[i-1].op == op) return &S->journal[i-1];
    return NULL;
}

static void erase_nvs_key(const char *key)
{
    for (unsigned i=0; i<FSIM_NVS_MAX; ++i)
        if (S->nvs[i].used && strcmp(S->nvs[i].key,key)==0) { S->nvs[i].used=0; return; }
    die("NVS key not found: %s", key);
}

static uint32_t peek_pop_uid(void)
{
    int rc = run_fork(ACT_PEEK_POP, rec(0,REC_TYPE_NODE), -1, 1);
    if (rc) die("peek/pop child rc=%d", rc);
    fsim_jentry_t *p = last(JOP_PEEK);
    return (p && p->result) ? p->uid : 0;
}

static uint32_t boot_count(void)
{
    int rc = run_fork(ACT_BOOT, rec(0,REC_TYPE_NODE), -1, 1);
    if (rc) die("reboot consistency rc=%d", rc);
    fsim_jentry_t *b = last(JOP_BOOT);
    if (!b) die("missing boot journal");
    return b->c;
}

static void assert_flash_clean(void)
{
    if (S->program_violations || S->oob_accesses || S->erase_misaligned)
        die("flash invariant program=%u oob=%u align=%u", S->program_violations,
            S->oob_accesses, S->erase_misaligned);
}

static unsigned power_cut_sweep(void)
{
    unsigned cases=0;
    /* Non-boundary slot-write (budget 0), majd NVS-index commit (budget 1). */
    for (int budget=0; budget<=1; ++budget) for (uint64_t seed=1; seed<=8; ++seed) {
        fresh(); run_fork(ACT_BOOT,rec(0,REC_TYPE_NODE),-1,1);
        run_fork(ACT_ENQ,rec(100,REC_TYPE_NODE),-1,1);
        int rc=run_fork(ACT_ENQ,rec(101,REC_TYPE_NODE),budget,seed);
        if (rc!=FSIM_EXIT_CUT) die("enqueue sweep expected cut budget=%d seed=%llu rc=%d",
                                  budget,(unsigned long long)seed,rc);
        uint32_t count=boot_count();
        if (count!=1 && count!=2) die("enqueue cut inconsistent count=%u",count);
        if (peek_pop_uid()!=100) die("enqueue cut lost/reordered prior record");
        if (count==1) {
            rc=run_fork(ACT_ENQ,rec(101,REC_TYPE_NODE),-1,seed);
            if (rc || !last(JOP_ENQ)->result) die("enqueue cut recovery failed");
        }
        if (peek_pop_uid()!=101 || peek_pop_uid()!=0) die("incoming not exactly once after recovery");
        assert_flash_clean(); ++cases;
    }

    /* Pop NVS commit: reboot után régi front vagy pontosan a következő. */
    for (uint64_t seed=1; seed<=8; ++seed) {
        fresh(); run_fork(ACT_BOOT,rec(0,REC_TYPE_NODE),-1,1);
        for (uint32_t u=200;u<203;++u) run_fork(ACT_ENQ,rec(u,REC_TYPE_NODE),-1,1);
        int rc=run_fork(ACT_PEEK_POP,rec(0,REC_TYPE_NODE),0,seed);
        if (rc!=FSIM_EXIT_CUT || !last(JOP_PEEK) || last(JOP_PEEK)->uid!=200)
            die("pop sweep cut setup seed=%llu",(unsigned long long)seed);
        uint32_t first=peek_pop_uid();
        if (first!=200 && first!=201) die("pop cut skipped/swapped first=%u",first);
        uint32_t expected=first+1;
        while (expected<=202) {
            if (peek_pop_uid()!=expected) die("pop cut order expected=%u",expected);
            ++expected;
        }
        if (peek_pop_uid()!=0) die("pop cut residual record");
        assert_flash_clean(); ++cases;
    }

    /* Valódi stale sector-boundary erase, miközben az előző sector 16 rekordja pending. */
    for (uint64_t seed=1; seed<=4; ++seed) {
        fresh(); run_fork(ACT_BOOT,rec(0,REC_TYPE_NODE),-1,1);
        int prep=run_fork(ACT_BOUNDARY_PREP,rec(0,REC_TYPE_NODE),-1,seed);
        if (prep || !last(JOP_DONE)) die("boundary prep seed=%llu",(unsigned long long)seed);
        int rc=run_fork(ACT_ENQ,rec(50016,REC_TYPE_NODE),0,seed);
        if (rc!=FSIM_EXIT_CUT || boot_count()!=16) die("sector erase cut state seed=%llu",
                                                       (unsigned long long)seed);
        rc=run_fork(ACT_ENQ,rec(50016,REC_TYPE_NODE),-1,seed);
        if (rc || !last(JOP_ENQ)->result || boot_count()!=17) die("sector erase recovery");
        for (uint32_t i=0;i<17;++i)
            if (peek_pop_uid()!=50000+i) die("sector cut lost/reordered i=%u",i);
        if (peek_pop_uid()!=0) die("sector cut residual");
        assert_flash_clean(); ++cases;
    }
    return cases;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    S = mmap(NULL, sizeof(*S), PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);
    if (S == MAP_FAILED) die("mmap failed");

    fresh();
    int rc = run_fork(ACT_SUITE, rec(0, REC_TYPE_NODE), -1, 1);
    if (rc != 0) die("full/wrap suite failed rc=%d", rc);
    fsim_jentry_t *done = last(JOP_DONE);
    if (!done) die("full/wrap suite missing completion");
    printf("PASS full/wrap/sector/capacity: cap=%u slots=%u\n", done->uid, FSIM_SLOTS);

    fresh();
    rc = run_fork(ACT_BOOT, rec(0, REC_TYPE_NODE), -1, 1);
    if (rc) die("initial boot failed rc=%d", rc);
    host_master_record_t r1 = rec(10, REC_TYPE_NODE);
    if (run_fork(ACT_ENQ, r1, -1, 1) != 0 || !last(JOP_ENQ)->result) die("setup enqueue failed");
    if (run_fork(ACT_PEEK_ERROR, r1, -1, 1) != 0) die("peek-error test failed");
    if (run_fork(ACT_PEEK_POP, r1, -1, 1) != 0 || last(JOP_PEEK)->uid != 10) die("peek retry failed");
    printf("PASS transient peek read error preserves tail\n");

    /* Teljes slot + nem publikált NVS index: rebootkor orphan-adopt, duplikáció nélkül. */
    fresh(); run_fork(ACT_BOOT, rec(0, REC_TYPE_NODE), -1, 1);
    host_master_record_t base = rec(20, REC_TYPE_NODE), orphan = rec(21, REC_TYPE_NODE);
    run_fork(ACT_ENQ, base, -1, 1);
    rc = run_fork(ACT_ENQ, orphan, 1, 2); /* write kész, commit cut; rng=2 => régi index */
    if (rc != FSIM_EXIT_CUT) die("expected orphan cut, got rc=%d", rc);
    rc = run_fork(ACT_ENQ, orphan, -1, 1);
    if (rc || !last(JOP_ENQ) || !last(JOP_ENQ)->result || last(JOP_ENQ)->c != 2)
        die("orphan adoption failed/duplicated");
    run_fork(ACT_PEEK_POP, orphan, -1, 1);
    if (last(JOP_PEEK)->uid != 20) die("orphan test first order");
    run_fork(ACT_PEEK_POP, orphan, -1, 1);
    if (last(JOP_PEEK)->uid != 21) die("orphan test second order");
    printf("PASS orphan-adopt + retry idempotence\n");

    /* Pop commit vágás: reboot után legfeljebb az ACK-olt front ismétlődhet. */
    fresh(); run_fork(ACT_BOOT, rec(0, REC_TYPE_NODE), -1, 1);
    for (uint32_t u=30; u<33; ++u) run_fork(ACT_ENQ, rec(u, REC_TYPE_NODE), -1, 1);
    rc = run_fork(ACT_PEEK_POP, rec(0, REC_TYPE_NODE), 0, 2);
    if (rc != FSIM_EXIT_CUT || !last(JOP_PEEK) || last(JOP_PEEK)->uid != 30) die("pop cut setup");
    run_fork(ACT_PEEK_POP, rec(0, REC_TYPE_NODE), -1, 1);
    uint32_t after = last(JOP_PEEK)->uid;
    if (after != 30 && after != 31) die("pop cut skipped records: next=%u", after);
    printf("PASS pop power-cut old/new atomic outcome (next=%u)\n", after);

    /* Master seq az indexben együtt commitolódik és reboot után folytatódik. */
    fresh(); run_fork(ACT_BOOT, rec(0, REC_TYPE_MASTER), -1, 1);
    host_master_record_t m1 = rec(1000, REC_TYPE_MASTER);
    run_fork(ACT_ENQ, m1, -1, 1);
    fsim_jentry_t *e1 = last(JOP_ENQ);
    if (!e1 || e1->a != 1) die("master first assigned seq=%u", e1 ? e1->a : 0);
    host_master_record_t m2 = rec(2000, REC_TYPE_MASTER);
    run_fork(ACT_ENQ, m2, -1, 1);
    fsim_jentry_t *e2 = last(JOP_ENQ);
    if (!e2 || e2->a != 2) die("master second assigned seq=%u", e2 ? e2->a : 0);
    run_fork(ACT_BOOT, rec(0,REC_TYPE_MASTER), -1, 1);
    if (!last(JOP_BOOT) || last(JOP_BOOT)->uid != 2) die("prod_master_seq after reboot != 2");
    run_fork(ACT_PEEK_POP, m1, -1, 1); fsim_jentry_t *p1 = last(JOP_PEEK);
    if (!p1 || p1->uid != 1000) die("master first record missing");
    run_fork(ACT_PEEK_POP, m2, -1, 1); fsim_jentry_t *p2 = last(JOP_PEEK);
    if (!p2 || p2->uid != 2000) die("master second record missing");
    printf("PASS master sequence/index reboot persistence\n");

    /* Reprodukció: normál ring-lap kitörli a régi slot0-t; state2 elvesztése után seq=1. */
    fresh();
    rc = run_fork(ACT_LOSS_PREP, rec(0,REC_TYPE_NODE), -1, 1);
    if (rc) die("NVS-loss prep failed rc=%d", rc);
    erase_nvs_key("state2");
    host_master_record_t after_loss = rec(700002, REC_TYPE_MASTER);
    rc = run_fork(ACT_ENQ, after_loss, -1, 1);
    fsim_jentry_t *collision = last(JOP_ENQ);
    if (rc || !collision || collision->a != 1)
        die("expected current master seq-reset reproduction, got %u", collision ? collision->a : 0);
    host_master_record_t before_loss = rec(700001, REC_TYPE_MASTER);
    before_loss.seq = 1;
    before_loss.boot_id = 0; /* pre-fix backlog/cloud rekord */
    after_loss.seq = collision->a;
    char old_key[40], new_key[40];
    if (prod_master_record_key(old_key,sizeof(old_key),&before_loss) <= 0 ||
        prod_master_record_key(new_key,sizeof(new_key),&after_loss) <= 0 ||
        strcmp(old_key,new_key)==0)
        die("boot-scoped master keys still collide: %s", old_key);
    printf("PASS reproduced seq reset remains cloud-safe: %s != %s\n", old_key, new_key);

    if (S->program_violations || S->oob_accesses || S->erase_misaligned)
        die("flash invariant counters: program=%u oob=%u align=%u", S->program_violations,
            S->oob_accesses, S->erase_misaligned);
    printf("PASS master_ring_sim mutations=%" PRIu64 " reads=%" PRIu64 " cuts=%u\n",
           S->mutation_ops, S->read_ops, S->cuts_fired);
    unsigned sweep_cases=power_cut_sweep();
    printf("PASS bounded power-cut sweep: %u cases (slot-write/index-commit/pop/stale-sector-erase)\n",
           sweep_cases);
    return 0;
}

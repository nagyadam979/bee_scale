/*
 * fuzz_main.c — power-loss fuzz vezérlő a node_buffer.c (VÁLTOZATLAN produkciós forrás) köré.
 *
 * Architektúra:
 *   - A "flash" (mock NVS map) + a referencia-modell MAP_SHARED memóriában él.
 *   - Minden "boot" egy fork()-olt gyerek-processz: node_buffer_init() + véletlen
 *     enqueue/drain workload. A gyerek exitje = reset: a node_buffer.c static state-je
 *     (s_nvs, s_ready, s_head/next/count) tényleg elveszik, a flash megmarad.
 *   - Az áramszünetet a mock NVS injektálja: cut_budget API-hívás után _exit(SIM_EXIT_CUT).
 *
 * Orákulum (referencia-modell, lásd sim_shared.h):
 *   LOSS      : ref_floor < seq < ref_next rekordnak reboot után sorrendben, ép
 *               tartalommal olvashatónak kell lennie.
 *   RESURRECT : seq <= ref_acked (befejezett clear_until-lal lefedett) nem térhet vissza.
 *   Cut-olt enqueue: a rekord jelenléte VAGY hiánya is elfogadott; ha jelen van, initnek
 *               adoptálnia kell és a next_seq-nek konzisztensnek kell lennie.
 *   Init      : sikeres init után state2 érvényes, count == next-head (mod 2^32),
 *               count <= NODE_PENDING_CAPACITY, RAM state == flash state2, és a flashben
 *               nincs rekord a [head, next) ablakon kívül (szivárgás/önkényes tisztítás).
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sim_shared.h"
#include "nvs.h"          /* mock: NVS_TYPE_* a backdoor ellenőrzésekhez */
#include "node_buffer.h"  /* produkciós header (esp_err stub + valódi protocol.h) */
#include "node_config.h"  /* NODE_ID, NODE_PENDING_CAPACITY (valódi fájl) */

static sim_shared_t *S;

/* ---------------- wrap-safe seq összehasonlítás (a node_buffer.c idiómája) ------------- */
static bool sgt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static bool slt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static bool sle(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

/* ---------------- PRNG (determinisztikus repróhoz) ---------------- */
static uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t g_rng; /* gyerek-oldali PRNG state */
static uint32_t rnd(void) { return (uint32_t)(splitmix64(&g_rng) >> 32); }

/* ---------------- state2 blob tükör (a node_buffer.c privát layoutjának másolata) ------ */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t head;
    uint32_t next;
    uint32_t count;
    uint16_t crc;
} state2_blob_t;

_Static_assert(sizeof(state2_blob_t) == 22, "state2 tükör-layout eltér");

#define STATE2_MAGIC   0x3246424EU
#define STATE2_VERSION 2U

/* ---------------- determinisztikus payload (a main.c build_payload mintájára) ---------- */
static void make_payload(uint32_t seq, sz2_payload_t *p)
{
    memset(p, 0, sizeof(*p));
    p->protocol_version  = SZ2_PROTO_VERSION;
    p->packet_type       = SZ2_PKT_DATA;
    p->node_id           = NODE_ID;
    for (int i = 0; i < 6; i++) {
        p->node_mac[i] = (uint8_t)(0xA0 + i);
    }
    p->boot_id           = seq * 2654435761U;
    p->seq               = seq;
    p->packet_idx        = seq;
    p->total_packets     = 0;
    p->send_timestamp_ms = seq ^ 0x5A5A5A5AU;
    p->weight            = (float)(seq % 100000U);
    p->temp              = 21.5f;
    p->vbat              = (uint16_t)(3300U + (seq & 0xFFU));
    p->crc               = sz2_payload_crc(p);
}

/* ---------------- backdoor flash-hozzáférés (nem tickel, nem vágható) ------------------ */
static sim_entry_t *find_entry(const char *key)
{
    for (int i = 0; i < SIM_MAX_ENTRIES; i++) {
        if (S->entries[i].used && strncmp(S->entries[i].key, key, SIM_KEY_SIZE) == 0) {
            return &S->entries[i];
        }
    }
    return NULL;
}

static sim_entry_t *find_record(uint32_t seq)
{
    char key[SIM_KEY_SIZE];
    snprintf(key, sizeof(key), "r%08" PRIX32, seq);
    return find_entry(key);
}

/* Teszt-oldali másolat a node_buffer.c parse_record_key logikájáról. */
static bool parse_rec_key(const char *key, uint32_t *seq_out)
{
    if (strnlen(key, SIM_KEY_SIZE) != 9 || key[0] != 'r') {
        return false;
    }
    uint32_t seq = 0;
    for (int i = 1; i <= 8; i++) {
        char c = key[i];
        uint8_t nib;
        if (c >= '0' && c <= '9')      nib = (uint8_t)(c - '0');
        else if (c >= 'A' && c <= 'F') nib = (uint8_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') nib = (uint8_t)(c - 'a' + 10);
        else return false;
        seq = (seq << 4) | nib;
    }
    *seq_out = seq;
    return true;
}

/* ---------------- FAIL riport ---------------- */
static _Noreturn void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S->fail_msg, sizeof(S->fail_msg), fmt, ap);
    va_end(ap);
    _exit(SIM_EXIT_FAIL);
}

static bool must_survive_nonempty(void)
{
    return (int32_t)(S->ref_next - (S->ref_floor + 1U)) > 0;
}

/* ---------------- validálás minden sikeres init után (vágás kikapcsolva) --------------- */
static void validate_after_init(void)
{
    uint32_t mnext  = node_buffer_next_seq();
    uint32_t mcount = node_buffer_count();

    sim_entry_t *se = find_entry("state2");
    if (se == NULL) {
        fail("state2 hianyzik sikeres init utan");
    }
    if (se->type != (uint8_t)NVS_TYPE_BLOB || se->len != sizeof(state2_blob_t)) {
        fail("state2 type/len rossz: type=0x%02X len=%u", se->type, se->len);
    }
    state2_blob_t b;
    memcpy(&b, se->data, sizeof(b));
    if (b.magic != STATE2_MAGIC || b.version != STATE2_VERSION || b.size != sizeof(b)) {
        fail("state2 fejlec rossz: magic=0x%08" PRIX32 " ver=%u size=%u",
             b.magic, b.version, b.size);
    }
    uint16_t crc = sz2_crc16((const uint8_t *)&b, offsetof(state2_blob_t, crc));
    if (b.crc != crc) {
        fail("state2 CRC rossz: 0x%04X != 0x%04X", b.crc, crc);
    }
    if (b.count > NODE_PENDING_CAPACITY) {
        fail("count=%" PRIu32 " > cap=%u", b.count, NODE_PENDING_CAPACITY);
    }
    if ((uint32_t)(b.next - b.head) != b.count) {
        fail("count != next-head: head=%" PRIu32 " next=%" PRIu32 " count=%" PRIu32,
             b.head, b.next, b.count);
    }
    if (mnext != b.next || mcount != b.count) {
        fail("RAM/flash elteres: RAM next=%" PRIu32 " count=%" PRIu32
             " vs state2 next=%" PRIu32 " count=%" PRIu32, mnext, mcount, b.next, b.count);
    }

    /* Cut-olt enqueue feloldása: ha a rekord flashben van, adoptálva kellett lennie. */
    if (S->inflight_enq) {
        if (S->inflight_enq_seq != S->ref_next) {
            fail("inflight_enq marker seq=%" PRIu32 " != ref_next=%" PRIu32,
                 S->inflight_enq_seq, S->ref_next);
        }
        if (find_record(S->inflight_enq_seq) != NULL) {
            if (b.next != S->inflight_enq_seq + 1U) {
                fail("orphan seq=%" PRIu32 " jelen van, de nincs adoptalva (next=%" PRIu32 ")",
                     S->inflight_enq_seq, b.next);
            }
            S->ref_next = S->inflight_enq_seq + 1U;
        }
        S->inflight_enq = false;
    }
    S->inflight_clr = false; /* a szürke zónát a ref_floor már tartja */

    if (b.next != S->ref_next) {
        fail("NEXT elteres: state2 next=%" PRIu32 " ref_next=%" PRIu32, b.next, S->ref_next);
    }
    if (b.count > 0 && !sgt(b.head, S->ref_acked)) {
        fail("RESURRECT: head=%" PRIu32 " <= acked=%" PRIu32, b.head, S->ref_acked);
    }

    /* LOSS + tartalom: a [head, next) ablakban minden ref_floor feletti seq kötelező. */
    for (uint32_t s = b.head; s != b.next; s++) {
        sim_entry_t *rec = find_record(s);
        bool must = sgt(s, S->ref_floor);
        if (rec == NULL) {
            if (must) {
                fail("LOSS: seq=%" PRIu32 " hianyzik (head=%" PRIu32 " next=%" PRIu32
                     " floor=%" PRIu32 " acked=%" PRIu32 ")",
                     s, b.head, b.next, S->ref_floor, S->ref_acked);
            }
            continue; /* szürke zóna (cut-olt clear célja): hiányozhat */
        }
        if (rec->type != (uint8_t)NVS_TYPE_BLOB || rec->len != sizeof(sz2_payload_t)) {
            fail("rekord seq=%" PRIu32 " type/len rossz: type=0x%02X len=%u",
                 s, rec->type, rec->len);
        }
        sz2_payload_t exp;
        make_payload(s, &exp);
        if (memcmp(rec->data, &exp, sizeof(exp)) != 0) {
            fail("rekord tartalom elter seq=%" PRIu32, s);
        }
    }

    /* Szivárgás / ablakon kívüli rekord: minden r-kulcsnak [head, next)-ben kell lennie. */
    for (int i = 0; i < SIM_MAX_ENTRIES; i++) {
        const sim_entry_t *e = &S->entries[i];
        if (!e->used || e->type != (uint8_t)NVS_TYPE_BLOB) {
            continue;
        }
        uint32_t s;
        if (!parse_rec_key(e->key, &s)) {
            continue; /* state2; a legacy kulcsok u32-k, ide se jutnak */
        }
        bool in_window = !slt(s, b.head) && slt(s, b.next);
        if (!in_window) {
            fail("ablakon kivuli rekord seq=%" PRIu32 " (head=%" PRIu32 " next=%" PRIu32 ")",
                 s, b.head, b.next);
        }
    }

    /* Szigorítás: ami most a sorban van, annak innentől túl kell élnie. */
    S->ref_floor = b.head - 1U;
}

/* ---------------- workload lépések ---------------- */
static void do_enqueue(void)
{
    uint32_t seq = node_buffer_next_seq();
    if (seq != S->ref_next) {
        fail("next_seq elteres workloadban: RAM=%" PRIu32 " ref=%" PRIu32, seq, S->ref_next);
    }
    sz2_payload_t p;
    make_payload(seq, &p);

    S->inflight_enq = true;
    S->inflight_enq_seq = seq;
    esp_err_t err = node_buffer_enqueue(&p);
    S->inflight_enq = false;

    if (err == ESP_OK) {
        S->ref_next = seq + 1U;
        S->enq_ok++;
    } else if (err == ESP_ERR_NO_MEM) {
        if (node_buffer_count() != NODE_PENDING_CAPACITY) {
            fail("enqueue NO_MEM de count=%" PRIu32 " != cap", node_buffer_count());
        }
    } else {
        fail("enqueue seq=%" PRIu32 " varatlan hiba: %s", seq, esp_err_to_name(err));
    }
}

static void do_drain(void)
{
    sz2_payload_t front;
    esp_err_t err = node_buffer_peek_oldest(&front);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (must_survive_nonempty()) {
            fail("LOSS: peek ures, pedig (%" PRIu32 "..%" PRIu32 ") tulelo kotelezo",
                 S->ref_floor + 1U, S->ref_next);
        }
        return;
    }
    if (err != ESP_OK) {
        fail("peek varatlan hiba: %s", esp_err_to_name(err));
    }

    if (!sgt(front.seq, S->ref_acked)) {
        fail("RESURRECT: peek seq=%" PRIu32 " <= acked=%" PRIu32, front.seq, S->ref_acked);
    }
    if (!slt(front.seq, S->ref_next)) {
        fail("peek seq=%" PRIu32 " >= ref_next=%" PRIu32, front.seq, S->ref_next);
    }
    sz2_payload_t exp;
    make_payload(front.seq, &exp);
    if (memcmp(&front, &exp, sizeof(exp)) != 0) {
        fail("peek tartalom elter seq=%" PRIu32, front.seq);
    }
    if (must_survive_nonempty() && !sle(front.seq, S->ref_floor + 1U)) {
        fail("peek atugrott tulelot: front=%" PRIu32 " floor=%" PRIu32,
             front.seq, S->ref_floor);
    }

    /* Szimulált exact ACK a frontra (néha batch, a többelemű erase útvonalhoz). */
    uint32_t target = front.seq + (rnd() % 4U);
    if (!slt(target, S->ref_next)) {
        target = S->ref_next - 1U;
    }

    S->inflight_clr = true;
    S->inflight_clr_target = target;
    if (sgt(target, S->ref_floor)) {
        S->ref_floor = target; /* a hívás ELŐTT: cut esetén szürke zóna */
    }
    err = node_buffer_clear_until(target);
    S->inflight_clr = false;

    if (err != ESP_OK) {
        fail("clear_until(%" PRIu32 ") varatlan hiba: %s", target, esp_err_to_name(err));
    }
    if (sgt(target, S->ref_acked)) {
        S->ref_acked = target;
    }
    S->clr_ok++;
}

/* ---------------- egy "boot" (gyerek-processz) ---------------- */
static _Noreturn void child_run(void)
{
    g_rng = S->boot_seed;

    esp_err_t err = node_buffer_init();
    if (err != ESP_OK) {
        /* Ebben a modellben nincs injektált IO-hiba, initnek mindig sikerülnie kell:
         * a fail-closed hibaágakat itt hibaként jelentjük, hogy látszódjon, ha aktiválódnak. */
        fail("node_buffer_init hiba: %s", esp_err_to_name(err));
    }

    /* Validálás vágás nélkül (a hátralevő budget megőrzésével). */
    bool armed = S->cut_armed;
    int64_t budget = S->cut_budget;
    S->cut_armed = false;
    validate_after_init();
    S->cut_armed = armed;
    S->cut_budget = budget;

    uint32_t nops = rnd() % 9U;
    for (uint32_t i = 0; i < nops; i++) {
        if (rnd() % 100U < 55U) {
            do_enqueue();
        } else {
            do_drain();
        }
    }

    S->cut_armed = false;
    S->boots_completed++;
    _exit(SIM_EXIT_OK);
}

/* ---------------- szülő-oldal ---------------- */
static void seed_legacy_u32(const char *key, uint32_t value)
{
    for (int i = 0; i < SIM_MAX_ENTRIES; i++) {
        if (!S->entries[i].used) {
            memset(&S->entries[i], 0, sizeof(S->entries[i]));
            memcpy(S->entries[i].key, key, strlen(key));
            S->entries[i].type = (uint8_t)NVS_TYPE_U32;
            S->entries[i].len = sizeof(uint32_t);
            memcpy(S->entries[i].data, &value, sizeof(value));
            S->entries[i].used = true;
            return;
        }
    }
    fprintf(stderr, "seed_legacy_u32: nincs szabad slot\n");
    exit(2);
}

static uint64_t g_total_boots, g_total_cuts, g_total_api, g_total_enq, g_total_clr;

static int run_one(uint64_t seed, uint64_t boots, uint32_t base_seq)
{
    S = mmap(NULL, sizeof(sim_shared_t), PROT_READ | PROT_WRITE,
             MAP_ANON | MAP_SHARED, -1, 0);
    if (S == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    memset(S, 0, sizeof(*S));
    nvs_sim_attach(S);

    /*
     * Wrap-teszt: a magas kezdő seq-et a PRODUKCIÓS legacy-migrációval építtetjük fel —
     * a szülő csak a régi formátumú "head"/"next"/"count" u32 kulcsokat teszi a flashbe,
     * a state2-t az első boot node_buffer_init-je írja meg (migrate_legacy_state).
     */
    if (base_seq != 0) {
        seed_legacy_u32("head", base_seq);
        seed_legacy_u32("next", base_seq);
        seed_legacy_u32("count", 0);
    }
    S->ref_next  = base_seq;
    S->ref_acked = base_seq - 1U;
    S->ref_floor = base_seq - 1U;

    uint64_t prng = seed ^ 0xD1B54A32D192ED03ULL;
    for (uint64_t boot = 0; boot < boots; boot++) {
        uint64_t r = splitmix64(&prng);
        if ((r % 100U) < 25U) {
            S->cut_armed = false;          /* vágás nélküli boot */
            S->cut_budget = 0;
        } else {
            S->cut_armed = true;
            S->cut_budget = 1 + (int64_t)((r >> 8) % 100U);
        }
        int64_t budget0 = S->cut_budget;
        bool armed0 = S->cut_armed;
        S->boot_seed = splitmix64(&prng);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            munmap(S, sizeof(*S));
            return 2;
        }
        if (pid == 0) {
            child_run(); /* nem tér vissza */
        }

        int st = 0;
        if (waitpid(pid, &st, 0) < 0) {
            perror("waitpid");
            munmap(S, sizeof(*S));
            return 2;
        }
        if (WIFSIGNALED(st)) {
            fprintf(stderr,
                    "CRASH (signal %d, sanitizer?) seed=%llu boot=%llu budget=%lld armed=%d "
                    "base=0x%08" PRIX32 "\n  repro: ./fuzz --seed %llu --boots %llu --base 0x%"
                    PRIX32 "\n",
                    WTERMSIG(st), (unsigned long long)seed, (unsigned long long)boot,
                    (long long)budget0, armed0 ? 1 : 0, base_seq,
                    (unsigned long long)seed, (unsigned long long)boots, base_seq);
            munmap(S, sizeof(*S));
            return 1;
        }
        int code = WEXITSTATUS(st);
        if (code == SIM_EXIT_FAIL) {
            fprintf(stderr,
                    "FAIL seed=%llu boot=%llu budget=%lld armed=%d base=0x%08" PRIX32 "\n"
                    "  %s\n"
                    "  repro: ./fuzz --seed %llu --boots %llu --base 0x%" PRIX32 "\n",
                    (unsigned long long)seed, (unsigned long long)boot,
                    (long long)budget0, armed0 ? 1 : 0, base_seq, S->fail_msg,
                    (unsigned long long)seed, (unsigned long long)boots, base_seq);
            munmap(S, sizeof(*S));
            return 1;
        }
        if (code != SIM_EXIT_OK && code != SIM_EXIT_CUT) {
            fprintf(stderr, "varatlan gyerek exit=%d seed=%llu boot=%llu\n",
                    code, (unsigned long long)seed, (unsigned long long)boot);
            munmap(S, sizeof(*S));
            return 1;
        }
    }

    printf("PASS seed=%llu base=0x%08" PRIX32 " boots=%llu (teljes=%llu, cut=%llu) "
           "api_calls=%llu enq_ok=%llu clr_ok=%llu vegallapot[next=%" PRIu32
           " acked=%" PRIu32 " floor=%" PRIu32 "]\n",
           (unsigned long long)seed, base_seq, (unsigned long long)boots,
           (unsigned long long)S->boots_completed, (unsigned long long)S->cuts_total,
           (unsigned long long)S->api_calls_total, (unsigned long long)S->enq_ok,
           (unsigned long long)S->clr_ok, S->ref_next, S->ref_acked, S->ref_floor);
    fflush(stdout);

    g_total_boots += boots;
    g_total_cuts  += S->cuts_total;
    g_total_api   += S->api_calls_total;
    g_total_enq   += S->enq_ok;
    g_total_clr   += S->clr_ok;

    munmap(S, sizeof(*S));
    S = NULL;
    return 0;
}

int main(int argc, char **argv)
{
    g_sim_log_enabled = (getenv("SIM_LOG") != NULL);

    uint64_t seed = 0, boots = 8000;
    uint32_t base = 0;
    bool have_seed = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
            have_seed = true;
        } else if (strcmp(argv[i], "--boots") == 0 && i + 1 < argc) {
            boots = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            base = (uint32_t)strtoull(argv[++i], NULL, 0);
        } else {
            fprintf(stderr,
                    "hasznalat: %s [--seed N [--boots M] [--base HEX]]\n"
                    "  arg nelkul: teljes suite (5 futas x 6000 boot, base=0 es wrap-base)\n",
                    argv[0]);
            return 2;
        }
    }

    if (have_seed) {
        return run_one(seed, boots, base);
    }

    static const struct { uint64_t seed; uint32_t base; } runs[] = {
        { 1, 0 },
        { 2, 0 },
        { 3, 0 },
        { 11, 0xFFFFFF00U },  /* wrap-teszt: seq atfordul 0xFFFFFFFF -> 0 kozben */
        { 12, 0xFFFFFF00U },
    };
    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        int rc = run_one(runs[i].seed, 6000, runs[i].base);
        if (rc != 0) {
            return rc;
        }
    }
    printf("OSSZESEN: boots=%llu cuts=%llu api_calls=%llu enq_ok=%llu clr_ok=%llu — "
           "invarians-sertes nincs\n",
           (unsigned long long)g_total_boots, (unsigned long long)g_total_cuts,
           (unsigned long long)g_total_api, (unsigned long long)g_total_enq,
           (unsigned long long)g_total_clr);
    return 0;
}

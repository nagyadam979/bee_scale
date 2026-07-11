/*
 * sim_shared.h — a fuzz-vezérlő (szülő) és a "boot" gyerek-processzek közös,
 * MAP_SHARED memóriában élő állapota (node_queue_sim tesztpad).
 *
 * Modell:
 *   - entries[]  = a szimulált NVS partíció ("flash"): kulcs->typed blob map.
 *                  Túléli a gyerek exitjét ("reset"), mert megosztott memória.
 *   - cut_*      = áramszünet-injektálás: minden mock NVS API hívás fogyaszt a
 *                  cut_budget-ből; 0-nál a gyerek _exit(SIM_EXIT_CUT)-tal "elszáll".
 *                  Mutáló műveletnél (set/erase) a tick a művelet ELŐTT és UTÁN is
 *                  megtörténik -> a vágás vagy a teljes művelet előtt, vagy után üt;
 *                  fél-kész bejegyzés nincs (a valós NVS item-szinten CRC-vel atomi).
 *   - ref_*      = referencia-modell (orákulum), lásd fuzz_main.c.
 */
#ifndef SIM_SHARED_H
#define SIM_SHARED_H

#include <stdbool.h>
#include <stdint.h>

#define SIM_MAX_ENTRIES 1400   /* 512 rekord + state2 + orphan + legacy kulcsok + bő tartalék */
#define SIM_KEY_SIZE    16     /* == NVS_KEY_NAME_MAX_SIZE */
#define SIM_DATA_MAX    64     /* legnagyobb tárolt blob: sz2_payload_t (41 B) */

/* Gyerek-processz exit kódok. */
#define SIM_EXIT_OK   0        /* boot ciklus hiba és vágás nélkül végigfutott */
#define SIM_EXIT_CUT  42       /* injektált áramszünet ("reset") */
#define SIM_EXIT_FAIL 43       /* invariáns-sértés — fail_msg a részletekkel */

typedef struct {
    bool     used;
    uint8_t  type;                 /* nvs_type_t érték */
    char     key[SIM_KEY_SIZE];    /* NUL-lal padolt */
    uint16_t len;
    uint8_t  data[SIM_DATA_MAX];
} sim_entry_t;

typedef struct {
    /* --- szimulált flash --- */
    sim_entry_t entries[SIM_MAX_ENTRIES];

    /* --- áramszünet-injektálás --- */
    bool     cut_armed;
    int64_t  cut_budget;           /* hátralevő API-hívás a vágásig */
    uint64_t api_calls_total;
    uint64_t cuts_total;

    /* --- referencia-modell (orákulum) ---
     * ref_next : a következő seq, amit a producer enqueue-olni fog. Csak akkor lép,
     *            ha az enqueue ESP_OK-t adott, VAGY a cut-olt enqueue rekordja
     *            bizonyítottan a flashben van (validáláskor feloldva).
     * ref_acked: az utolsó BEFEJEZETT (ESP_OK) clear_until célja. seq <= ref_acked
     *            soha többé nem térhet vissza (RESURRECT = FAIL).
     * ref_floor: túlélési padló. seq > ref_floor && seq < ref_next KÖTELEZŐEN
     *            kiolvasható (LOSS = FAIL). A clear_until HÍVÁSA ELŐTT emeljük
     *            (cut esetén a célzott rekordok "szürke zónába" kerülnek: lehetnek
     *            meg vagy hiányozhatnak), sikeres init utáni validálásnál pedig
     *            head-1-re állítjuk (ami a sorban van, annak túl kell élnie).
     */
    uint32_t ref_next;
    uint32_t ref_acked;
    uint32_t ref_floor;
    bool     inflight_enq;         /* enqueue hívás közben ütött vágás jelölője */
    uint32_t inflight_enq_seq;
    bool     inflight_clr;         /* clear_until közben ütött vágás jelölője */
    uint32_t inflight_clr_target;

    /* --- statisztika + hibariport --- */
    uint64_t enq_ok;
    uint64_t clr_ok;
    uint64_t boots_completed;      /* vágás nélkül végigfutott bootok */
    uint64_t boot_seed;            /* a szülő írja fork előtt; a gyerek PRNG-je */
    char     fail_msg[512];
} sim_shared_t;

#ifdef __cplusplus
extern "C" {
#endif

/* A mock NVS backend rákötése a megosztott memóriára (fork ELŐTT hívandó). */
void nvs_sim_attach(sim_shared_t *shm);

/* A mock logkapcsolója; a fuzz vezérlő SIM_LOG környezeti változóval állítja. */
extern int g_sim_log_enabled;

#ifdef __cplusplus
}
#endif

#endif /* SIM_SHARED_H */

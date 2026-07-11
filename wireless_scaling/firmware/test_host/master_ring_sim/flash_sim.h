/*
 * flash_sim.h — közös shared-memory modell a power-loss fuzz tesztpadhoz.
 *
 * A szülő (fuzz_main) mmap(MAP_SHARED|MAP_ANON)-nal hozza létre a fsim_shared_t-t,
 * minden "boot" egy fork()-olt gyerek. A NOR-flash tömb, az NVS kulcs-érték tár,
 * a cut-számláló és a művelet-journal a shared memóriában él; a firmware (master.c)
 * static/global RAM-állapota a fork-nál másolódik, így bootonként tisztán indul.
 */
#ifndef FLASH_SIM_H
#define FLASH_SIM_H

#include <stddef.h>
#include <stdint.h>

#define FSIM_FLASH_SIZE   0x260000u              /* backup partíció a partitions.csv-ből */
#define FSIM_SECTOR       4096u
#define FSIM_SLOT         256u
#define FSIM_SLOTS        (FSIM_FLASH_SIZE / FSIM_SLOT)   /* 9728 */

#define FSIM_NVS_MAX      64
#define FSIM_NVS_BLOB_MAX 96

/* Journal: a gyerek ide írja, mit csinált; a szülő ebből vezeti a referencia-modellt. */
typedef struct {
    uint32_t op;      /* JOP_* */
    uint32_t uid;     /* rekord-azonosító (rec.rx_ms) vagy op-specifikus */
    int32_t  result;  /* JRES_STARTED = elkezdve, de cut miatt nem tért vissza */
    uint32_t a, b, c, d; /* op-specifikus mezők (lásd fuzz_main.c) */
} fsim_jentry_t;

enum {
    JOP_BOOT = 1,     /* uid=write_seq, result=backup enabled, a=head b=tail c=pending d=slot_count */
    JOP_ENQ,          /* uid=rekord uid, a=rec.type, result=backup_enqueue visszatérés */
    JOP_PEEK,         /* uid=kiadott rekord uid (ha result==1), result=backup_peek */
    JOP_POP,          /* uid=a megelőző sikeres peek uid-ja, result=backup_pop */
    JOP_CAP,          /* uid=sikeres enqueue-k száma üres ringtől a tele-refusalig */
    JOP_DRAINED,      /* teljes ürítés vége; a=maradék pending (0 kell legyen) */
    JOP_DONE,         /* a boot ütemezett op-sora hiánytalanul lefutott */
};

#define JRES_STARTED  INT32_MIN

/* Journal-kapacitás: teljes ring drain (peek+pop) + fill + fuzz-tartalék. */
#define FSIM_JMAX     (4u * FSIM_SLOTS + 8192u)

typedef struct {
    char     ns[16];
    char     key[16];
    uint8_t  used;
    uint8_t  type;    /* 1=u32, 2=blob */
    uint32_t u32;
    uint32_t blob_len;
    uint8_t  blob[FSIM_NVS_BLOB_MAX];
} fsim_nvs_entry_t;

typedef struct {
    /* --- NOR flash (backup partíció) --- */
    uint8_t flash[FSIM_FLASH_SIZE];

    /* --- NVS tartós tár --- */
    fsim_nvs_entry_t nvs[FSIM_NVS_MAX];

    /* --- áramszünet-injektálás ---
     * cut_countdown: hány DURABLE MUTÁCIÓ (esp_partition_write / erase_range /
     * nvs_commit) után üssön a cut. -1 = ebben a bootban nincs cut.
     * Olvasások nem számítanak: olvasás közbeni áramszünet ekvivalens a következő
     * mutáció előttivel (nem változtat tartós állapotot). */
    int64_t  cut_countdown;
    uint64_t rng;              /* torn-write/erase véletlen (bootonként a szülő seedeli) */
    uint32_t cuts_fired;

    /* --- számlálók / firmware-bug jelzők --- */
    uint64_t mutation_ops;
    uint64_t read_ops;
    uint32_t program_violations;   /* 0->1 bit-programozási kísérlet nem törölt cellába */
    uint32_t violation_first_off;
    uint32_t oob_accesses;         /* partíción kívüli hozzáférés */
    uint32_t erase_misaligned;

    /* --- journal --- */
    uint32_t jcount;
    fsim_jentry_t journal[FSIM_JMAX];

    /* --- fuzz-vezérlés --- */
    uint32_t next_uid;         /* globálisan egyedi rekord-uid (bootokon át) */
    uint32_t resend_valid;     /* szülő állítja: cut-olt node-enqueue újraküldendő */
    uint32_t resend_uid;
} fsim_shared_t;

extern fsim_shared_t *g_fsim;

/* gyerek-exit kódok */
#define FSIM_EXIT_OK        0
#define FSIM_EXIT_CUT       42
#define FSIM_EXIT_STUCK     95   /* drain nem ér véget */
#define FSIM_EXIT_DISABLED  96   /* backup_init letiltotta a backlogot */
#define FSIM_EXIT_JFULL     97   /* journal megtelt (harness-hiba) */

void fsim_attach(fsim_shared_t *s);
/* Mutációnként hívandó; 1-et ad, ha ezen a műveleten üt a cut. */
int  fsim_mutation_should_cut(void);
void fsim_die_cut(void) __attribute__((noreturn));
uint64_t fsim_rand(void);

extern int g_mock_log_enabled;
void fsim_fail_next_read(void);

#endif /* FLASH_SIM_H */

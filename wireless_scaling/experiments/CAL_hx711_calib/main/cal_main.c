/*
 * cal_main.c — HX711 mérleg-cella KALIBRÁCIÓS firmware (NODE-variáns).
 * bee_scale / wireless_scaling · 2. fázis · experiments/CAL_hx711_calib
 *
 * CÉL: egyetlen firmware, runtime-vezérelt, a felhő-oldali kalibrációhoz szükséges nyers
 * adat kinyerésére. A prod flottát NEM cseréljük; a kalibráció idejére ezt flasheljük, majd
 * a prod firmware visszakerül.
 *
 * KÉT MÓD (nem build-flag, hanem runtime):
 *   1) SÚLY-MÓD (bench, USB, élő): a `w` parancs folyamatosan streameli a raw-mediánt + scale_c-t.
 *      Ismert súlyokat pakolsz, a PC-oldali script rögzíti -> zero_count + count_per_kg illesztés.
 *   2) HŐ-MÓD (off-grid, akkuról): a `g` parancs (vagy 10 s néma boot -> AUTOSTART) periodikusan
 *      mér és a `callog` NYERS partícióba appendel. Hűtő/nap közti hőpontokra viszed, majd USB-n
 *      visszadugva `d` (DUMP) kiolvasod -> tempCoef (count/°C) illesztés cellánként.
 *
 * SOROS PROTOKOLL (minden gépi sor egy sor; a `#`-kezdetűek emberi/komment):
 *   Súly-stream:   `WGT <raw_median> <scale_c|nan>`
 *   Hő-log élő:    `TLG <idx> <raw> <scale_c|nan>`
 *   Dump rekord:   `REC <idx> <raw> <scale_c|nan>`
 *   Dump vége:     `# DUMP END n=<count>`
 *   Státusz:       `# STATUS records=<n> capacity=<max> free=<max-n>`
 *   Erase kérdés:  `# ERASE? send Y`  ->  `# ERASED`  vagy  `# ERASE ABORTED`
 *   Log tele:      `# LOG FULL`
 *   Menü/prompt:   `#`-kezdetű sorok.
 *
 * NVS helyett NYERS partíció (callog): szándékos eltérés a spec „NVS namespace" javaslatától.
 * Indok: a tápot AKKU KIHÚZÁSÁVAL szüntetjük meg, ezért PER-REKORD tartósság kell (RAM-buffer +
 * ritka NVS-flush esetén az utolsó egyensúlyi pontok elveszhetnének). A fix 16 B rekordos
 * esp_partition append-log per-rekord durable, nagy kapacitású és triviálisan dumpolható.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "esp_log.h"

/* A USB-Serial-JTAG VFS-routing header IDF-verziónként más helyen van. */
#if __has_include("driver/usb_serial_jtag_vfs.h")
#  include "driver/usb_serial_jtag_vfs.h"
#  define CAL_VFS_USE_DRIVER usb_serial_jtag_vfs_use_driver
#else
#  include "esp_vfs_usb_serial_jtag.h"
#  define CAL_VFS_USE_DRIVER esp_vfs_usb_serial_jtag_use_driver
#endif

#include "cal_config.h"
#include "sens_hx711.h"
#include "sens_ds18b20.h"

/* ===================== NYERS log-partíció (callog) ===================== */

#define CAL_REC_MAGIC   0xCA10CA10U
#define CAL_LOG_SUBTYPE 0x40

typedef struct __attribute__((packed)) {
    uint32_t magic;     /* CAL_REC_MAGIC = érvényes rekord; 0xFFFFFFFF = üres (log vége) */
    int32_t  raw;       /* HX711 nyers medián (előjeles 24-bit -> 32-bit) */
    float    scale_c;   /* DS18B20 °C, vagy NaN */
    uint32_t seq;       /* növekvő sorszám (0-tól) */
} cal_rec_t;            /* 16 B */

_Static_assert(sizeof(cal_rec_t) == 16, "cal_rec_t must be 16 bytes");

static const esp_partition_t *s_log_part = NULL;
static uint32_t s_log_count = 0;        /* eddig eltárolt rekordok száma (= write-kurzor) */
static uint32_t s_log_capacity = 0;     /* férőhely rekordban */

static bool log_slot_is_erased(uint32_t idx)
{
    cal_rec_t r;
    if (esp_partition_read(s_log_part, (size_t)idx * sizeof(cal_rec_t), &r, sizeof(r)) != ESP_OK) {
        return false;
    }
    return (r.magic == 0xFFFFFFFFU);
}

static void log_erase_all(void)
{
    esp_partition_erase_range(s_log_part, 0, s_log_part->size);
    s_log_count = 0;
}

/* Boot-scan: megkeresi az első nem-érvényes slotot (= write-kurzor / count).
 * Ha a kurzornál lévő slot „piszkos" (nem 0xFF, hanem szemét), a partíciót auto-törli. */
static void log_init(void)
{
    s_log_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          (esp_partition_subtype_t)CAL_LOG_SUBTYPE, "callog");
    if (!s_log_part) {
        printf("# ERROR callog partition not found\n");
        s_log_capacity = 0;
        s_log_count = 0;
        return;
    }
    s_log_capacity = s_log_part->size / sizeof(cal_rec_t);

    uint32_t i = 0;
    cal_rec_t r;
    for (; i < s_log_capacity; i++) {
        if (esp_partition_read(s_log_part, (size_t)i * sizeof(cal_rec_t), &r, sizeof(r)) != ESP_OK) {
            break;
        }
        if (r.magic != CAL_REC_MAGIC) {
            break;
        }
    }
    s_log_count = i;

    /* Ha a kurzornál nem tiszta (0xFF) a flash (újrahasznált chip szemét), takarítsunk. */
    if (s_log_count < s_log_capacity && !log_slot_is_erased(s_log_count)) {
        printf("# LOG AUTO-ERASED (dirty flash at cursor)\n");
        log_erase_all();
    }
}

/* Egy rekord appendelése. false = a log tele. */
static bool log_append(int32_t raw, float scale_c)
{
    if (!s_log_part || s_log_count >= s_log_capacity) {
        return false;
    }
    cal_rec_t r = { .magic = CAL_REC_MAGIC, .raw = raw, .scale_c = scale_c, .seq = s_log_count };
    esp_err_t err = esp_partition_write(s_log_part, (size_t)s_log_count * sizeof(cal_rec_t),
                                        &r, sizeof(r));
    if (err != ESP_OK) {
        printf("# ERROR log write err=0x%x at idx=%" PRIu32 "\n", (int)err, s_log_count);
        return false;
    }
    s_log_count++;
    return true;
}

static void log_dump(void)
{
    for (uint32_t i = 0; i < s_log_count; i++) {
        cal_rec_t r;
        if (esp_partition_read(s_log_part, (size_t)i * sizeof(cal_rec_t), &r, sizeof(r)) != ESP_OK) {
            printf("# ERROR read idx=%" PRIu32 "\n", i);
            continue;
        }
        printf("REC %" PRIu32 " %" PRId32 " %.4f\n", r.seq, r.raw, (double)r.scale_c);
    }
    printf("# DUMP END n=%" PRIu32 "\n", s_log_count);
}

static void log_status(void)
{
    printf("# STATUS records=%" PRIu32 " capacity=%" PRIu32 " free=%" PRIu32 "\n",
           s_log_count, s_log_capacity, s_log_capacity - s_log_count);
}

/* ===================== I/O (USB-Serial-JTAG, non-blocking stdin) ===================== */

static void io_init(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&ucfg);
    CAL_VFS_USE_DRIVER();
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);
}

/* Egy karakter nem-blokkoló olvasása; -1 = nincs adat. A '\r','\n',' ' figyelmen kívül hagyva. */
static int io_getcmd(void)
{
    unsigned char c;
    int n = read(fileno(stdin), &c, 1);
    if (n != 1) {
        return -1;
    }
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
        return -1;
    }
    return (int)c;
}

/* Blokkoló(-ish) karakter-várás timeout_ms-ig (erase-megerősítéshez). -1 = timeout. */
static int io_getcmd_wait(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int c = io_getcmd();
        if (c >= 0) {
            return c;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}

/* ===================== Mérés-segédek ===================== */

/* Egy raw-medián olvasás (power_up -> median -> power_down). */
static int32_t measure_raw(void)
{
    hx711_power_up();
    float med = hx711_read_weight();   /* HX711_MEDIAN_SAMPLES-ből medián (float, egész count) */
    hx711_power_down();
    if (isnan(med)) {
        return 0;
    }
    return (int32_t)llroundf(med);
}

/* scale_c olvasás; hibán NaN. */
static float measure_scale_c(void)
{
    float t = ds18b20_read_temp();
    if (ds18b20_last_read_had_error()) {
        return NAN;
    }
    return t;
}

static void led_pulse(void)
{
    gpio_set_level(USER_LED_GPIO, 0);   /* aktív-alacsony: BE */
    vTaskDelay(pdMS_TO_TICKS(40));
    gpio_set_level(USER_LED_GPIO, 1);   /* KI */
}

/* ===================== Módok ===================== */

static void print_menu(void)
{
    printf("# --- CAL_NODE menu ---\n");
    printf("#  w = weight-stream (WGT raw scale_c),  x = stop\n");
    printf("#  g = start temp-log (append to callog)\n");
    printf("#  d = dump log,  s = status,  e = erase log,  ? = menu\n");
    printf("# READY\n");
}

/* SÚLY-STREAM: folyamatos WGT sorok, amíg 'x' nem jön. */
static void mode_weight_stream(void)
{
    printf("# WSTREAM start (x=stop)\n");
    int64_t last_temp_us = 0;
    float scale_c = NAN;
    while (1) {
        int c = io_getcmd();
        if (c == 'x') {
            break;
        }
        int64_t now = esp_timer_get_time();
        if (last_temp_us == 0 || (now - last_temp_us) >= (int64_t)CAL_WSTREAM_TEMP_REFRESH_MS * 1000) {
            scale_c = measure_scale_c();
            last_temp_us = now;
        }
        int32_t raw = measure_raw();
        printf("WGT %" PRId32 " %.4f\n", raw, (double)scale_c);
        vTaskDelay(pdMS_TO_TICKS(CAL_WSTREAM_INTERVAL_MS));
    }
    printf("# WSTREAM stop\n");
}

/* Interaktív parancs végrehajtása. Visszatérési érték: a KÖVETKEZŐ mód jelzése.
 *   0 = maradj a hívó módban, 1 = váltás súly-streamre, 2 = váltás hő-logra, 3 = stop->menu. */
static int handle_cmd(int c)
{
    switch (c) {
    case 'w': return 1;
    case 'g': return 2;
    case 'x': return 3;
    case 'd': log_dump();   return 0;
    case 's': log_status(); return 0;
    case 'e': {
        printf("# ERASE? send Y\n");
        int y = io_getcmd_wait(5000);
        if (y == 'Y') {
            log_erase_all();
            printf("# ERASED\n");
        } else {
            printf("# ERASE ABORTED\n");
        }
        return 0;
    }
    case '?':
    case 'h': print_menu(); return 0;
    default:  return 0;
    }
}

/* HŐ-LOG: ~CAL_TEMP_LOG_INTERVAL_MS-enként append + élő TLG sor. Közben stdin-t figyel
 * (d/s/e inline; x -> vissza menübe; w -> súly-stream). */
static void mode_temp_log(void)
{
    printf("# TLOG start (interval=%d ms, x=stop)\n", CAL_TEMP_LOG_INTERVAL_MS);
    int64_t next_us = 0;   /* azonnal mér az első körben */
    bool full_reported = false;
    while (1) {
        int c = io_getcmd();
        if (c >= 0) {
            int action = handle_cmd(c);
            if (action == 1) { mode_weight_stream(); printf("# TLOG resume\n"); }
            else if (action == 3) { break; }
            /* action==2 (g) hő-logban értelmetlen -> marad; 0 -> inline kész */
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_us) {
            float scale_c = measure_scale_c();
            int32_t raw = measure_raw();
            if (log_append(raw, scale_c)) {
                led_pulse();
                printf("TLG %" PRIu32 " %" PRId32 " %.4f\n", s_log_count - 1, raw, (double)scale_c);
                full_reported = false;
            } else if (!full_reported) {
                printf("# LOG FULL\n");
                full_reported = true;
            }
            next_us = now + (int64_t)CAL_TEMP_LOG_INTERVAL_MS * 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    printf("# TLOG stop\n");
}

/* ===================== app_main ===================== */

void app_main(void)
{
    /* A rutin szenzor-INFO logokat elnémítjuk, hogy ne spam-eljék a protokoll-sorokat. */
    esp_log_level_set("sens_hx711", ESP_LOG_ERROR);
    esp_log_level_set("sens_ds18b20", ESP_LOG_ERROR);

    io_init();

    /* LED kimenet, alap KI (aktív-alacsony). */
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << USER_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    gpio_set_level(USER_LED_GPIO, 1);

    hx711_init();
    ds18b20_init();
    log_init();

    printf("\n# CAL_NODE build=%s %s log_records=%" PRIu32 " capacity=%" PRIu32 "\n",
           __DATE__, __TIME__, s_log_count, s_log_capacity);
    print_menu();

    /* Boot figyelő-ablak: parancs -> INTERAKTÍV; néma -> AUTOSTART hő-log. */
    int first = io_getcmd_wait(CAL_BOOT_LISTEN_MS);
    if (first < 0) {
        printf("# AUTOSTART temp-log (no host)\n");
        mode_temp_log();
    } else {
        int action = handle_cmd(first);
        if (action == 1) mode_weight_stream();
        else if (action == 2) mode_temp_log();
    }

    /* Interaktív fő-hurok (a módokból ide térünk vissza). */
    while (1) {
        int c = io_getcmd();
        if (c >= 0) {
            int action = handle_cmd(c);
            if (action == 1) { mode_weight_stream(); print_menu(); }
            else if (action == 2) { mode_temp_log(); print_menu(); }
            /* action==3 (x) a menüben no-op */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

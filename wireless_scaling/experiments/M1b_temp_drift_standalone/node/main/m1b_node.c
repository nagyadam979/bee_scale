/*
 * M1b — STANDALONE off-grid HŐ-DRIFT mérő · NODE (ESP32-C3 SuperMini)
 * bee_scale / wireless_scaling · 2. fázis · experiments/M1b_temp_drift_standalone/node
 *
 * ============================================================================
 * CÉL / MIT MÉRÜNK
 * ----------------------------------------------------------------------------
 *   Egy ESP32-C3 SuperMini node-ot nagy power bankról kiviszünk hűtőbe / napra —
 *   se master, se PC, se rádió. Minden deep-sleep ébredéskor MÉR és PERZISZTENSEN
 *   (AT24C32 EEPROM-ba) LOGOL:
 *     (1) a saját belső RC slow-clock óra-DRIFTjét egy DS3231 hőkompenzált (±2 ppm)
 *         referenciához  -> drift = (várt cycle*T) vs. a DS3231-ből olvasott valós idő;
 *     (2) a DS3231 saját hőszenzorát (ds_c) és a C3 belső die-hőjét (die_c) — a drift
 *         hőfüggésének jellemzéséhez;
 *     (3) a mérőcella + HX711 NYERS 24-bit olvasatát (hx) — konstans terhelés alatt,
 *         a HX711 hő-drift kiméréséhez.
 *   NINCS RÁDIÓ. A wake-forrás a C3 SAJÁT deep-sleep timere (= a VIZSGÁLT óra). A DS3231
 *   alarmot NEM használjuk ébresztésre — csak a valós idő referenciájához olvassuk.
 *
 * ============================================================================
 * MUNKAFOLYAMAT (a felhasználónak — flashelés -> mérés -> dump)
 * ----------------------------------------------------------------------------
 *  ELŐKÉSZÍTÉS — DS3231 idő beállítása (EGYSZER, ha az RTC üres / 2000-es év):
 *    1) Állítsd M1B_SET_RTC_ON_BOOT = 1 -re és írd be a M1B_RTC_SET_EPOCH-ba az AKTUÁLIS
 *       UTC epoch-másodpercet (pl. `date -u +%s` a flashelés pillanatában, + a flash/boot
 *       késleltetésre adj pár s-ot). Flashelj. A node boot-kor beírja a DS3231-be ezt az
 *       időt (UTC), majd a normál menetbe lép. (A drift relatív, de abszolút epoch jobb.)
 *    2) Állítsd vissza M1B_SET_RTC_ON_BOOT = 0 -ra (különben MINDEN boot felülírná az időt!).
 *
 *  ÚJ MÉRÉS INDÍTÁSA — EXPLICIT, compile-flaggel (robusztus, jumper-mentes):
 *    3) Állítsd M1B_ERASE_AND_START = 1 -re. Flashelj. A node az EEPROM-fejlécet
 *       write_index=0 -ra inicializálja (a régi logot ELDOBJA), kiír egy "ERASE+START"
 *       INFO-sort, majd AZONNAL belép a mérés-ciklusba (mér 1 rekordot -> alszik T-t).
 *    4) Állítsd vissza M1B_ERASE_AND_START = 0 -ra és flashelj ÚJRA (a "normál", dump-képes
 *       build). Innentől minden timer-wake mér+logol; minden cold boot (PC-re dugva) DUMP-ol.
 *       FIGYELEM: a 0-ról induló flash közben a board tápot kap -> ha az EEPROM-fejléc érvényes,
 *       a normál build cold boot-ja DUMP-ol és NEM töröl, tehát a 0. rekord(ok) megmaradnak.
 *    5) Húzd ki a PC-ből, csatlakoztasd a power bankot, vidd ki (hűtő/nap). A node off-grid
 *       méri+logolja a rekordokat (timer-wake-enként 1), sosem dumpol és sosem töröl.
 *
 *  DUMP (visszahozva, PC-re dugva):
 *    6) Dugd a boardot a PC-be (teljes power-on/reset = NEM timer-wake) -> a firmware
 *       DUMP-módba megy: kiírja a "M1B-HDR ..." fejléc-sort + minden rekordot ("M1B idx=..."),
 *       majd TARTÓS idle-ben vár (NEM ír, NEM töröl). Olvasd: pio device monitor (vagy a
 *       log_master.py). A logot a dump SOHA nem írja felül.
 *
 *  Összefoglalva a hibatűrő logika:
 *    - timer-wake (off-grid) -> MÉR + LOGOL, sosem dumpol, sosem töröl.
 *    - cold boot (PC, nem timer-wake) -> DUMP, sosem töröl ... KIVÉVE
 *    - M1B_ERASE_AND_START==1 (explicit build) -> EGYSZER töröl (write_index=0) + mérés-start.
 *
 * ============================================================================
 * BEFAGYASZTOTT KONTRAKTUS (a dump SZÓ SZERINT ezt írja — a Python-regex erre épül)
 * ----------------------------------------------------------------------------
 *   Fejléc (PONTOSAN egy sor a dump elején):
 *     M1B-HDR records=%u period_s=%u start_unix=%lld
 *       records    = a kidumpolt érvényes rekordok száma
 *       period_s   = a névleges ébredési periódus T (M1B_WAKE_PERIOD_SEC)
 *       start_unix = az ELSŐ rekord ds_unix-e (mérés-kezdet epoch-másodperc)
 *   Rekordonként (PONTOSAN egy sor):
 *     M1B idx=%u cycle=%u ds_unix=%lld ds_c=%.2f die_c=%.2f hx=%ld awake_ms=%u
 *       idx      = EEPROM-rekordindex (0-tól)
 *       cycle    = C3 RTC deep-sleep ébredés-számláló az adott rekordhoz
 *       ds_unix  = DS3231 epoch-másodperc (%lld = int64_t)
 *       ds_c     = DS3231 hőszenzor °C (%.2f; hiba -> nan)
 *       die_c    = C3 belső die-hő °C (%.2f; hiba -> nan)
 *       hx       = HX711 nyers előjeles 24-bit, int32-re kiterjesztve (%ld)
 *       awake_ms = az ÉBREN töltött mérés-idő (boot -> a rekord-szerializálásig, esp_timer ms).
 *                  KRITIKUS a drift-pontossághoz: a ds_unix-különbség minden ciklusban
 *                  = awake + period×(1+drift); a Python ezt LEVONJA, hogy a tiszta ALVÁS-
 *                  drift maradjon (különben a ~1,4 s awake 60 s periódusnál ~2,3% műdriftet ad).
 *   A printf-formátumok PONTOSAK. nan-t a "%.2f" newlib-en "nan"-ként ír — OK.
 *
 * ============================================================================
 * HARDVER / BEKÖTÉS (tények — webről ellenőrzött I2C API, NEM emlékezetből)
 * ----------------------------------------------------------------------------
 *   ESP32-C3 SuperMini. Kivezetett GPIO: 0-10, 20, 21. Strapping (kerülendő): 2/8/9.
 *   USB-Serial-JTAG (konzol): 18/19. UART0: 20/21.
 *
 *   HX711 + mérőcella (ÉLES BEKÖTÉS — casing-routing miatt átkötve; felhasználó igazolta,
 *   hogy így fér be minden elektronika a nyomtatott házba. Korábban GPIO5/6, lásd H1/H2):
 *     PD_SCK = GPIO4  (HIGH = power-down, LOW = power-up)   [korábban GPIO5]
 *     DOUT   = GPIO3  (adat)                                 [korábban GPIO6]
 *     25 SCK-pulzus = 24-bit, A-csatorna / gain 128. Power-up után ~400 ms settle,
 *     első pár mintát eldobni, N átlag. Sweep alatt KONSTANS terhelés.
 *
 *   ZS-042 modul EGY I2C buszon (modul 3,3V-ról):
 *     DS3231  @0x68  (±2 ppm hőkompenzált referencia + saját hőszenzor ±0,25°C)
 *     AT24C32 @0x57  (4 KB = 32 kbit, perzisztens — túléli a tápelvételt)
 *     I2C pinek: SDA = GPIO5, SCL = GPIO6  — FIGYELEM: NEM VÉGLEGES. A HX711 4/3-ra kötése
 *                elvette az I2C korábbi 4/3 lábait, az új I2C-hely viszont MÉG NINCS ELDÖNTVE.
 *                Az itteni 5/6 csak IDEIGLENES, ütközésmentes placeholder (a HX711 által
 *                felszabadított lábak), hogy a build érvényes maradjon és ne ütközzön a
 *                HX711 4/3-mal. A tényleges I2C-kiosztást a node végleges bekötésekor kell
 *                meghatározni (NEM 3/4=HX711, NEM 18/19=USB, NEM 20/21=UART, NEM 2/8/9=strapping).
 *                400 kHz.
 *
 *   Táp: power bank a SuperMini USB-C-jén -> on-board LDO -> 3,3V.
 *
 *   I2C API: IDF 5.5 ÚJ driver/i2c_master.h (i2c_new_master_bus, i2c_master_bus_add_device
 *     külön device-handle a 0x68-ra és a 0x57-re ugyanazon a buszon, i2c_master_transmit /
 *     _receive / _transmit_receive). scl_speed_hz = 400000.
 *
 * ============================================================================
 * EEPROM (AT24C32) TÁROLÁS
 * ----------------------------------------------------------------------------
 *   A board PC-hez dugva teljes tápciklust kap -> az RTC_DATA_ATTR RAM ELVESZNE -> ezért
 *   MINDEN rekordot az EEPROM-ba logolunk. A cycle-t is perzisztáljuk (rekordonként a
 *   record.cycle mezőben), így a dump cycle-mezője a TÉNYLEGES RTC-számlálót tükrözi.
 *
 *   AT24C32 részletek: 2-bájtos szó-cím (big-endian, MSB előbb). 32-bájtos LAP-írás:
 *   egy I2C-tranzakció NEM léphet át lap-határt (különben a lapon belül körbefordul). Írás
 *   után ~5 ms write-cycle. Itt: lap-tudatos darabolt írás (a [cím..cím+len) tartományt a
 *   32-bájtos lap-határoknál vágjuk), és fix 6 ms delay minden chunk után (egyszerű+robusztus,
 *   az ACK-pollingnál konzervatívabb). Olvasásnál nincs lap-határ-korlát (dummy-write a
 *   szó-címre + szekvenciális read), de itt rekordonként olvasunk.
 *
 *   Memória-térkép:
 *     0x0000: HEADER (8 B):  magic u32 (LE)  + write_index u16 (LE) + 2 B tartalék
 *     0x0010: az 1. rekord (REC_BASE = 16; szépen igazítva, nem lóg a header lapjára)
 *   Rekord (M1B_REC_SIZE = 16 B), little-endian:
 *     off 0 : cycle    u16
 *     off 2 : ds_unix  u32   (epoch-másodperc; 2106-ig elég)
 *     off 6 : ds_c_q   i16   (0,25°C-egység: °C * 4)
 *     off 8 : die_c_q  i16   (0,25°C-egység: °C * 4)  — NAN esetén M1B_TEMP_Q_NAN szentinel
 *     off 10: hx       i32   (HX711 nyers előjeles 24-bit, 32-re kiterjesztve)
 *     off 14: awake_ms u16   (ÉBREN-idő ms; a drift awake-korrekciójához — lásd a kontraktust)
 *   Kapacitás: (4096 - REC_BASE) / 16 = (4096-16)/16 = 255 rekord.
 *     -> ~255 rekord; T=60 s mellett ~4,25 óra; T=300 s mellett ~21 óra. (Lásd M1B_MAX_RECORDS.)
 *   A %.2f dumpnál a q-értékből q/4.0 a float; a szentinel -> NAN.
 *
 * ============================================================================
 * DS3231 RÉSZLETEK
 * ----------------------------------------------------------------------------
 *   @0x68. Idő-regiszterek BCD: 0x00 sec, 0x01 min, 0x02 hour(24h), 0x03 dow, 0x04 date,
 *   0x05 month(bit7=century), 0x06 year(00-99). Hő: 0x11 = előjeles int8 °C (egész),
 *   0x12 bit7-6 = törtrész 0,25°C-lépésben -> ds_c = (int8)reg11 + ((reg12>>6)*0.25).
 *   A BCD-időt MANUÁLISAN konvertáljuk UTC epoch-má (days-since-1970; NEM mktime/TZ).
 *   Idő-beállítás: a fenti M1B_SET_RTC_ON_BOOT + M1B_RTC_SET_EPOCH (UTC) -> BCD-re bontva,
 *   0x00-tól visszaírva (a 0x0E control register OSC-engedélyezve marad / EOSC=0).
 *
 * ============================================================================
 * ÉBREDÉSI MENET (minden timer-wakeön)  — lásd app_main()
 * ----------------------------------------------------------------------------
 *   1) RTC magic-guard: timer-wake + magic -> cycle++; különben cold start.
 *      (Cold start a normál buildben = DUMP-mód; az M1B_ERASE_AND_START buildben = erase+start.)
 *   2) DS3231 idő (ds_unix) + hő (ds_c); C3 die-hő (die_c).
 *   3) HX711 power-up (PD_SCK LOW) -> settle -> skip -> N átlag -> power-down + GPIO hold.
 *   4) Rekord EEPROM-ba (write_index helyre), write_index++.
 *   5) Rövid INFO-sor.
 *   6) USB-JTAG pad off -> esp_sleep_enable_timer_wakeup(T) -> esp_deep_sleep_start().
 *
 * HANGOLHATÓ PARAMÉTEREK: a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>            /* NAN, isnan */
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"                 /* esp_rom_delay_us() */
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"           /* IDF 5.5 ÚJ I2C master driver */
#include "driver/temperature_sensor.h"   /* C3 belső die-hőmérséklet */
#include "soc/soc_caps.h"                /* SOC_USB_SERIAL_JTAG_SUPPORTED */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "esp_private/sleep_console.h"   /* sleep_console_usj_pad_backup_and_disable() */
#endif

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* A deep-sleep ébredési periódus T másodpercben (= a dump period_s mezeje). Alap 60 s. */
#define M1B_WAKE_PERIOD_SEC     300

/* --- ÜZEMMÓD-KAPCSOLÓK (lásd a fenti MUNKAFOLYAMAT-blokkot) --- */
/* 1 -> boot-kor beírja a DS3231-be a M1B_RTC_SET_EPOCH időt (UTC), majd normál menet. EGYSZER! */
#define M1B_SET_RTC_ON_BOOT     0
/* UTC epoch-másodperc a DS3231-hez (csak ha M1B_SET_RTC_ON_BOOT==1). Pl. `date -u +%s` + pár s. */
#define M1B_RTC_SET_EPOCH       1751212800LL   /* 2025-06-29 16:00:00 UTC (példa — írd át!) */
/* 1 -> az EEPROM-fejléc write_index=0-ra (régi log eldobva) + AZONNAL mérés-start. EGYSZER! */
#define M1B_ERASE_AND_START     0

/* 1 -> DIAGNOSZTIKAI mód: az app ÉBREN marad (nem alszik, USB-n végig monitorozható), és
 *      ciklikusan: I2C-busz scan + AT24C32 írás/visszaolvasás teszt + DS3231 idő/hő + HX711
 *      nyers. HW-bringup hibakereséshez (cím / write-protect / bekötés). 0 = normál üzem. */
#define M1B_DIAG_MODE           0
/* 1 -> RECOVERY-dump: cold boot-kor a write_index-tol ES a magic-tol FUGGETLENUL kiirja a
 *      TELJES EEPROM-ot (mind a M1B_MAX_RECORDS rekord-helyet). Adatmentes esetre: ha egy
 *      veletlen erase csak a write_index-et nullazta, a regi rekordok FIZIKAILAG meg ott
 *      vannak — ez kiolvassa mind; a Python szuri az ervenyeseket (valos ds_unix). NEM ir,
 *      NEM torol (dump-ag). 0 = normal (write_index szerinti) dump. */
#define M1B_DUMP_ALL            0
/* A diag EEPROM write-teszt CÍME: NEM a 0x0000 fejléc, hanem egy magas, nem használt cím,
 * hogy a teszt ne basszon bele egy esetleges valós logba. */
#define M1B_DIAG_EE_TEST_ADDR   0x0F00

/* --- HX711 mintavétel --- */
#define M1B_HX_SETTLE_MS        400     /* power-up utáni settle (ms) az 1. minta előtt */
#define M1B_HX_SKIP             2       /* az első ennyi mintát eldobjuk (tranziens) */
#define M1B_HX_AVG              8       /* ennyi mintát átlagolunk a rekordhoz */

/* --- I2C busz / eszközcímek --- */
#define M1B_I2C_PORT            (-1)            /* -1 = auto port-választás (IDF 5.5) */
#define M1B_I2C_SDA_GPIO        GPIO_NUM_5   /* ZS-042 SDA — IDEIGLENES placeholder! Végleges I2C-hely MÉG NINCS eldöntve (a volt 4/3-at a HX711 elvette). */
#define M1B_I2C_SCL_GPIO        GPIO_NUM_6   /* ZS-042 SCL — IDEIGLENES placeholder! Végleges I2C-hely MÉG NINCS eldöntve (a volt 4/3-at a HX711 elvette). */
#define M1B_I2C_FREQ_HZ         400000
#define M1B_I2C_TIMEOUT_MS      100
#define DS3231_ADDR             0x68
#define AT24C32_ADDR            0x57

/* --- HX711 GPIO (ÉLES BEKÖTÉS — casing-routing miatt átkötve, felhasználó igazolta; volt 5/6) --- */
#define HX711_PD_SCK_GPIO       GPIO_NUM_4      /* SCK: HIGH = power-down, LOW = power-up (volt GPIO5) */
#define HX711_DOUT_GPIO         GPIO_NUM_3      /* DT/DOUT: adat (volt GPIO6) */

/* --- AT24C32 EEPROM elrendezés --- */
#define AT24C32_PAGE_SIZE       32
#define AT24C32_TOTAL_BYTES     4096
#define AT24C32_WRITE_MS        6               /* write-cycle delay chunkonként (datasheet ~5 ms) */
#define M1B_EE_MAGIC            0xB0EE5A1Eu      /* fejléc érvényesség-jel */
#define M1B_HDR_ADDR            0x0000
#define M1B_REC_BASE            16               /* az 1. rekord címe (a header után, lap-igazítva) */
#define M1B_REC_SIZE            16               /* cycle2 + ds_unix4 + ds_c2 + die_c2 + hx4 + awake2 */
#define M1B_MAX_RECORDS         ((AT24C32_TOTAL_BYTES - M1B_REC_BASE) / M1B_REC_SIZE)  /* = 255 */

/* die_c/ds_c kvantálás: 0,25°C-egység (q = round(°C * 4)). NAN szentinel: i16 min. */
#define M1B_TEMP_Q_NAN          ((int16_t)0x8000)

/* ================================================================= */

static const char *TAG = "M1B";

/* RTC-megőrzött állapot (timer-wakeöt túléli; cold boot -> a magic dönt). */
#define M1B_RTC_MAGIC           0xBEE5C10Cu
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint32_t s_rtc_cycle;

/* I2C handle-ök (busz + két device). */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_ds_dev  = NULL;   /* DS3231 @0x68 */
static i2c_master_dev_handle_t s_ee_dev  = NULL;   /* AT24C32 @0x57 */

/* ---- mért rekord (RAM-beli, kicsomagolt) ---- */
typedef struct {
    uint32_t cycle;
    int64_t  ds_unix;     /* epoch-másodperc; <0 -> érvénytelen idő */
    float    ds_c;        /* DS3231 hő °C (NAN = hiba) */
    float    die_c;       /* C3 die-hő °C (NAN = hiba) */
    int32_t  hx;          /* HX711 nyers */
    uint16_t awake_ms;    /* ÉBREN-idő (boot -> rekord-szerializálás) ms; drift awake-korrekcióhoz */
} m1b_sample_t;

/* ===================== I2C alap ===================== */

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = M1B_I2C_PORT,
        .sda_io_num        = M1B_I2C_SDA_GPIO,
        .scl_io_num        = M1B_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* a ZS-042 modulon van pull-up, de biztos ami biztos */
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t ds_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = DS3231_ADDR,
        .scl_speed_hz    = M1B_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &ds_cfg, &s_ds_dev);
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t ee_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AT24C32_ADDR,
        .scl_speed_hz    = M1B_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &ee_cfg, &s_ee_dev);
}

/* ===================== DS3231 ===================== */

static inline uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static inline uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

/* Naptári UTC -> epoch-másodperc, MANUÁLISAN (days-since-1970; NEM mktime/TZ).
 * year a teljes év (pl. 2025), month 1..12, day 1..31. */
static int64_t ymd_to_epoch(int year, int month, int day, int hour, int min, int sec)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int y = 1970; y < year; y++) {
        bool leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        days += leap ? 366 : 365;
    }
    for (int m = 1; m < month; m++) {
        days += mdays[m - 1];
        if (m == 2) {
            bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
            if (leap) days += 1;
        }
    }
    days += (day - 1);
    return ((days * 24 + hour) * 60 + min) * 60 + sec;
}

/* DS3231 idő olvasása -> epoch (UTC). Hiba / nem-beállított óra esetén <0 jelölhetné, de a
 * 2000-es alap is valós epoch -> a hívó az érvényességet a magic/ds_unix>0-ból dönti. */
static int64_t ds3231_read_unix(void)
{
    uint8_t reg = 0x00;
    uint8_t t[7] = { 0 };
    esp_err_t err = i2c_master_transmit_receive(s_ds_dev, &reg, 1, t, sizeof(t), M1B_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 ido-olvasas hiba: %s", esp_err_to_name(err));
        return -1;
    }
    int sec   = bcd2dec(t[0] & 0x7F);
    int min   = bcd2dec(t[1] & 0x7F);
    int hour  = bcd2dec(t[2] & 0x3F);          /* 24h mód (bit6=0) */
    int day   = bcd2dec(t[4] & 0x3F);
    int month = bcd2dec(t[5] & 0x1F);
    int year  = 2000 + bcd2dec(t[6]);
    if (t[5] & 0x80) {                          /* century bit */
        year += 100;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return -1;
    }
    return ymd_to_epoch(year, month, day, hour, min, sec);
}

/* DS3231 hőszenzor (°C). Hiba -> NAN. */
static float ds3231_read_temp_c(void)
{
    uint8_t reg = 0x11;
    uint8_t r[2] = { 0 };
    if (i2c_master_transmit_receive(s_ds_dev, &reg, 1, r, sizeof(r), M1B_I2C_TIMEOUT_MS) != ESP_OK) {
        return NAN;
    }
    int8_t whole = (int8_t)r[0];
    float frac = (float)(r[1] >> 6) * 0.25f;
    return (float)whole + frac;
}

#if M1B_SET_RTC_ON_BOOT
/* A M1B_RTC_SET_EPOCH (UTC) beírása a DS3231 idő-regisztereibe. EGYSZER, build-flaggel.
 * Epoch -> naptár (UTC) MANUÁLISAN, majd BCD-re bontva 0x00-tól írjuk. EOSC=0 marad (osc be). */
static void ds3231_set_time_from_epoch(int64_t epoch)
{
    int64_t days = epoch / 86400;
    int64_t rem  = epoch % 86400;
    int hour = (int)(rem / 3600);
    int min  = (int)((rem % 3600) / 60);
    int sec  = (int)(rem % 60);

    int year = 1970;
    for (;;) {
        bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        int ylen = leap ? 366 : 365;
        if (days >= ylen) { days -= ylen; year++; } else { break; }
    }
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int month = 1;
    for (int m = 0; m < 12; m++) {
        int dim = mdays[m];
        if (m == 1) {
            bool leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
            if (leap) dim = 29;
        }
        if (days >= dim) { days -= dim; month++; } else { break; }
    }
    int day = (int)days + 1;

    int yy = year - 2000;
    uint8_t century = 0;
    if (yy >= 100) { yy -= 100; century = 0x80; }

    uint8_t buf[8];
    buf[0] = 0x00;                                  /* start reg */
    buf[1] = dec2bcd((uint8_t)sec);
    buf[2] = dec2bcd((uint8_t)min);
    buf[3] = dec2bcd((uint8_t)hour);                /* 24h: bit6=0 */
    buf[4] = 1;                                     /* dow (nem használt; 1=hétfő tetszőleges) */
    buf[5] = dec2bcd((uint8_t)day);
    buf[6] = (uint8_t)(dec2bcd((uint8_t)month) | century);
    buf[7] = dec2bcd((uint8_t)yy);

    esp_err_t err = i2c_master_transmit(s_ds_dev, buf, sizeof(buf), M1B_I2C_TIMEOUT_MS);
    ESP_LOGI(TAG, "DS3231 ido beallitva: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch=%lld) -> %s",
             year, month, day, hour, min, sec, (long long)epoch, esp_err_to_name(err));
}
#endif /* M1B_SET_RTC_ON_BOOT */

/* ===================== AT24C32 EEPROM ===================== */

/* Lap-tudatos írás: a [addr..addr+len) tartományt a 32-bájtos lap-határoknál darabolja,
 * minden chunk után fix write-cycle delay. (Egy I2C-tranzakció nem léphet át lap-határt.) */
static esp_err_t ee_write(uint16_t addr, const uint8_t *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        uint16_t cur   = (uint16_t)(addr + done);
        uint16_t page_end = (uint16_t)((cur / AT24C32_PAGE_SIZE + 1) * AT24C32_PAGE_SIZE);
        size_t   chunk = page_end - cur;
        if (chunk > (len - done)) {
            chunk = len - done;
        }
        uint8_t buf[2 + AT24C32_PAGE_SIZE];
        buf[0] = (uint8_t)(cur >> 8);     /* szó-cím MSB (big-endian) */
        buf[1] = (uint8_t)(cur & 0xFF);   /* szó-cím LSB */
        memcpy(&buf[2], data + done, chunk);
        esp_err_t err = i2c_master_transmit(s_ee_dev, buf, 2 + chunk, M1B_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        esp_rom_delay_us(AT24C32_WRITE_MS * 1000);   /* write-cycle (busy-wait, FreeRTOS előtt is OK) */
        done += chunk;
    }
    return ESP_OK;
}

/* Olvasás: dummy-write a szó-címre (big-endian) + szekvenciális read. Nincs lap-határ-korlát. */
static esp_err_t ee_read(uint16_t addr, uint8_t *data, size_t len)
{
    uint8_t wa[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF) };
    return i2c_master_transmit_receive(s_ee_dev, wa, 2, data, len, M1B_I2C_TIMEOUT_MS);
}

/* Fejléc be/ki. magic(LE u32) + write_index(LE u16) + 2 B tartalék. */
static esp_err_t ee_read_header(uint32_t *magic, uint16_t *widx)
{
    uint8_t h[6] = { 0 };
    esp_err_t err = ee_read(M1B_HDR_ADDR, h, sizeof(h));
    if (err != ESP_OK) {
        return err;
    }
    *magic = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    *widx  = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
    return ESP_OK;
}

static esp_err_t ee_write_header(uint32_t magic, uint16_t widx)
{
    uint8_t h[6];
    h[0] = (uint8_t)(magic & 0xFF);
    h[1] = (uint8_t)((magic >> 8) & 0xFF);
    h[2] = (uint8_t)((magic >> 16) & 0xFF);
    h[3] = (uint8_t)((magic >> 24) & 0xFF);
    h[4] = (uint8_t)(widx & 0xFF);
    h[5] = (uint8_t)((widx >> 8) & 0xFF);
    return ee_write(M1B_HDR_ADDR, h, sizeof(h));
}

/* float °C -> i16 q (0,25°C). NAN -> szentinel. */
static int16_t temp_to_q(float c)
{
    if (isnan(c)) {
        return M1B_TEMP_Q_NAN;
    }
    float q = c * 4.0f;
    if (q >= 0.0f) q += 0.5f; else q -= 0.5f;   /* round-to-nearest */
    if (q > 32767.0f)  q = 32767.0f;
    if (q < -32767.0f) q = -32767.0f;           /* a -32768 a NAN-szentinel, ne ütközzön */
    return (int16_t)q;
}

static float q_to_temp(int16_t q)
{
    if (q == M1B_TEMP_Q_NAN) {
        return NAN;
    }
    return (float)q / 4.0f;
}

/* Rekord szerializálás (16 B, LE) + EEPROM-ba az idx helyére. */
static esp_err_t ee_write_record(uint16_t idx, const m1b_sample_t *s)
{
    uint8_t r[M1B_REC_SIZE];
    uint16_t cyc = (uint16_t)s->cycle;                         /* u16 (255 < 65535: bőven elég) */
    uint32_t du  = (s->ds_unix < 0) ? 0u : (uint32_t)s->ds_unix;
    int16_t  dsq = temp_to_q(s->ds_c);
    int16_t  diq = temp_to_q(s->die_c);
    uint32_t hx  = (uint32_t)s->hx;
    uint16_t aw  = s->awake_ms;

    r[0]  = (uint8_t)(cyc & 0xFF);
    r[1]  = (uint8_t)((cyc >> 8) & 0xFF);
    r[2]  = (uint8_t)(du & 0xFF);
    r[3]  = (uint8_t)((du >> 8) & 0xFF);
    r[4]  = (uint8_t)((du >> 16) & 0xFF);
    r[5]  = (uint8_t)((du >> 24) & 0xFF);
    r[6]  = (uint8_t)(dsq & 0xFF);
    r[7]  = (uint8_t)(((uint16_t)dsq >> 8) & 0xFF);
    r[8]  = (uint8_t)(diq & 0xFF);
    r[9]  = (uint8_t)(((uint16_t)diq >> 8) & 0xFF);
    r[10] = (uint8_t)(hx & 0xFF);
    r[11] = (uint8_t)((hx >> 8) & 0xFF);
    r[12] = (uint8_t)((hx >> 16) & 0xFF);
    r[13] = (uint8_t)((hx >> 24) & 0xFF);
    r[14] = (uint8_t)(aw & 0xFF);
    r[15] = (uint8_t)((aw >> 8) & 0xFF);

    uint16_t addr = (uint16_t)(M1B_REC_BASE + (uint32_t)idx * M1B_REC_SIZE);
    return ee_write(addr, r, sizeof(r));
}

/* Rekord olvasás + kicsomagolás. */
static esp_err_t ee_read_record(uint16_t idx, m1b_sample_t *s)
{
    uint8_t r[M1B_REC_SIZE] = { 0 };
    uint16_t addr = (uint16_t)(M1B_REC_BASE + (uint32_t)idx * M1B_REC_SIZE);
    esp_err_t err = ee_read(addr, r, sizeof(r));
    if (err != ESP_OK) {
        return err;
    }
    uint16_t cyc = (uint16_t)r[0] | ((uint16_t)r[1] << 8);
    uint32_t du  = (uint32_t)r[2] | ((uint32_t)r[3] << 8) | ((uint32_t)r[4] << 16) | ((uint32_t)r[5] << 24);
    int16_t  dsq = (int16_t)((uint16_t)r[6] | ((uint16_t)r[7] << 8));
    int16_t  diq = (int16_t)((uint16_t)r[8] | ((uint16_t)r[9] << 8));
    uint32_t hx  = (uint32_t)r[10] | ((uint32_t)r[11] << 8) | ((uint32_t)r[12] << 16) | ((uint32_t)r[13] << 24);
    uint16_t aw  = (uint16_t)r[14] | ((uint16_t)r[15] << 8);

    s->cycle    = cyc;
    s->ds_unix  = (int64_t)du;
    s->ds_c     = q_to_temp(dsq);
    s->die_c    = q_to_temp(diq);
    s->hx       = (int32_t)hx;
    s->awake_ms = aw;
    return ESP_OK;
}

/* ===================== HX711 (bit-bang, a H2-ből) ===================== */

static void hx711_gpio_setup(int pd_sck_level)
{
    /* KRITIKUS: a deep sleep előtt a PD_SCK-t gpio_hold_en()-nel HIGH-on (power-down)
     * befagyasztottuk. Ébredés után (deep sleep = reboot a C3-on) a hold MÉG AKTÍV, ezért
     * a lenti gpio_set_level NEM hatna -> a HX711 power-downban ragadna, a DOUT sosem menne
     * LOW, és minden timer-wake olvasás 0-t / timeoutot adna (a cold-boot idx=0 jó volt,
     * idx>=1 hx=0 — ez volt a tünet). A hold-ot a setup ELŐTT fel kell oldani. */
    gpio_hold_dis(HX711_PD_SCK_GPIO);
    gpio_deep_sleep_hold_dis();

    gpio_config_t pdsck_cfg = {
        .pin_bit_mask = (1ULL << HX711_PD_SCK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pdsck_cfg));
    ESP_ERROR_CHECK(gpio_set_level(HX711_PD_SCK_GPIO, pd_sck_level));

    gpio_config_t dout_cfg = {
        .pin_bit_mask = (1ULL << HX711_DOUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout_cfg));
}

/* Egy nyers 24-bit olvasás (A-csatorna, gain 128). DOUT LOW = adat kész; 25 SCK-pulzus. */
static int32_t hx711_read_raw(void)
{
    int guard = 0;
    while (gpio_get_level(HX711_DOUT_GPIO) == 1) {
        esp_rom_delay_us(100);
        if (++guard > 5000) {   /* ~0.5 s timeout */
            ESP_LOGW(TAG, "HX711 DOUT timeout (nem kesz / nincs modul / rossz bekotes?)");
            return 0;
        }
    }
    uint32_t value = 0;
    for (int i = 0; i < 24; i++) {
        gpio_set_level(HX711_PD_SCK_GPIO, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (uint32_t)gpio_get_level(HX711_DOUT_GPIO);
        gpio_set_level(HX711_PD_SCK_GPIO, 0);
        esp_rom_delay_us(1);
    }
    /* 25. pulzus: gain 128 (A) a következő konverzióhoz. */
    gpio_set_level(HX711_PD_SCK_GPIO, 1);
    esp_rom_delay_us(1);
    gpio_set_level(HX711_PD_SCK_GPIO, 0);
    esp_rom_delay_us(1);

    if (value & 0x800000U) {
        value |= 0xFF000000U;
    }
    return (int32_t)value;
}

/* Power-up -> settle -> skip -> N átlag -> a nyers értéket adja vissza. */
static int32_t hx711_measure(void)
{
    hx711_gpio_setup(0);                          /* PD_SCK LOW = power-up */
    vTaskDelay(pdMS_TO_TICKS(M1B_HX_SETTLE_MS));  /* settle */

    for (int i = 0; i < M1B_HX_SKIP; i++) {
        (void)hx711_read_raw();                   /* tranziens minták eldobása */
    }
    int64_t acc = 0;
    for (int i = 0; i < M1B_HX_AVG; i++) {
        acc += hx711_read_raw();
    }
    return (int32_t)(acc / M1B_HX_AVG);
}

/* HX711 power-down + GPIO hold deep sleephez (a H1/H2-ből; a C3-nak nincs RTCIO). */
static void hx711_power_down_and_hold(void)
{
    gpio_set_level(HX711_PD_SCK_GPIO, 1);   /* HIGH = power-down */
    esp_rom_delay_us(100);                  /* >60 µs, hogy biztosan power-downba menjen */
    gpio_hold_en(HX711_PD_SCK_GPIO);        /* a HIGH szint befagyasztva alvásra */
    gpio_deep_sleep_hold_en();              /* a hold deep sleep alatt is éljen */
}

/* ===================== C3 die-hő (a H1/M1-ből) ===================== */

static float read_die_temp_c(void)
{
    temperature_sensor_handle_t h = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    float die_c = NAN;
    if (temperature_sensor_install(&cfg, &h) != ESP_OK) {
        return NAN;
    }
    if (temperature_sensor_enable(h) == ESP_OK) {
        if (temperature_sensor_get_celsius(h, &die_c) != ESP_OK) {
            die_c = NAN;
        }
        temperature_sensor_disable(h);
    }
    temperature_sensor_uninstall(h);
    return die_c;
}

/* ===================== DUMP-mód ===================== */

/* Beolvassa a fejlécet; ha érvényes magic + records>0, kiírja a BEFAGYASZTOTT KONTRAKTUS
 * szerinti fejléc-sort + minden rekordot. SOHA nem ír / nem töröl. */
static void dump_log(void)
{
    uint32_t magic = 0;
    uint16_t widx = 0;
    if (ee_read_header(&magic, &widx) != ESP_OK) {
        ESP_LOGW(TAG, "DUMP: EEPROM-fejlec olvasas hiba (I2C?). Nincs mit dumpolni.");
        return;
    }
#if M1B_DUMP_ALL
    /* RECOVERY: a write_index/magic figyelmen kivul hagyva, MIND a 255 rekord-helyet kiirjuk.
     * A Python a valos ds_unix alapjan szuri az ervenyeseket (a soha-nem-irt helyek szemete /
     * 0xFF / 0 kiesik). Igy egy veletlen erase utan a fizikailag megmaradt regi log visszanyerheto. */
    ESP_LOGW(TAG, "DUMP-ALL (RECOVERY): teljes EEPROM (%u hely), write_index=%u figyelmen kivul.",
             (unsigned)M1B_MAX_RECORDS, (unsigned)widx);
    widx = M1B_MAX_RECORDS;
#else
    if (magic != M1B_EE_MAGIC || widx == 0) {
        ESP_LOGW(TAG, "DUMP: nincs ervenyes log (magic=0x%08X widx=%u). Indits uj merest (M1B_ERASE_AND_START=1).",
                 (unsigned)magic, (unsigned)widx);
        return;
    }
    if (widx > M1B_MAX_RECORDS) {
        widx = M1B_MAX_RECORDS;   /* védelem: a buffer megtelt, csak a férőképes rekordok */
    }
#endif

    /* start_unix = az 1. rekord ds_unix-e. */
    m1b_sample_t first;
    int64_t start_unix = 0;
    if (ee_read_record(0, &first) == ESP_OK) {
        start_unix = first.ds_unix;
    }

    /* FEJLÉC-SOR (PONTOSAN egy sor; printf közvetlenül, ne a logger-prefixszel). */
    printf("M1B-HDR records=%u period_s=%u start_unix=%lld\n",
           (unsigned)widx, (unsigned)M1B_WAKE_PERIOD_SEC, (long long)start_unix);

    for (uint16_t i = 0; i < widx; i++) {
        m1b_sample_t s;
        if (ee_read_record(i, &s) != ESP_OK) {
            ESP_LOGW(TAG, "DUMP: rekord %u olvasas hiba", (unsigned)i);
            continue;
        }
        printf("M1B idx=%u cycle=%u ds_unix=%lld ds_c=%.2f die_c=%.2f hx=%ld awake_ms=%u\n",
               (unsigned)i, (unsigned)s.cycle, (long long)s.ds_unix,
               (double)s.ds_c, (double)s.die_c, (long)s.hx, (unsigned)s.awake_ms);
    }
    fflush(stdout);
}

/* ===================== mérés + alvás ===================== */

/* Egy mérési ciklus: minden szenzort kiolvas, EEPROM-ba ír, write_index++. */
static void measure_and_log(uint32_t cycle)
{
    /* 1) DS3231 idő + hő. */
    int64_t ds_unix = ds3231_read_unix();
    float   ds_c    = ds3231_read_temp_c();
    /* 2) C3 die-hő. */
    float   die_c   = read_die_temp_c();
    /* 3) HX711. */
    int32_t hx      = hx711_measure();

    /* 3b) ÉBREN-idő: a boot óta esp_timer (µs->ms) MOST, a rekord-szerializálás előtt. Ez fedi a
     *     mérés dominánsát (HX711 settle+olvasás, DS3231, die-hő); a rekord-írás utáni ~20 ms +
     *     a boot (reset->app_main, ~100 ms, esp_timer-rel nem mérhető) kimarad -> kis maradék
     *     offset, de a domináns awake-et a Python ebből pontosan levonja. u16-ra telítve. */
    uint32_t aw_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t awake_ms = (aw_ms > 65535u) ? 65535u : (uint16_t)aw_ms;

    m1b_sample_t s = {
        .cycle = cycle, .ds_unix = ds_unix, .ds_c = ds_c, .die_c = die_c, .hx = hx,
        .awake_ms = awake_ms,
    };

    /* 4) EEPROM-ba: a write_index helyre, majd write_index++. */
    uint32_t magic = 0;
    uint16_t widx  = 0;
    if (ee_read_header(&magic, &widx) != ESP_OK || magic != M1B_EE_MAGIC) {
        /* Nem inicializált fejléc a mérés alatt: első mérésnél inicializáljuk (widx=0).
         * (Normál esetben az ERASE+START build már beírta a fejlécet.) */
        ESP_LOGW(TAG, "Ervenytelen fejlec a meresnel -> init (widx=0).");
        widx = 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ee_write_header(M1B_EE_MAGIC, 0));
    }

    if (widx >= M1B_MAX_RECORDS) {
        ESP_LOGW(TAG, "EEPROM MEGTELT (%u/%u rekord) -> nem irok tobbet.",
                 (unsigned)widx, (unsigned)M1B_MAX_RECORDS);
    } else {
        if (ee_write_record(widx, &s) == ESP_OK) {
            widx++;
            ESP_ERROR_CHECK_WITHOUT_ABORT(ee_write_header(M1B_EE_MAGIC, widx));
        } else {
            ESP_LOGW(TAG, "Rekord-iras hiba (idx=%u).", (unsigned)widx);
        }
    }

    /* 5) Rövid INFO-sor. */
    ESP_LOGI(TAG, "cycle=%u idx=%u ds_unix=%lld ds_c=%.2f die_c=%.2f hx=%ld -> alvas %d s",
             (unsigned)cycle, (unsigned)(widx ? widx - 1 : 0), (long long)ds_unix,
             (double)ds_c, (double)die_c, (long)hx, M1B_WAKE_PERIOD_SEC);
}

/* HX711 power-down + GPIO-hold, USB-JTAG pad off, timer-wakeup, deep sleep. NEM tér vissza. */
static void enter_deep_sleep(void)
{
    hx711_power_down_and_hold();

    fflush(stdout);
    esp_rom_delay_us(20000);   /* ~20 ms a soros kiírás befejezésére */

#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)M1B_WAKE_PERIOD_SEC * 1000000ULL));
    esp_deep_sleep_start();
}

/* ===================== DIAGNOSZTIKA (M1B_DIAG_MODE) ===================== */
#if M1B_DIAG_MODE
/* Ébren maradó hibakereső loop: I2C-scan + AT24C32 write/readback + DS3231 + HX711. NEM tér vissza. */
static void run_diag(void)
{
    ESP_LOGW(TAG, "=== M1B DIAG MODE === (nem alszik; Ctrl-C a monitorbol a kilepeshez)");
    ESP_LOGI(TAG, "I2C: SDA=GPIO%d SCL=GPIO%d @%d Hz | DS3231=0x%02X AT24C32=0x%02X",
             M1B_I2C_SDA_GPIO, M1B_I2C_SCL_GPIO, M1B_I2C_FREQ_HZ, DS3231_ADDR, AT24C32_ADDR);

    int round = 0;
    while (1) {
        printf("\n----- DIAG round %d -----\n", round++);

        /* 1) I2C-busz scan: mely 7-bites címek ACK-olnak. */
        printf("I2C scan:");
        int found = 0;
        for (uint16_t a = 0x08; a <= 0x77; a++) {
            if (i2c_master_probe(s_i2c_bus, a, 50) == ESP_OK) {
                printf(" 0x%02X", (unsigned)a);
                found++;
            }
        }
        printf("  (%d eszkoz)\n", found);

        /* 2) AT24C32 fejléc (0x0000) nyers 6 bájt — amit a dump is olvas. */
        uint8_t hdr[6] = { 0 };
        esp_err_t er = ee_read(M1B_HDR_ADDR, hdr, sizeof(hdr));
        if (er == ESP_OK) {
            printf("EE header @0x0000: %02X %02X %02X %02X %02X %02X\n",
                   hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]);
        } else {
            printf("EE header olvasas HIBA: %s\n", esp_err_to_name(er));
        }

        /* 3) AT24C32 ÍRÁS/VISSZAOLVASÁS teszt egy nem használt címen. round-fuggo minta,
         *    hogy a 'beragadt' (mindig ugyanaz) olvasas is kiderüljön. */
        uint8_t pat[6] = { 0xDE, 0xAD, 0xBE, 0xEF, (uint8_t)round, (uint8_t)~round };
        uint8_t rb[6]  = { 0 };
        esp_err_t we = ee_write(M1B_DIAG_EE_TEST_ADDR, pat, sizeof(pat));
        esp_err_t re = ee_read(M1B_DIAG_EE_TEST_ADDR, rb, sizeof(rb));
        bool match = (we == ESP_OK && re == ESP_OK && memcmp(pat, rb, sizeof(pat)) == 0);
        printf("EE write/readback @0x%04X: w=%s r=%s rb=%02X %02X %02X %02X %02X %02X -> %s\n",
               (unsigned)M1B_DIAG_EE_TEST_ADDR, esp_err_to_name(we), esp_err_to_name(re),
               rb[0], rb[1], rb[2], rb[3], rb[4], rb[5],
               match ? "OK (iras MEGY)" : "FAIL (write-protect? cim? bekotes?)");

        /* 4) DS3231 idő + hő. */
        int64_t u = ds3231_read_unix();
        float dc  = ds3231_read_temp_c();
        printf("DS3231: unix=%lld  temp=%.2f C\n", (long long)u, (double)dc);

        /* 5) C3 die-hő + HX711 nyers. */
        float die = read_die_temp_c();
        int32_t hx = hx711_measure();
        printf("die_c=%.2f C  HX711 raw=%ld\n", (double)die, (long)hx);

        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif /* M1B_DIAG_MODE */

/* ===================== app_main ===================== */

void app_main(void)
{
    /* NVS init (jó gyakorlat; a temp-szenzor/driverek nem feltétlen igénylik, de ártalmatlan). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s (folytatas)", esp_err_to_name(nvs_err));
    }

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool timer_wake = (cause == ESP_SLEEP_WAKEUP_TIMER) && (s_rtc_magic == M1B_RTC_MAGIC);

    /* I2C busz + DS3231 + AT24C32 device-ek. (Mindenhol kell: mérés ÉS dump.) */
    esp_err_t ierr = i2c_bus_init();
    if (ierr != ESP_OK) {
        ESP_LOGE(TAG, "I2C init hiba: %s — tartos idle.", esp_err_to_name(ierr));
        for (;;) { vTaskDelay(portMAX_DELAY); }
    }

#if M1B_SET_RTC_ON_BOOT
    /* EGYSZER: a DS3231 idejét beírjuk a beégetett epoch-ból, majd a normál menet. */
    ds3231_set_time_from_epoch(M1B_RTC_SET_EPOCH);
#endif

#if M1B_DIAG_MODE
    run_diag();   /* nem tér vissza: ébren maradó HW-bringup diagnosztika */
#endif

    if (timer_wake) {
        /* ---- OFF-GRID MÉRÉS (a normál ciklus) ---- */
        s_rtc_cycle++;
        ESP_LOGI(TAG, "=== M1B WAKE (timer) === cycle=%u", (unsigned)s_rtc_cycle);
        measure_and_log(s_rtc_cycle);
        enter_deep_sleep();   /* nem tér vissza */
        return;
    }

    /* ---- COLD BOOT (nem timer-wake): PC-re dugva ---- */
#if M1B_ERASE_AND_START
    /* EXPLICIT új mérés: fejléc reset (régi log eldobva) + AZONNAL mérés-start. */
    ESP_LOGW(TAG, "=== M1B ERASE+START === EEPROM-log torles (widx=0), uj meres indul.");
    ESP_ERROR_CHECK_WITHOUT_ABORT(ee_write_header(M1B_EE_MAGIC, 0));

    s_rtc_magic = M1B_RTC_MAGIC;
    s_rtc_cycle = 0;
    measure_and_log(s_rtc_cycle);   /* a 0. rekord */
    enter_deep_sleep();             /* nem tér vissza */
    return;
#else
    /* NORMÁL cold boot -> DUMP-mód: kiírja a logot, majd tartós idle (NEM ír, NEM töröl). */
    ESP_LOGI(TAG, "=== M1B DUMP MODE (cold boot) === a log kovetkezik (sosem irok/torlok):");
    dump_log();
    ESP_LOGI(TAG, "=== M1B DUMP VEGE === idle. Uj mereshez: M1B_ERASE_AND_START=1 + flash.");
    /* Tartós idle — NEM alszik el, hogy a USB-konzol elérhető maradjon és a dump olvasható. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
}

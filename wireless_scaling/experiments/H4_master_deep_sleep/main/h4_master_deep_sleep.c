/*
 * H4 — Master board deep-sleep áram (ESP32-S3-Zero)
 * bee_scale / wireless_scaling · 2. fázis · experiments/H4_master_deep_sleep
 *
 * CÉL
 *   A MASTER (ESP32-S3-Zero, Waveshare) nyers nyugalmi (deep-sleep) áramának kimérése.
 *   Ez a master-energiamérleg ALAPSZÁMA (MEMORY.md 7. nyitott kérdés #2, és a H3 duty-cycle
 *   ideális RX-komponensére rakódó "nyugalmi" alsó korlát). A firmware MINIMÁLIS és a MÉRÉST
 *   szolgálja: nincs rádió (sem WiFi, sem ESP-NOW), nincs PSRAM-használat — csak a fedélzeti
 *   fogyasztók lehető legkisebbre húzása, alvásbarát GPIO-konfig, majd esp_deep_sleep.
 *
 *   Ez a NODE-oldali H1 (ESP32-C3 SuperMini deep-sleep, ~185 µA) MASTER-párja. A H1-ből
 *   újrahasznált minta: USB-Serial-JTAG pad lekapcsolás + GPIO hold alvásra.
 *
 * BOARD / FEDÉLZETI FOGYASZTÓK — webről ellenőrizve (NEM emlékezetből), 2026-07-02:
 *   ESP32-S3-Zero (Waveshare, ESP32-S3FH4R2 = 4 MB flash + 2 MB PSRAM):
 *     - USB-C KÖZVETLENÜL az S3 beépített USB-Serial-JTAG-jára megy (nincs külön UART-bridge
 *       chip). A USB-Serial-JTAG pad deep sleepben is bekapcsolva marad (~500–800 µA!), az IDF
 *       ezt csak light sleepnél kapcsolja le automatikusan, deep sleepnél NEM -> explicit
 *       lekapcsoljuk (lásd H1 tanulság: 731 µA -> 185 µA a C3-on).
 *     - WS2812 RGB LED a GPIO21-en (fedélzeti). A chip a táprailen ül -> a nyugalmi
 *       (nem-világító) árama HW-adottság, SW-ből nem kapcsolható le (nincs load switch).
 *       A data-lábat (GPIO21) alvásra LOW-ra húzzuk + hold, hogy lebegő láb ne vigyen zajt
 *       a WS2812-be. Reset után a WS2812 tipikusan SÖTÉT. HA boot után mégis világít
 *       (glitch), jelezd -> akkor fekete-frame kell (led_strip). A LEGTISZTÁBB méréshez a
 *       WS2812 fizikailag is leszedhető, ahogy a H1-ben a C3 power-LED-jét.
 *     - LDO: ME6217C33M5G (max 800 mA), tipikus nyugalmi áram ~15 µA (adatlap-tartomány,
 *       darabfüggő) -> ez a mért deep-sleep áram egy komponense, HW-adottság.
 *     - Strapping: GPIO0 (BOOT gomb), GPIO3, GPIO45, GPIO46 — általános IO-nak kerülendők.
 *     - USB-JTAG/soros: GPIO19 (D-), GPIO20 (D+).
 *   Forrás: waveshare.com/wiki/ESP32-S3-Zero, docs.waveshare.com/ESP32-S3-Zero,
 *           espboards.dev/esp32/esp32-s3-zero/ (2026-07-02).
 *
 * VÁRHATÓ NAGYSÁGRENDEK (ellenőrizendő a mérésen):
 *   - ESP32-S3 chip RTC deep sleep: ~7–10 µA.
 *   - + LDO nyugalmi ~15 µA + WS2812 nyugalmi (0-tól ~1 mA-ig, darabfüggő).
 *   - USB-JTAG pad lekapcsolás NÉLKÜL: +~500–800 µA (ezért kapcsoljuk le).
 *
 * FLASHELÉS: tartós deep sleepben a board NEM enumerál USB-n -> BOOT+RESET a bootloaderbe
 *   (BOOT nyomva tartva reset), majd upload. A boot-log a USB-Serial-JTAG-on jön.
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"                 /* esp_rom_delay_us() */
#include "soc/soc_caps.h"               /* SOC_USB_SERIAL_JTAG_SUPPORTED */
#include "esp_private/sleep_console.h" /* sleep_console_usj_pad_backup_and_disable() */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/*
 * Wakeup-periódus.
 *   DEEP_SLEEP_WAKEUP_SEC == 0  -> NINCS timer-wakeup: tartós (örök) deep sleep.
 *       Ez az ALAPÉRTELMEZÉS a tiszta nyugalmi áram méréséhez — a board addig alszik,
 *       amíg manuálisan nem resetelsz. Így a multiméter stabil deep-sleep áramot mutat.
 *   DEEP_SLEEP_WAKEUP_SEC  > 0  -> timer-wakeup ennyi másodperc után (ciklikus szimuláció).
 *       Wakeup után a firmware újraindul az app_main()-ből (deep sleep = teljes reboot).
 */
#define DEEP_SLEEP_WAKEUP_SEC   0

/*
 * WS2812 fedélzeti RGB LED data-láb. Alvásra LOW-ra húzzuk + GPIO hold.
 * (A chip nyugalmi árama HW-adottság; ezt a láb szintje nem befolyásolja, csak a lebegő
 *  láb okozta esetleges WS2812-zajt zárja ki.)
 */
#define WS2812_DATA_GPIO        GPIO_NUM_21

/* ================================================================= */

static const char *TAG = "H4";

/*
 * A WS2812 data-lábat OUTPUT LOW-ra állítja. A folyamatos LOW a WS2812-nek reset-jel
 * (>50 µs), új színt nem ír; a lebegő láb okozta véletlen adatot kizárja. A LED aktuális
 * (reset után tipikusan OFF) állapota marad. A szintet alvásra a hold fagyasztja be.
 */
static void ws2812_data_low(void)
{
    gpio_config_t data_cfg = {
        .pin_bit_mask = (1ULL << WS2812_DATA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&data_cfg));
    ESP_ERROR_CHECK(gpio_set_level(WS2812_DATA_GPIO, 0));   /* LOW = nincs érvényes adat */

    ESP_LOGI(TAG, "WS2812 data (GPIO%d) = LOW (nincs uj szin; a LED tipikusan sotet)",
             WS2812_DATA_GPIO);
}

/*
 * Deep sleep alatt megőrzendő GPIO-szintek rögzítése (GPIO hold), hogy a WS2812 data-láb
 * LOW maradjon (ne lebegjen), különben deep sleepben az IO elengedne. Az ESP32-S3-nak van
 * RTCIO-ja, de a digitális szint befagyasztására a target-független gpio_hold_en() (lábanként)
 * + gpio_deep_sleep_hold_en() (globális) páros a legegyszerűbb és a H1-gyel konzisztens.
 */
static void hold_critical_gpios_for_sleep(void)
{
    gpio_hold_en(WS2812_DATA_GPIO);   /* WS2812 data LOW szintje befagyasztva */
    gpio_deep_sleep_hold_en();        /* a holdok deep sleep alatt is éljenek */

    ESP_LOGI(TAG, "GPIO hold: WS2812 data (GPIO%d) LOW megtartva alvas alatt",
             WS2812_DATA_GPIO);
}

void app_main(void)
{
    /* Miért ébredtünk? (időmért ciklusnál hasznos a logban.) */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "=== H4 master deep-sleep mero firmware (ESP32-S3-Zero) ===");
    ESP_LOGI(TAG, "Wakeup cause: %d (0 = power-on/reset)", (int)cause);

    /* 1) WS2812 data-láb LOW — a fedélzeti RGB LED ne kapjon véletlen adatot. */
    ws2812_data_low();

    /*
     * 2) Alvásbarát GPIO-konfig: minden NEM használt, kivezetett GPIO maradjon a reset utáni
     *    nagy-impedanciás (input, pull nélkül) állapotban a legkisebb szivárgásért. A reset
     *    default az S3-on már input + belső pull-ok tiltva -> szándékosan nem kapcsolunk be
     *    felhúzásokat. A GPIO19/20 (USB) lábakat nem piszkáljuk, hogy a soros/USB elérés
     *    megmaradjon a fejlesztéshez; a USB-pad hatását a 4) lépés kezeli.
     */

    /* 3) Kritikus szintek rögzítése alvásra (WS2812 data LOW hold). */
    hold_critical_gpios_for_sleep();

    /* 4) Wakeup-forrás konfig. */
#if DEEP_SLEEP_WAKEUP_SEC > 0
    ESP_LOGI(TAG, "Timer wakeup: %d s mulva ebredes", DEEP_SLEEP_WAKEUP_SEC);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(
        (uint64_t)DEEP_SLEEP_WAKEUP_SEC * 1000000ULL));
#else
    ESP_LOGI(TAG, "Nincs timer-wakeup -> TARTOS deep sleep (reset kell az ebreszteshez)");
    /* Minden alapertelmezett wakeup-forras tiltva a legtisztabb meresert. */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#endif

    ESP_LOGI(TAG, "Deep sleepbe lepunk. Mostantol a multimeter a nyugalmi aramot mutatja.");
    ESP_LOGI(TAG, "(USB-Serial-JTAG pad mindjart lekapcsol -> innentol nincs USB-log.)");
    /* A log kiürítése, MIELŐTT a USB-pad lekapcsol (különben az utolsó sorok elvesznek). */
    fflush(stdout);
    esp_rom_delay_us(20000);   /* ~20 ms a soros kiírás befejezésére */

    /*
     * 5) USB-Serial-JTAG pad LEKAPCSOLÁSA deep sleephez — KRITIKUS a fogyasztásra.
     *    Az S3-on (mint a C3-on) a USJ pad/USB-PHY deep sleepben is bekapcsolva marad
     *    (~500–800 µA), és az IDF ezt CSAK light sleepnél kapcsolja le automatikusan.
     *    Mivel a console a natív USB-Serial-JTAG (boot-log USB-n), ez tartja ébren a padot.
     *    (Reset-ből ébredünk deep sleep után -> pad-restore nem szükséges.)
     */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif

    /* 6) Deep sleep — innen nincs visszatérés; wakeup esetén app_main() újraindul. */
    esp_deep_sleep_start();
}

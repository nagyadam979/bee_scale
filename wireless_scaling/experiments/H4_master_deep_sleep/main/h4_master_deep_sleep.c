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
 *     - WS2812 RGB LED a GPIO21-en (fedélzeti). BOOT-GLITCH miatt bekapcsolva világít
 *       (éles hardveren igazolva) -> a puszta data-LOW NEM oltja ki, mert a WS2812 tartja a
 *       latch-elt színt (a folyamatos LOW csak reset-jel, nem törli a színt). Ezért deep sleep
 *       ELŐTT EGY FEKETE FRAME-et küldünk az led_strip RMT-driverével (led_strip_clear ->
 *       minden pixel 0 + refresh), majd az RMT-t felszabadítjuk és a data-lábat LOW-ra
 *       fagyasztjuk (hold). A LED így SÖTÉT marad, amíg a chip tápot kap.
 *       A chip NYUGALMI (nem-világító) árama viszont HW-adottság, SW-ből NEM kapcsolható le
 *       (nincs load switch a táprailén). A LEGTISZTÁBB méréshez a WS2812 fizikailag leszedhető,
 *       ahogy a H1-ben a C3 power-LED-jét.
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
#include "led_strip.h"                 /* espressif/led_strip (managed comp) — WS2812 fekete frame */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/*
 * Wakeup-periódus.
 *   DEEP_SLEEP_WAKEUP_SEC == 0  -> NINCS timer-wakeup: tartós (örök) deep sleep.
 *       Ez az ALAPÉRTELMEZÉS a tiszta nyugalmi áram méréséhez — a board addig alszik,
 *       amíg manuálisan nem resetelsz. Így a multiméter stabil deep-sleep áramot mutat.
 *   DEEP_SLEEP_WAKEUP_SEC  > 0  -> timer-wakeup ennyi másodperc után (ciklikus szimuláció).
 *       Wakeup után a firmware újraindul az app_main()-ből (deep sleep = teljes reboot).
 */
#define DEEP_SLEEP_WAKEUP_SEC   0   /* 0 = tartos (orok) deep sleep a tiszta nyugalmi arammeresert */

/*
 * DEBUG_KEEP_USB_ALIVE == 1 -> NEM kapcsoljuk le a USB-Serial-JTAG padot alvas elott.
 *   Ilyenkor a USB stabil marad, a soros log folyamatosan olvashato (ciklikus ebredesnel is),
 *   NEM enumeral ujra minden ciklusban. CSAK debughoz! A fogyasztas ilyenkor MAGAS (~pad ~500 uA).
 *   ELES MERES: 0 (a pad lekapcsol -> tiszta nyugalmi aram).
 */
#define DEBUG_KEEP_USB_ALIVE    0

/*
 * DEBUG_LED_BLINK == 1 -> az app_main NEM alszik el, hanem a WS2812-t PIROS<->SÖTÉT villogtatja
 *   vegtelen loopban. Tisztan vizualis teszt: latszik-e, hogy az led_strip vezerli a LED-et.
 *   ELES: 0.
 */
#define DEBUG_LED_BLINK         0

/*
 * DEBUG_OFF_NOSLEEP == 1 -> lefuttatja a teljes kioltas-utat (ws2812_off + data_low + hold),
 *   majd NEM alszik el, hanem loopol. Igy latszik, hogy a clear UTANI lepesek (del/data_low/hold)
 *   sotetben hagyjak-e a LED-et, a deep sleep zavaro hatasa nelkul. ELES: 0.
 */
#define DEBUG_OFF_NOSLEEP       0

/*
 * WS2812 fedélzeti RGB LED data-láb. Alvásra LOW-ra húzzuk + GPIO hold.
 * (A chip nyugalmi árama HW-adottság; ezt a láb szintje nem befolyásolja, csak a lebegő
 *  láb okozta esetleges WS2812-zajt zárja ki.)
 */
#define WS2812_DATA_GPIO        GPIO_NUM_21

/* ================================================================= */

static const char *TAG = "H4";

/*
 * A fedélzeti WS2812-t KIOLTJA: egy fekete (0,0,0) frame-et küld az led_strip RMT-driverével,
 * majd felszabadítja az RMT-csatornát + a GPIO-t. A boot-glitch miatt bekapcsolva világító
 * LED-et CSAK érvényes WS2812 frame oltja ki (a data-LOW nem elég). A led_strip_clear() minden
 * pixelt 0-ra állít és refresh-el -> a WS2812 fekete színt latch-el, és tápellátás alatt sötét
 * marad. Utána a data-láb szabaddá válik -> ws2812_data_low() definiált szintre állítja.
 */
static void ws2812_off(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = WS2812_DATA_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz — a WS2812 timinghez bőven elég */
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };

    led_strip_handle_t strip = NULL;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));
    ESP_ERROR_CHECK(led_strip_clear(strip));   /* minden pixel 0 (fekete) + refresh */
    ESP_ERROR_CHECK(led_strip_del(strip));     /* RMT-csatorna + GPIO felszabadítása */

    ESP_LOGI(TAG, "WS2812 (GPIO%d): fekete frame elkuldve -> LED KIOLTVA", WS2812_DATA_GPIO);
}

/*
 * A WS2812 data-lábat OUTPUT LOW-ra állítja (a fekete frame UTÁN). A LED ekkor már fekete
 * színt latch-elt; a folyamatos LOW csak reset-jel, új színt nem ír, és a lebegő láb okozta
 * véletlen adatot kizárja. A szintet alvásra a hold fagyasztja be.
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

#if DEBUG_LED_BLINK
    /*
     * DEBUG vizuális önteszt: eldönti, hogy az led_strip TÉNYLEGESEN vezérli-e a GPIO21-es
     * WS2812-t. Végtelen loop: PIROS 1 s <-> SÖTÉT 1 s. NEM megy deep sleepbe.
     *   - Ha a LED villog piros/sötét -> a driver+pin JÓ (a gond a kioltás sorrend/timing).
     *   - Ha a LED nem reagál (végig ugyanaz) -> az led_strip NEM éri el a LED-et (pin/hw).
     */
    {
        led_strip_config_t scfg = {
            .strip_gpio_num = WS2812_DATA_GPIO, .max_leds = 1,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
            .flags = { .invert_out = false },
        };
        led_strip_rmt_config_t rcfg = {
            .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000,
            .mem_block_symbols = 64, .flags = { .with_dma = false },
        };
        led_strip_handle_t st = NULL;
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&scfg, &rcfg, &st));
        ESP_LOGW(TAG, "DEBUG_LED_BLINK: PIROS<->SOTET villogas (nincs deep sleep)");
        bool on = false;
        while (1) {
            if (on) { ESP_ERROR_CHECK(led_strip_set_pixel(st, 0, 60, 0, 0)); }
            else    { ESP_ERROR_CHECK(led_strip_clear(st)); }
            ESP_ERROR_CHECK(led_strip_refresh(st));
            ESP_LOGI(TAG, "LED %s", on ? "PIROS" : "SOTET");
            on = !on;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
#endif

    /* 1) WS2812 KIOLTÁSA fekete frame-mel (a boot-glitch bekapcsolja) + data-láb LOW. */
    ws2812_off();
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

#if DEBUG_OFF_NOSLEEP
    ESP_LOGW(TAG, "OFF-TEST: kioltas-ut lefutott (off+data_low+hold). NINCS deep sleep.");
    ESP_LOGW(TAG, "A LED-nek MOST sotetnek kell lennie. Ha vilagit -> a del/data_low/hold glitch-el.");
    while (1) { vTaskDelay(pdMS_TO_TICKS(2000)); ESP_LOGI(TAG, "OFF-TEST el, LED?"); }
#endif

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
#if SOC_USB_SERIAL_JTAG_SUPPORTED && !DEBUG_KEEP_USB_ALIVE
    sleep_console_usj_pad_backup_and_disable();
#endif
#if DEBUG_KEEP_USB_ALIVE
    ESP_LOGW(TAG, "DEBUG_KEEP_USB_ALIVE=1 -> USB-pad NEM kapcsol le (magas aram, stabil log)!");
    fflush(stdout);
    esp_rom_delay_us(20000);
#endif

    /* 6) Deep sleep — innen nincs visszatérés; wakeup esetén app_main() újraindul. */
    esp_deep_sleep_start();
}

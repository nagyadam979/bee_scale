/*
 * H1 — Node board előkészítés (deep-sleep áram)
 * bee_scale / wireless_scaling · 2. fázis · experiments/H1_node_board_prep
 *
 * CÉL
 *   A node (ESP32-C3 SuperMini) nyers nyugalmi (deep-sleep) áramának kimérése és
 *   minimalizálása. Ez a teljes node-energiamérleg ALAPSZÁMA. A firmware MINIMÁLIS és a
 *   MÉRÉST szolgálja: nincs rádió (sem WiFi, sem ESP-NOW), nincs szenzor-olvasás —
 *   csak a HX711 power-downba küldése, az alvásbarát GPIO-konfig, majd esp_deep_sleep.
 *
 *   A pass/fail küszöb (lásd README pass/fail tábla, a HX711-gyel együtt, H2-vel közös):
 *     - JÓ          : < 50 µA   (teljes node deep sleep, LED nélkül, HX711 power-downban)
 *     - ELFOGADHATÓ : < 100–200 µA
 *     - PROBLÉMÁS   : > 500 µA  -> board / HX711 / tápút hibakeresés
 *
 * BOARD / PINOUT — webről ellenőrizve (NEM emlékezetből), 2026-06-28:
 *   ESP32-C3 SuperMini:
 *     - Kivezetett GPIO-k: GPIO0..GPIO10, GPIO20, GPIO21.
 *     - ADC-képes lábak: GPIO0..GPIO4 (A0..A4).
 *     - Default I2C: SDA=GPIO8, SCL=GPIO9 (itt nem használjuk).
 *     - Default SPI: SCK=GPIO4, MISO=GPIO5, MOSI=GPIO6, SS=GPIO7 (itt nem használjuk).
 *     - Strapping lábak (kerülendők általános IO-nak): GPIO2, GPIO8, GPIO9.
 *     - USB-JTAG/soros: GPIO18 (D-), GPIO19 (D+) — a C3-on a beépített USB-Serial-JTAG.
 *     - UART0: TX=GPIO21, RX=GPIO20.
 *     - KÉK user LED  : GPIO8 (a legtöbb darabon; egyes gyártóknál GPIO2/7/9 lehet,
 *                       és tipikusan AKTÍV-ALACSONY/invertált — de a deep-sleep mérésnél
 *                       a LED-et eleve KIKAPCSOLVA tartjuk, így a polaritás itt indifferens).
 *     - VÖRÖS power LED: NEM GPIO-vezérelt, a board tápra kapcsolásakor MINDIG világít ->
 *                       ez a "rejtett fogyasztó", amit a H1 mérési terv szerint LE KELL
 *                       SZEDNI és külön mérni a hatását (lásd meresi_setup.md / CSV).
 *     - Tipikus gyári deep-sleep áram: ~43 µA (gyártói adat, ellenőrzendő a mérésen).
 *   Forrás: espboards.dev/esp32/esp32-c3-super-mini/, mozelectronics.com,
 *           lastminuteengineers.com ESP32-C3 Super Mini pinout (2026-06-28).
 *
 * HX711 BEKÖTÉS (lásd meresi_setup.md):
 *   - PD_SCK : a HX711 órajel-bemenete. >60 µs MAGAS = power-down (a load cell excitation
 *              lekapcsol, modul nyugalmi áram ~1 µA jó modulnál). Itt MAGASRA húzzuk, és
 *              deep sleep alatt is magasan TARTJUK (GPIO hold), hogy a HX711 ténylegesen
 *              aludjon a teljes node-áram mérése közben. (Az excitation 0V igazolása H2.)
 *   - DOUT   : a HX711 adat-kimenete. Itt nem olvassuk; bemenetnek hagyjuk, hogy ne
 *              hajtsuk ütközésbe a modul kimenetét.
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us() — explicit include kell (esp_rom komponens) */
#include "soc/soc_caps.h"           /* SOC_USB_SERIAL_JTAG_SUPPORTED */
#include "esp_private/sleep_console.h" /* sleep_console_usj_pad_backup_and_disable() */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/*
 * Wakeup-periódus.
 *   DEEP_SLEEP_WAKEUP_SEC == 0  -> NINCS timer-wakeup: tartós (örök) deep sleep.
 *       Ez az ALAPÉRTELMEZÉS a tiszta nyugalmi áram méréséhez — a board addig alszik,
 *       amíg manuálisan nem resetelsz. Így a multiméter stabil deep-sleep áramot mutat.
 *   DEEP_SLEEP_WAKEUP_SEC  > 0  -> timer-wakeup ennyi másodperc után (ciklikus mérés /
 *       későbbi periodikus üzem szimulálása). A wakeup után a firmware újraindul a
 *       app_main()-ből (deep sleep = teljes reboot a C3-on).
 */
#define DEEP_SLEEP_WAKEUP_SEC   0

/*
 * HX711 GPIO-k. Általános IO, NEM strapping, nincs USB/UART/SPI-flash ütközés.
 *   PD_SCK = GPIO4, DOUT = GPIO3 — a casing tényleges bekötése (lásd node_config.h).
 *   A deep sleep alatti HIGH-tartást a C3-on a gpio_hold_en() + gpio_deep_sleep_hold_en()
 *   biztosítja — a C3-nak NINCS RTCIO perifériája (SOC_RTCIO_PIN_COUNT=0), így rtc_gpio_*
 *   nem elérhető. GPIO4 azért is jó választás, mert GPIO0..5 a VDD3P3_RTC tartományba esik
 *   (ez számít, ha később GPIO-wakeupot is akarnánk deep sleepből).
 */
#define HX711_PD_SCK_GPIO       GPIO_NUM_4   /* HX711 PD_SCK (power-down clock) */
#define HX711_DOUT_GPIO         GPIO_NUM_3   /* HX711 DOUT  (adat, itt nem olvassuk) */

/*
 * A CASINGBE ÉPÍTETT, TELJES node TOVÁBBI aktív lábai (ezeket az eredeti H1-váz NEM kezelte,
 * mert csak a csupasz boardot mérte — de a teljes elektronikán KRITIKUSAK az alvóáramra).
 * Egyeznek a firmware/node/main/node_config.h bekötésével.
 *   - VBAT_SW_GPIO (GPIO10): a 10k/10k vbat-osztó low-side NPN-kapcsolójának bázisa. HA a láb
 *     alvás közben LEBEG, az NPN részben nyithat -> az osztó folyamatosan szivárog az akkusínről
 *     (~3,7 V / 20 kΩ ≈ 185 µA). Ezért alvásra EXPLICIT LOW + GPIO-hold: az NPN biztosan ZÁR.
 *   - DS18B20_GPIO (GPIO7): 1-Wire adatláb (HW 4,7k felhúzóval). Itt nincs bekötve hőmérő /
 *     nem olvassuk -> bemenetnek hagyjuk (a felhúzót nem hajtjuk).
 */
#define VBAT_SW_GPIO            GPIO_NUM_10  /* vbat-osztó NPN bázis: LOW = osztó lekapcsol */
#define DS18B20_GPIO            GPIO_NUM_7   /* DS18B20 1-Wire (itt nem használt) */

/*
 * Beépített kék user LED. A deep-sleep mérésnél KIKAPCSOLVA tartjuk.
 *   A legtöbb ESP32-C3 SuperMini-n GPIO8 (és tipikusan aktív-alacsony). A vörös power
 *   LED nem GPIO-vezérelt -> azt HW-ben kell leszedni (lásd meresi_setup.md).
 */
#define USER_LED_GPIO           GPIO_NUM_8

/* Mennyi ideig világítson a "még élek" jelzés ébredéskor, mielőtt aludni megy (ms).
 * 0 = egyáltalán ne villantson (legtisztább mérés). Csak debug-vizuálishoz hasznos. */
#define BOOT_BLINK_MS           0

/* ================================================================= */

static const char *TAG = "H1";

/*
 * A HX711-et power-downba teszi: PD_SCK GPIO-t MAGASra húzza.
 * A HX711 adatlapja szerint PD_SCK > 60 µs magas -> a chip power-downba megy,
 * a load cell excitation lekapcsol. Deep sleep alatt GPIO hold tartja a szintet.
 */
static void hx711_power_down(void)
{
    gpio_config_t pdsck_cfg = {
        .pin_bit_mask = (1ULL << HX711_PD_SCK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pdsck_cfg));
    ESP_ERROR_CHECK(gpio_set_level(HX711_PD_SCK_GPIO, 1));   /* HIGH = power-down */

    /* DOUT bemenetnek, felhúzás nélkül — nem hajtjuk a modul kimenetét. */
    gpio_config_t dout_cfg = {
        .pin_bit_mask = (1ULL << HX711_DOUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout_cfg));

    /* Tartsuk legalább 60 µs-ig magasan, hogy a HX711 biztosan power-downba menjen. */
    esp_rom_delay_us(100);

    ESP_LOGI(TAG, "HX711 power-down: PD_SCK(GPIO%d)=HIGH, DOUT(GPIO%d)=input",
             HX711_PD_SCK_GPIO, HX711_DOUT_GPIO);
}

/*
 * A teljes node TOVÁBBI perifériáinak alvásbarát beállítása (a HX711-en/LED-en túl).
 * KRITIKUS a deep-sleep áramra: a vbat-osztó NPN-kapcsolóját (GPIO10) EXPLICIT LOW-ra hajtjuk,
 * hogy az NPN biztosan ZÁRJON és a 10k/10k osztó ne szivárogjon (a lebegő bázis részleges
 * nyitást és ~185 µA szivárgást okozhat). A DS18B20-lábat bemenetnek hagyjuk (HW felhúzó).
 * A GPIO10 LOW szintjének deep sleep alatti megtartását a hold_critical_gpios_for_sleep() végzi.
 */
static void extra_peripherals_sleep_safe(void)
{
    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << VBAT_SW_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_cfg));
    ESP_ERROR_CHECK(gpio_set_level(VBAT_SW_GPIO, 0));   /* NPN OFF -> osztó lekapcsol */

    /* DS18B20 adatláb bemenetnek, felhúzás nélkül (a felhúzó a HW-en van, nem hajtjuk). */
    gpio_config_t onewire_cfg = {
        .pin_bit_mask = (1ULL << DS18B20_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&onewire_cfg));

    ESP_LOGI(TAG, "Extra periferiak alvasra: vbat NPN(GPIO%d)=LOW (oszto le), DS18B20(GPIO%d)=input",
             VBAT_SW_GPIO, DS18B20_GPIO);
}

/*
 * A kék user LED-et biztosan kikapcsolja (a deep-sleep mérés alatt sötét).
 * A polaritás darabfüggő lehet; itt OUTPUT-ra állítjuk és kioltjuk mindkét feltevés
 * szerint biztonságosan: a leggyakoribb aktív-alacsony esetben HIGH = LED off.
 */
static void user_led_off(void)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << USER_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    gpio_set_level(USER_LED_GPIO, 1);   /* aktív-alacsony LED esetén: off */

#if BOOT_BLINK_MS > 0
    /* Opcionális "még élek" villantás (csak ha BOOT_BLINK_MS > 0). */
    gpio_set_level(USER_LED_GPIO, 0);   /* on */
    vTaskDelay(pdMS_TO_TICKS(BOOT_BLINK_MS));
    gpio_set_level(USER_LED_GPIO, 1);   /* off */
#endif
}

/*
 * Deep sleep alatt megőrzendő GPIO-szintek rögzítése (GPIO hold), hogy a HX711
 * PD_SCK MAGAS maradjon és a LED kikapcsolva — különben deep sleepben a digitális IO
 * elengedne, és a HX711 felébredhetne / a node-áram megugorhatna.
 *
 * Az ESP32-C3-nak NINCS RTCIO perifériája (SOC_RTCIO_PIN_COUNT=0), így a klasszikus
 * rtc_gpio_hold_en() nem elérhető C3 targetre. A digitális GPIO-szint deep sleep alatti
 * befagyasztása a sima gpio_hold_en() (lábanként) + a globális gpio_deep_sleep_hold_en()
 * párossal történik. (Forrás: ESP-IDF GPIO / Sleep Modes dok., ESP32-C3.)
 */
static void hold_critical_gpios_for_sleep(void)
{
    gpio_hold_en(HX711_PD_SCK_GPIO);   /* PD_SCK aktuális (HIGH) szintje befagyasztva */
    gpio_hold_en(USER_LED_GPIO);       /* LED off szintje befagyasztva */
    gpio_hold_en(VBAT_SW_GPIO);        /* vbat NPN bázis LOW szintje befagyasztva -> osztó zárva marad */

    /* A holdok deep sleep alatt is éljenek (globális engedély). */
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "GPIO hold: PD_SCK(GPIO%d) HIGH, LED(GPIO%d) off, vbat NPN(GPIO%d) LOW megtartva alvas alatt",
             HX711_PD_SCK_GPIO, USER_LED_GPIO, VBAT_SW_GPIO);
}

void app_main(void)
{
    /* Miért ébredtünk? (időmért ciklusnál hasznos a logban.) */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "=== H1 node deep-sleep mero firmware ===");
    ESP_LOGI(TAG, "Wakeup cause: %d (0 = power-on/reset)", (int)cause);

    /* 1) LED ki — semmi fény a mérés alatt. */
    user_led_off();

    /* 2) HX711 power-down — PD_SCK magas, DOUT bemenet. */
    hx711_power_down();

    /* 2b) A teljes node többi lába alvásra: vbat-osztó NPN LE (GPIO10 LOW), DS18B20 input.
     *     Enélkül a lebegő NPN-bázis miatt az osztó szivárog -> hamis deep-sleep áram. */
    extra_peripherals_sleep_safe();

    /*
     * 3) Alvásbarát GPIO-konfig: minden NEM használt, kivezetett GPIO maradjon
     *    a reset utáni nagy-impedanciás (input, pull nélkül) állapotban. A reset
     *    default a C3-on már input + a belső pull-ok jellemzően tiltottak, így a
     *    legkisebb szivárgás érdekében NEM kapcsolunk be felhúzásokat. A két
     *    HX711-lábat fent már explicit beállítottuk. Tudatosan nem piszkáljuk a
     *    GPIO18/19 (USB) és GPIO20/21 (UART) lábakat, hogy a soros/USB elérés
     *    megmaradjon a fejlesztéshez; ezek hatása a mérésen ellenőrizendő.
     */

    /* 4) Kritikus szintek rögzítése alvásra (PD_SCK HIGH hold). */
    hold_critical_gpios_for_sleep();

    /* 5) Wakeup-forrás konfig. */
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
     * 6) USB-Serial-JTAG pad LEKAPCSOLÁSA deep sleephez.
     *    KRITIKUS a fogyasztásra: a C3-on a USJ pad/USB-PHY deep sleepben is bekapcsolva
     *    marad (~500–800 µA!), és az IDF ezt CSAK light sleepnél kapcsolja le automatikusan,
     *    deep sleepnél NEM. Mivel a console-t a natív USB-Serial-JTAG-ra állítottuk (boot-log
     *    USB-n), ez tartotta ébren a padot -> ez a mért ~731 µA fő oka. Itt explicit lekapcsoljuk.
     *    (Reset-ből ébredünk deep sleep után, ezért a pad-restore nem szükséges.)
     */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif

    /* 7) Deep sleep — innen nincs visszatérés; wakeup esetén app_main() újraindul. */
    esp_deep_sleep_start();
}

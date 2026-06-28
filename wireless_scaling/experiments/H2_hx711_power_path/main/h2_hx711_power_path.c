/*
 * H2 — HX711 power-path (excitation-lekapcsolás)
 * bee_scale / wireless_scaling · 2. fázis · experiments/H2_hx711_power_path
 *
 * CÉL
 *   A HX711-M mérőerősítő-MODUL nyugalmi (power-down) áramának MÉRHETŐVÉ tétele, ÉS annak
 *   igazolása, hogy alvás közben a load cell GERJESZTÉSE (excitation) ténylegesen lekapcsol
 *   (0 V a cellán). Szemben a H1-gyel — ahol HX711 modul NÉLKÜL mértük a node nyers
 *   deep-sleep áramát — itt a HX711-M modul ÉS a load cell ténylegesen BE VAN KÖTVE: a chipet
 *   power-downba küldjük (PD_SCK magas + GPIO hold), majd deep sleep. A firmware MINIMÁLIS és
 *   a MÉRÉST szolgálja: nincs rádió, nincs HX711-olvasás — csak power-down + alvásbarát
 *   GPIO-konfig + esp_deep_sleep.
 *
 *   A pass/fail küszöb (lásd README pass/fail tábla) — a pass CSAK akkor áll, ha MINDHÁROM:
 *     - HX711 nyugalmi áram          : < ~5 µA  (a modul VCC ágában mérve — lásd meresi_setup.md)
 *     - Load cell excitation alvásban : ~0 V    (E+/E− között — ténylegesen lekapcsol)
 *     - Tápút / LED                  : nincs szivárgó tápút, nincs LED
 *   Jó modul ~1 µA, olcsó/atipikus ~120 µA (kohacraft). A leggyakoribb valós hiba NEM a modul,
 *   hanem hogy a PD_SCK nem marad magasan deep sleepben — ezért tartjuk hold-dal (lásd lent).
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
 *                       a node "rejtett fogyasztója" (a H1-ben leszedve/külön mérve).
 *     - Tipikus gyári deep-sleep áram: ~43 µA (gyártói adat). A H1-ben mért node-alapszám
 *                       ~185 µA (AMS1117 LDO + GPIO/RTC) — ehhez jön a HX711 modul árama.
 *   Forrás: espboards.dev/esp32/esp32-c3-super-mini/, mozelectronics.com,
 *           lastminuteengineers.com ESP32-C3 Super Mini pinout (2026-06-28).
 *
 * HX711 BEKÖTÉS (lásd meresi_setup.md) — H2-ben a modul TÉNYLEGESEN csatlakoztatva, load cellel:
 *   - PD_SCK : a HX711 órajel-bemenete. >60 µs MAGAS = power-down (a load cell excitation
 *              lekapcsol -> a cellán ~0 V; modul nyugalmi áram ~1 µA jó modulnál). Itt MAGASRA
 *              húzzuk, és deep sleep alatt is magasan TARTJUK (GPIO hold), hogy a HX711
 *              ténylegesen aludjon. Ha a PD_SCK elengedne -> a chip kiesik power-downból ->
 *              a cellán FESZÜLTSÉG marad + 100+ µA (a leggyakoribb valós hiba).
 *   - DOUT   : a HX711 adat-kimenete. Itt nem olvassuk; bemenetnek hagyjuk, hogy ne
 *              hajtsuk ütközésbe a modul kimenetét.
 *   - A load cell (HBS-SCL-50) 4 vezetékkel a modulra köt: E+/E− (gerjesztés), A+/A− (jel).
 *     Az excitation-mérés (B) a load cell E+/E− pontjain történik — lásd meresi_setup.md.
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
 * MÉRÉSI ÜZEMMÓD — a firmware fő kapcsolója.
 *   H2_MEASURE_MODE == 0  -> DEEP-SLEEP mód (alap): a HX711-et power-downba teszi
 *       (+ opc. excitation-probe), majd a node TARTÓS deep sleepbe megy. Ez méri az
 *       (A) HX711 nyugalmi áramot és a (B) excitation-lekapcsolást (cella ~0 V alvásban).
 *   H2_MEASURE_MODE == 1  -> AKTÍV-CIKLUS mód: az ESP ÉBREN marad (NINCS deep sleep, NINCS
 *       USB-JTAG pad off -> a soros log végig él), és a HX711-et ciklikusan POWER-UP
 *       (HX711_POWERUP_MS ideig aktív, excitation BE) és POWER-DOWN (HX711_POWERDOWN_MS
 *       ideig) állapotba kapcsolja, a végtelenségig. CÉL: a teljes node-áramot (overall
 *       fogyasztás) figyelve a powered-up ablakokban látszik, mennyit visz a HX711 modul
 *       AKTÍVAN (a powered-up vs. power-down áram különbsége ~ a HX711 üzemi fogyasztása,
 *       tipikusan ~1.5 mA). A node-áramot itt a szokásos táp-ágban (5V vagy 3V3) méred.
 */
#define H2_MEASURE_MODE         1

/*
 * AKTÍV-CIKLUS mód (H2_MEASURE_MODE == 1) ablakai. Az ESP ébren ezeket ismétli:
 *   HX711_POWERUP_MS   : meddig legyen a HX711 POWER-UP (PD_SCK LOW, aktív) — leolvasható
 *                        ablak a DMM-en (alapból 10 s).
 *   HX711_POWERDOWN_MS : meddig legyen POWER-DOWN (PD_SCK HIGH) az egyes power-up ablakok
 *                        közt (alapból 10 ms — ez a DMM-en NEM leolvasható, csak gyors
 *                        átkapcsolás; ha a power-down áramot is le akarod olvasni, növeld
 *                        ezt is több másodpercre).
 */
#define HX711_POWERUP_MS        10000   /* HX711 powered-up ablak (ms) */
#define HX711_POWERDOWN_MS      10000   /* HX711 power-down ablak (ms) — szimmetrikus, hogy a
                                         * DMM-en a leesés is jól látszódjon (10 ms-nál a ~0.1%
                                         * duty miatt a power-down láthatatlan volt) */

/*
 * Wakeup-periódus.
 *   DEEP_SLEEP_WAKEUP_SEC == 0  -> NINCS timer-wakeup: tartós (örök) deep sleep.
 *       Ez az ALAPÉRTELMEZÉS a tiszta nyugalmi áram / excitation-méréshez — a board addig
 *       alszik, amíg manuálisan nem resetelsz. Így a multiméter stabil deep-sleep áramot és
 *       a cellán stabil ~0 V-ot mutat.
 *   DEEP_SLEEP_WAKEUP_SEC  > 0  -> timer-wakeup ennyi másodperc után (ciklikus mérés /
 *       későbbi periodikus üzem szimulálása). A wakeup után a firmware újraindul a
 *       app_main()-ből (deep sleep = teljes reboot a C3-on). Flasheléshez kényelmesebb
 *       (periodikusan ébred -> enumerál USB-n), lásd meresi_setup.md.
 */
#define DEEP_SLEEP_WAKEUP_SEC   0

/*
 * HX711 GPIO-k. Általános IO, NEM strapping, nincs USB/UART/SPI-flash ütközés.
 *   PD_SCK = GPIO5, DOUT = GPIO6 — a H1 pin-terv, NE változtasd (a két chunk azonos bekötés).
 *   A deep sleep alatti HIGH-tartást a C3-on a gpio_hold_en() + gpio_deep_sleep_hold_en()
 *   biztosítja — a C3-nak NINCS RTCIO perifériája (SOC_RTCIO_PIN_COUNT=0), így rtc_gpio_*
 *   nem elérhető. GPIO5 azért is jó választás, mert GPIO0..5 a VDD3P3_RTC tartományba esik
 *   (ez számít, ha később GPIO-wakeupot is akarnánk deep sleepből).
 */
#define HX711_PD_SCK_GPIO       GPIO_NUM_5   /* HX711 PD_SCK (power-down clock) */
#define HX711_DOUT_GPIO         GPIO_NUM_6   /* HX711 DOUT  (adat, itt nem olvassuk) */

/*
 * Beépített kék user LED. A deep-sleep mérésnél KIKAPCSOLVA tartjuk.
 *   A legtöbb ESP32-C3 SuperMini-n GPIO8 (és tipikusan aktív-alacsony). A vörös power
 *   LED nem GPIO-vezérelt -> azt HW-ben kell leszedni (lásd meresi_setup.md).
 */
#define USER_LED_GPIO           GPIO_NUM_8

/* Mennyi ideig világítson a "még élek" jelzés ébredéskor, mielőtt aludni megy (ms).
 * 0 = egyáltalán ne villantson (legtisztább mérés). Csak debug-vizuálishoz hasznos. */
#define BOOT_BLINK_MS           0

/*
 * Excitation-probe ablak (ms) — a (B) mérés "ébren" referenciájához.
 *   EXCITATION_PROBE_MS == 0  -> NINCS probe-ablak: a boot legelején AZONNAL power-down.
 *       Ez a legtisztább az (A) áram-méréshez (a modul VCC ágában mért nyugalmi áramhoz).
 *   EXCITATION_PROBE_MS  > 0  -> a power-down ELŐTT ennyi ms-ig AKTÍVAN tartja a HX711-et
 *       (PD_SCK = GPIO5 OUTPUT LOW, órajel-impulzus NÉLKÜL -> a chip normál módban, az
 *       excitation BE), hogy a felhasználó a DMM-mel megmérhesse az "ébren" E+/E− feszültséget
 *       (várt > 0 V). Ez adja a lekapcsolás "be" referenciáját: a puszta ~0 V alvásban egy
 *       szakadt/rosszul bekötött cellánál is teljesülne -> az önmagában NEM bizonyít lekapcsolást.
 *   A (B) excitation-kontrollhoz pl. 30000 (30 s), majd újraflash; az (A) áram-méréshez 0.
 *   A két mérés tehát KÉT KÜLÖN flasheléssel (külön paraméterrel) történik — lásd meresi_setup.md.
 *   FONTOS: a probe alatt CSAK PD_SCK LOW van, SCK-pulzálás (órajel) NINCS -> a HX711 nem ad
 *   adatot / nem vált gain-t; nekünk csak az aktív excitation kell. A DOUT (GPIO6) input marad.
 */
#define EXCITATION_PROBE_MS     0

/* ================================================================= */

static const char *TAG = "H2";

#if H2_MEASURE_MODE == 0
/*
 * A HX711-et power-downba teszi: PD_SCK GPIO-t MAGASra húzza.
 * A HX711 adatlapja szerint PD_SCK > 60 µs magas -> a chip power-downba megy, a chipen belüli
 * AVDD-regulátor leáll, így a load cell excitation (E+/E−) lekapcsol -> a cellán ~0 V.
 * A modul nyugalmi árama jó modulnál ~1 µA. Deep sleep alatt GPIO hold tartja a szintet.
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

    ESP_LOGI(TAG, "HX711 power-down: PD_SCK(GPIO%d)=HIGH, DOUT(GPIO%d)=input (excitation lekapcsol -> cella ~0 V)",
             HX711_PD_SCK_GPIO, HX711_DOUT_GPIO);
}
#endif /* H2_MEASURE_MODE == 0 */

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

#if H2_MEASURE_MODE == 0
/*
 * Deep sleep alatt megőrzendő GPIO-szintek rögzítése (GPIO hold), hogy a HX711
 * PD_SCK MAGAS maradjon és a LED kikapcsolva — különben deep sleepben a digitális IO
 * elengedne, a HX711 kiesne power-downból (a cellán FESZÜLTSÉG maradna + 100+ µA), és a
 * node-áram megugorhatna. Ez a H2 LÉNYEGE: a PD_SCK magasan tartása = excitation lekapcsolva.
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

    /* A holdok deep sleep alatt is éljenek (globális engedély). */
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "GPIO hold: PD_SCK(GPIO%d) HIGH es LED(GPIO%d) megtartva alvas alatt",
             HX711_PD_SCK_GPIO, USER_LED_GPIO);
}

/*
 * Excitation-probe: a power-down ELŐTT AKTÍVRA állítja a HX711-et, hogy a felhasználó
 * megmérhesse az "ébren" E+/E− feszültséget (a lekapcsolás "be" referenciája).
 * A PD_SCK-t (GPIO5) OUTPUT LOW-ra állítja — órajel-impulzus NÉLKÜL: a HX711 normál módban,
 * a chipen belüli AVDD-regulátor (excitation) BE. SCK-t NEM pulzálunk, így a chip nem ad
 * adatot / nem vált gain-t; csak az aktív gerjesztés kell. A DOUT (GPIO6) input marad.
 * Csak akkor fordul le és hívódik, ha EXCITATION_PROBE_MS > 0 (különben -Wunused-function).
 */
#if EXCITATION_PROBE_MS > 0
static void excitation_probe(void)
{
    gpio_config_t pdsck_cfg = {
        .pin_bit_mask = (1ULL << HX711_PD_SCK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pdsck_cfg));
    ESP_ERROR_CHECK(gpio_set_level(HX711_PD_SCK_GPIO, 0));   /* LOW = HX711 aktiv (excitation BE) */

    /* DOUT bemenetnek, felhúzás nélkül — nem hajtjuk a modul kimenetét. */
    gpio_config_t dout_cfg = {
        .pin_bit_mask = (1ULL << HX711_DOUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout_cfg));

    ESP_LOGI(TAG, "EXCITATION PROBE: HX711 AKTIV (PD_SCK(GPIO%d)=LOW), mERd most az E+/E- feszultseget; %d ms mulva power-down",
             HX711_PD_SCK_GPIO, EXCITATION_PROBE_MS);
    vTaskDelay(pdMS_TO_TICKS(EXCITATION_PROBE_MS));
}
#endif /* EXCITATION_PROBE_MS > 0 */
#endif /* H2_MEASURE_MODE == 0 */

#if H2_MEASURE_MODE == 1
/*
 * AKTÍV-CIKLUS: az ESP ÉBREN, a HX711-et a végtelenségig power-up / power-down ciklusban
 * kapcsolja. A teljes node-áramot (overall fogyasztás) figyelve a HX711_POWERUP_MS hosszú
 * powered-up ablakokban látszik a HX711 AKTÍV fogyasztása (a power-down ablakhoz képesti
 * többlet ~ a HX711 üzemi árama, tipikusan ~1.5 mA).
 *   - POWER-UP   : PD_SCK = LOW, órajel-impulzus NÉLKÜL -> a chip normál módban, az excitation
 *                  BE; NEM olvasunk adatot (nem pulzálunk SCK-t), csak az aktív tápállapot kell.
 *   - POWER-DOWN : PD_SCK = HIGH (> 60 µs) -> a chip power-downba megy, excitation lekapcsol.
 * NINCS deep sleep és NINCS USB-JTAG pad off -> a soros log végig elérhető (monitorozható).
 * A DOUT (GPIO6) végig input. Ez a függvény nem tér vissza.
 */
static void hx711_active_cycle(void)
{
    /* PD_SCK output, DOUT input — egyszer beállítva, a loop csak a szintet váltja. */
    gpio_config_t pdsck_cfg = {
        .pin_bit_mask = (1ULL << HX711_PD_SCK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pdsck_cfg));

    gpio_config_t dout_cfg = {
        .pin_bit_mask = (1ULL << HX711_DOUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout_cfg));

    ESP_LOGI(TAG, "AKTIV-CIKLUS: HX711 power-up %d ms / power-down %d ms, az ESP EBREN marad. Merd az overall node-aramot.",
             HX711_POWERUP_MS, HX711_POWERDOWN_MS);

    while (1) {
        /* POWER-UP: PD_SCK LOW -> HX711 aktiv, excitation BE. */
        gpio_set_level(HX711_PD_SCK_GPIO, 0);
        ESP_LOGI(TAG, "HX711 POWER-UP (PD_SCK(GPIO%d)=LOW) -> %d ms aktiv; ez a node-aram tetejen latszik.",
                 HX711_PD_SCK_GPIO, HX711_POWERUP_MS);
        vTaskDelay(pdMS_TO_TICKS(HX711_POWERUP_MS));

        /* POWER-DOWN: PD_SCK HIGH > 60 µs -> HX711 power-down, excitation lekapcsol. */
        gpio_set_level(HX711_PD_SCK_GPIO, 1);
        ESP_LOGI(TAG, "HX711 POWER-DOWN (PD_SCK(GPIO%d)=HIGH) -> %d ms.",
                 HX711_PD_SCK_GPIO, HX711_POWERDOWN_MS);
        vTaskDelay(pdMS_TO_TICKS(HX711_POWERDOWN_MS));
    }
}
#endif /* H2_MEASURE_MODE == 1 */

void app_main(void)
{
    /* Miért ébredtünk? (időmért ciklusnál hasznos a logban.) */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "=== H2 HX711 power-path mero firmware ===");
    ESP_LOGI(TAG, "Wakeup cause: %d (0 = power-on/reset)", (int)cause);

    /* 1) LED ki — semmi fény a mérés alatt. */
    user_led_off();

#if H2_MEASURE_MODE == 1
    /* AKTÍV-CIKLUS mód: az ESP ÉBREN marad, a HX711-et power-up/power-down ciklusban
     * kapcsolja a végtelenségig; a függvény NEM tér vissza (a lenti deep-sleep út kimarad). */
    hx711_active_cycle();
#else

    /*
     * 1b) Opcionális excitation-probe: ha EXCITATION_PROBE_MS > 0, a power-down ELŐTT
     *     AKTÍVRA állítjuk a HX711-et (PD_SCK LOW), hogy a user megmérhesse az "ébren"
     *     E+/E− feszültséget. EXCITATION_PROBE_MS == 0 esetén ez kimarad -> azonnal power-down.
     */
#if EXCITATION_PROBE_MS > 0
    excitation_probe();
#endif

    /* 2) HX711 power-down — PD_SCK magas, DOUT bemenet (excitation lekapcsol). */
    hx711_power_down();

    /*
     * 3) Alvásbarát GPIO-konfig: minden NEM használt, kivezetett GPIO maradjon
     *    a reset utáni nagy-impedanciás (input, pull nélkül) állapotban. A reset
     *    default a C3-on már input + a belső pull-ok jellemzően tiltottak, így a
     *    legkisebb szivárgás érdekében NEM kapcsolunk be felhúzásokat. A két
     *    HX711-lábat fent már explicit beállítottuk. Tudatosan nem piszkáljuk a
     *    GPIO18/19 (USB) és GPIO20/21 (UART) lábakat, hogy a soros/USB elérés
     *    megmaradjon a fejlesztéshez; ezek hatása a mérésen ellenőrizendő.
     */

    /* 4) Kritikus szintek rögzítése alvásra (PD_SCK HIGH hold = excitation marad lekapcsolva). */
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

    ESP_LOGI(TAG, "Deep sleepbe lepunk. Mostantol merd: (A) HX711 VCC ag arama, (B) load cell E+/E- excitation.");
    ESP_LOGI(TAG, "(USB-Serial-JTAG pad mindjart lekapcsol -> innentol nincs USB-log.)");
    /* A log kiürítése, MIELŐTT a USB-pad lekapcsol (különben az utolsó sorok elvesznek). */
    fflush(stdout);
    esp_rom_delay_us(20000);   /* ~20 ms a soros kiírás befejezésére */

    /*
     * 6) USB-Serial-JTAG pad LEKAPCSOLÁSA deep sleephez.
     *    KRITIKUS a fogyasztásra: a C3-on a USJ pad/USB-PHY deep sleepben is bekapcsolva
     *    marad (~500–800 µA!), és az IDF ezt CSAK light sleepnél kapcsolja le automatikusan,
     *    deep sleepnél NEM. Mivel a console-t a natív USB-Serial-JTAG-ra állítottuk (boot-log
     *    USB-n), ez tartotta ébren a padot -> ez volt a H1-ben mért ~731 µA fő oka. Itt
     *    explicit lekapcsoljuk. (Reset-ből ébredünk deep sleep után, ezért a pad-restore nem
     *    szükséges.) MEGJEGYZÉS: a (A) HX711-áramot úgyis a modul SAJÁT VCC ágában mérjük,
     *    a node-áramtól külön — lásd meresi_setup.md.
     */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif

    /* 7) Deep sleep — innen nincs visszatérés; wakeup esetén app_main() újraindul. */
    esp_deep_sleep_start();
#endif /* H2_MEASURE_MODE */
}

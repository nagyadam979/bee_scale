// RG — Esőmérő reed-impulzus kísérlet (WH-SP-RG tipping bucket) · ESP32-C3 SuperMini
//
// CÉL: a billenővödör-esőmérő reed-kapcsolójának impulzusait kiolvasni és megbízhatóan
// számolni a node-on. A szenzor MINDEN 0,2794 mm csapadéknál egy PILLANATNYI kontaktus-
// zárást ad (reed short). A polaritás nem számít — a reed egy egyszerű kapcsoló.
//
// BEKÖTÉS (RJ11 két KÖZÉPSŐ ere = a reed két lába):
//   egyik ér -> RG_PIN (GPIO, belső pull-up bekapcsolva)
//   másik ér -> GND
//   Nyugalomban a pin HIGH; billenéskor a reed zárja -> a pin rövid időre LOW.
//
// A KÍSÉRLET LÉNYEGI KÉRDÉSE (deep-sleep impulzusszámlálás a C3-on):
//   A C3-nak NINCS ULP-je, a PCNT hardveres számláló deep sleepben NEM számol, és a
//   deep-sleep GPIO-ébresztés SZINT-triggerelt (nem él). Két járható út:
//     (A) WAKE-ON-TIP: minden billenésre felébredünk (esp_deep_sleep_enable_gpio_wakeup,
//         LOW szintre), RTC_DATA_ATTR számlálót növelünk, debounce/lockout után vissza
//         alvásba. Nincs extra HW; ára ~192 ms/ébredés (nagy esőben ébredés-vihar).
//     (B) KÜLSŐ SZÁMLÁLÓ IC (pl. CD4520/74HC4040, ~µA nyugalmi): a billenéseket alvás
//         alatt is számolja, a node a SAJÁT (pl. 30 s-os) ébredésekor olvassa ki és
//         nullázza. Extra alkatrész, de nincs per-tip ébredés.
//   Ez a firmware MINDKETTŐ méréséhez ad módot (RG_MODE), + egy padi kalibráló/bounce módot.
//
// ÚJRAHASZNÁLT MINTA (H1): deep sleep előtt a USB-Serial-JTAG pad lekapcsolása a nyugalmi
// áram miatt; a boot-log USB-n megmarad. GPIO-hold a C3-on gpio_hold_en (nincs RTCIO).

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

// ───────────────────────── HANGOLHATÓ PARAMÉTEREK ─────────────────────────

// RG_MODE — melyik kísérletet futtatjuk:
//   0 = PADI KALIBRÁLÓ / BOUNCE (ESP ébren marad, GPIO-ISR + SW-debounce, élő számláló-log).
//       Erre kalibrálod a tényleges ml/billenés térfogatot és nézed a reed-bounce viselkedést.
//   1 = DEEP-SLEEP WAKE-ON-TIP (A út): minden billenésre ébred, RTC-számláló, vissza alvásba.
//       Ezen méred az ébredés-költséget és a deep-sleep számlálás megbízhatóságát.
#define RG_MODE 0

// A reed egyik ere ide, a másik GND-re. Kerüld a HX711 lábakat (H1: SCK=GPIO5, DT=GPIO6).
// GPIO4 szabad és deep-sleep GPIO-ébresztésre alkalmas a C3-on.
#define RG_PIN GPIO_NUM_4

// Debounce / lockout: a billenés utáni holtidő, amíg új billenést NEM fogadunk el.
// A billenővödör mechanikusan lassú (billenések közt >>100 ms), a reed-bounce ~ms-os.
// 150 ms bőven elnyeli a bounce-ot és nem veszít valós billenést (>50 mm/h esőnél is >20 s/tip).
#define RG_DEBOUNCE_MS 150

// Egy billenés csapadék-ekvivalense (adatlap: WH-SP-RG).
#define RG_MM_PER_TIP 0.2794f

// MODE 1: mennyi ideig figyeljünk ébren egy tip-ébredés után, mielőtt visszaalszunk (a
// kontaktus-nyitás + bounce-lockout bevárása). Legalább RG_DEBOUNCE_MS.
#define RG_TIP_WINDOW_MS 200

// ──────────────────────────────────────────────────────────────────────────

static const char *TAG = "RG";

// Deep sleepet túlélő állapot (MODE 1). A boot-oka alapján tudjuk, tip-ébredés volt-e.
static RTC_DATA_ATTR uint32_t rtc_tip_count = 0;   // összes billenés a bekapcsolás óta
static RTC_DATA_ATTR uint32_t rtc_boot_count = 0;  // hányszor ébredtünk (diagnosztika)

// ───────────────────────── MODE 0: padi kalibráló ─────────────────────────

static QueueHandle_t s_evt_queue = NULL;
static volatile int64_t s_last_tip_us = 0;

static void IRAM_ATTR rg_isr_handler(void *arg)
{
    // Él-ISR a lefutó élre (HIGH->LOW = reed zár). SW-debounce: az utolsó elfogadott
    // billenés óta RG_DEBOUNCE_MS-nél korábbi éleket eldobjuk (reed-bounce).
    int64_t now = esp_timer_get_time();
    if (now - s_last_tip_us < (int64_t)RG_DEBOUNCE_MS * 1000) {
        return;  // bounce -> eldob
    }
    s_last_tip_us = now;
    uint32_t evt = 1;
    xQueueSendFromISR(s_evt_queue, &evt, NULL);
}

static void run_calibration_mode(void)
{
    ESP_LOGI(TAG, "MODE 0 — padi kalibráló/bounce. Pin=GPIO%d, debounce=%d ms, %.4f mm/tip.",
             RG_PIN, RG_DEBOUNCE_MS, RG_MM_PER_TIP);
    ESP_LOGI(TAG, "Billentsd a vödröt kézzel / önts vizet. A számláló élőben nő.");

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RG_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // nyugalomban HIGH, zárás -> LOW
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,       // HIGH->LOW él
    };
    gpio_config(&io);

    s_evt_queue = xQueueCreate(16, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(RG_PIN, rg_isr_handler, NULL);

    uint32_t tips = 0;
    uint32_t evt;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(1000))) {
            tips++;
            ESP_LOGI(TAG, "TIP #%" PRIu32 "  ->  %.4f mm (össz.)", tips, tips * RG_MM_PER_TIP);
        }
    }
}

// ───────────────────── MODE 1: deep-sleep wake-on-tip ─────────────────────

// H1-minta: a USB-Serial-JTAG pad lekapcsolása deep sleep előtt (nyugalmi áram).
// A gyenge deklaráció miatt a linker akkor is elmegy, ha az adott IDF-ben nincs meg.
extern void sleep_console_usj_pad_backup_and_disable(void) __attribute__((weak));

static void run_deep_sleep_wake_on_tip(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    rtc_boot_count++;

    // Ha GPIO-ébresztésből jöttünk, az egy billenés volt. Debounce: várjuk meg, míg a
    // kontaktus visszanyit (pin HIGH) + lockout, hogy a bounce ne számoljon többször.
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        rtc_tip_count++;
        ESP_LOGI(TAG, "WAKE (tip) #%" PRIu32 "  ->  össz. tip=%" PRIu32 "  (%.4f mm)  boot=%" PRIu32,
                 rtc_tip_count, rtc_tip_count, rtc_tip_count * RG_MM_PER_TIP, rtc_boot_count);
        // Bounce-lockout + a kontaktus nyitásának bevárása.
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < (int64_t)RG_TIP_WINDOW_MS * 1000) {
            if (gpio_get_level(RG_PIN) == 1) {
                // nyitva; adjunk még egy rövid stabilizáló holtidőt
                vTaskDelay(pdMS_TO_TICKS(RG_DEBOUNCE_MS));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    } else {
        ESP_LOGI(TAG, "BOOT (nem tip: cause=%d). Kezdő tip=%" PRIu32 ", boot=%" PRIu32,
                 (int)cause, rtc_tip_count, rtc_boot_count);
    }

    // Pin újra-konfigurálása bemenetnek, pull-uppal (nyugalomban HIGH).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RG_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    // Deep-sleep GPIO-ébresztés a C3-on: SZINT-triggerelt, LOW szintre (reed zár = LOW).
    ESP_ERROR_CHECK(gpio_wakeup_enable(RG_PIN, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    // A pull-up tartása deep sleep alatt, hogy a nyitott reednél a pin biztosan HIGH
    // maradjon (különben azonnali ál-ébresztés). C3: gpio_hold_en (nincs RTCIO).
    gpio_hold_en(RG_PIN);

    ESP_LOGI(TAG, "Deep sleepbe megyek, ébresztés a következő billenésre (GPIO%d LOW)...", RG_PIN);

    if (sleep_console_usj_pad_backup_and_disable) {
        sleep_console_usj_pad_backup_and_disable();  // H1: USB-JTAG pad le a nyugalmi áramért
    }
    esp_deep_sleep_start();  // ide már nem tér vissza
}

// ─────────────────────────────────── main ───────────────────────────────────

void app_main(void)
{
#if RG_MODE == 0
    run_calibration_mode();
#else
    run_deep_sleep_wake_on_tip();
#endif
}

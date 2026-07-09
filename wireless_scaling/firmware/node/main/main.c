/*
 * main.c — a node-firmware főciklusa (fázis-szekvencer / éles egyszeri mérés)
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A C3-on a deep sleep = teljes reboot, így app_main() minden ébredéskor újraindul. A menete:
 *   1) NVS init (WiFi PHY-kalibrációhoz kell)
 *   2) RTC-állapot (cycle -> seq, boot_id) — node_sleep
 *   3) profil-függő munka:
 *        BENCH: fázis-szekvencer — baseline, összes szenzor együtt, rádió-idle, ESP-NOW TX.
 *               A szenzorok a WiFi BEKAPCSOLÁSA ELŐTT mérnek (tiszta, zajmentes táp/ADC).
 *        FIELD: egyszeri gyors mérés (HX711 + DS18B20 + vbat) -> payload -> broadcast.
 *   4) deep sleep NODE_WAKE_PERIOD_SEC-ig — node_sleep
 *
 * A main CSAK vezényel; a driver-logika a modulokban van (sens_*, espnow_tx, node_sleep).
 * A hangolható paraméterek: node_config.h.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "node_config.h"
#include "protocol.h"
#include "node_sleep.h"
#include "espnow_tx.h"
#include "sens_hx711.h"
#include "sens_vbat.h"
#include "sens_ds18b20.h"

static const char *TAG = "node";

/* NVS init — a WiFi PHY/kalibrációs adatokhoz kell. (M1-minta.) */
static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

/* Payload feltöltése a KÖZÖS wire-szerződés szerint (protocol.h). A CRC-t utolsóként számoljuk. */
static void build_payload(sz2_payload_t *p, const node_wake_state_t *st,
                          float weight, float temp, uint16_t vbat)
{
    memset(p, 0, sizeof(*p));
    p->protocol_version = SZ2_PROTO_VERSION;
    p->packet_type      = SZ2_PKT_DATA;
    p->node_id          = NODE_ID;
    espnow_tx_get_mac(p->node_mac);
    p->boot_id          = st->boot_id;
    p->seq              = st->cycle;
    p->packet_idx       = st->cycle;
    p->total_packets    = 0;   /* éles üzemben nincs fix futás-hossz; diagnosztikára szabad */
    p->send_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    p->weight           = weight;
    p->temp             = temp;
    p->vbat             = vbat;
    p->crc              = sz2_payload_crc(p);

    /* A mért adat láthatóvá tétele a soros logon (temp NAN, ha nincs bekötve hőmérő). */
    ESP_LOGI(TAG, "payload: seq=%u weight=%.0f temp=%.2f vbat=%u mV",
             (unsigned)p->seq, (double)p->weight, (double)p->temp, (unsigned)p->vbat);
}

static void user_led_set_off(void)
{
    gpio_set_level(USER_LED_GPIO, 1);   /* SuperMini kék LED tipikusan aktív-LOW. */
}

static void user_led_init(void)
{
    gpio_hold_dis(USER_LED_GPIO);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << USER_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    user_led_set_off();
    ESP_LOGI(TAG, "user_led_init: GPIO%d output, OFF=HIGH, ON=LOW", USER_LED_GPIO);
}

static void user_led_blink_once(const char *why)
{
    ESP_LOGI(TAG, "LED villanas: %s", why);
    gpio_set_level(USER_LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    user_led_set_off();
    vTaskDelay(pdMS_TO_TICKS(120));
}

#if NODE_PROFILE == NODE_PROFILE_BENCH
/* ===================== BENCH: fázis-szekvencer ===================== */

typedef enum {
    PH_BASELINE = 0,  /* minden ki: az MCU puszta ébren-alapárama (referencia) */
    PH_SENSORS_ALL,   /* HX711 + VBAT + DS18B20 együtt, egy mérési fázisban */
    PH_WIFI_IDLE,     /* WiFi/ESP-NOW init kész, de NEM küld (rádió-alapáram) */
    PH_ESPNOW_TX,     /* folyamatos broadcast (TX-csúcsok + link-teszt) */
    PH_COUNT
} bench_phase_t;

static const char *phase_name(bench_phase_t ph)
{
    switch (ph) {
        case PH_BASELINE:  return "BASELINE (minden ki)";
        case PH_SENSORS_ALL: return "SENSORS_ALL (HX711 + VBAT + DS18B20)";
        case PH_WIFI_IDLE: return "WIFI_IDLE (radio be, nem kuld)";
        case PH_ESPNOW_TX: return "ESPNOW_TX (folyamatos broadcast)";
        default:           return "?";
    }
}

/* Egy fázis lefuttatása BENCH_PHASE_SEC-ig, `sample_interval_ms`-onként a `sample` visszahívással. */
static void run_phase_for(uint32_t sample_interval_ms, void (*sample)(const node_wake_state_t *),
                          const node_wake_state_t *st)
{
    const int64_t interval_us = (int64_t)sample_interval_ms * 1000;
    const int64_t end_us = esp_timer_get_time() + (int64_t)BENCH_PHASE_MS * 1000;
    int64_t next_us = esp_timer_get_time();

    while (esp_timer_get_time() < end_us) {
        if (sample) {
            sample(st);
        }

        next_us += interval_us;
        int64_t delay_us = next_us - esp_timer_get_time();
        int64_t until_end_us = end_us - esp_timer_get_time();
        if (delay_us > until_end_us) {
            delay_us = until_end_us;
        }
        if (delay_us > 0) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(delay_us / 1000)));
        }
    }
}

/* --- fázis-mintavevő visszahívások (a szekvencer hívja ciklikusan) --- */
/* A fázis-mintavevők a nyers értéket a logra is kiírják (a DMM-lépcső mellett a mért adat is látszik). */
static void sample_sensors_all(const node_wake_state_t *st)
{
    (void)st;
    int32_t raw = hx711_read_raw();
    uint16_t vbat = vbat_sample_mv();
    float temp = ds18b20_read_temp();
    ESP_LOGI(TAG, "sensors: HX711 raw=%ld vbat=%u mV temp=%.2f C",
             (long)raw, (unsigned)vbat, (double)temp);
}

static void sample_espnow(const node_wake_state_t *st)
{
    /* Friss mérés -> payload -> broadcast. A HX711-et a leolvasáshoz fel kell hozni (a HX711-fázis
     * végén power-downba került); a vbat_read_mv maga kapcsolja az osztót. A cél a link-terhelés is. */
    hx711_power_up();
    float weight = hx711_read_weight();
    hx711_power_down();

    sz2_payload_t p;
    build_payload(&p, st, weight, ds18b20_read_temp(), vbat_read_mv());
    espnow_tx_send(&p, NODE_TX_REPEAT);
}

static void run_bench(const node_wake_state_t *st)
{
    ESP_LOGI(TAG, "BENCH profil: %d fazis x %d ms ebrenlet, majd %d s alvas.",
             (int)PH_COUNT, BENCH_PHASE_MS, NODE_WAKE_PERIOD_SEC);

    for (bench_phase_t ph = 0; ph < PH_COUNT; ph++) {
        ESP_LOGI(TAG, "=== FAZIS %d/%d: %s ===", (int)ph + 1, (int)PH_COUNT, phase_name(ph));

        switch (ph) {
            case PH_BASELINE:
                user_led_set_off();
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, NULL, st);
                break;

            case PH_SENSORS_ALL:
                hx711_power_up();
                vbat_enable();
                vTaskDelay(pdMS_TO_TICKS(VBAT_SETTLE_MS));   /* egyszeri RC-beallas */
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, sample_sensors_all, st);
                vbat_disable();
                hx711_power_down();
                break;

            case PH_WIFI_IDLE:
                espnow_tx_init();   /* rádió BE, de nem küldünk ebben a fázisban */
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, NULL, st);
                break;

            case PH_ESPNOW_TX:
                run_phase_for(BENCH_TX_INTERVAL_MS, sample_espnow, st);
                break;

            default:
                break;
        }
    }
}
#else
/* ===================== FIELD: egyszeri mérés ===================== */

static void run_field(const node_wake_state_t *st)
{
    ESP_LOGI(TAG, "FIELD profil: egyszeri meres, majd %d s alvas.", NODE_WAKE_PERIOD_SEC);

    /* Szenzorok a rádió BEKAPCSOLÁSA ELŐTT (tiszta táp/ADC). */
    hx711_power_up();
    float weight = hx711_read_weight();
    hx711_power_down();

    float temp = ds18b20_read_temp();
    uint16_t vbat = vbat_read_mv();

    /* Payload + broadcast. */
    sz2_payload_t p;
    espnow_tx_init();
    build_payload(&p, st, weight, temp, vbat);
    espnow_tx_send(&p, NODE_TX_REPEAT);
}
#endif

void app_main(void)
{
#if NODE_PROFILE == NODE_PROFILE_SLEEP_FLOOR
    ESP_LOGI(TAG, "SLEEP_FLOOR profil: HX711 power-down + minimal deep sleep %d s.", NODE_WAKE_PERIOD_SEC);
    hx711_init();
    hx711_power_down();
    node_enter_deep_sleep_minimal(NODE_WAKE_PERIOD_SEC);
#else
    nvs_init();

    node_wake_state_t st;
    node_sleep_begin_cycle(&st);
    ESP_LOGI(TAG, "=== bee_scale NODE === id=%d cycle=%u boot=0x%08X %s",
             NODE_ID, (unsigned)st.cycle, (unsigned)st.boot_id,
             st.cold_boot ? "(hideg indulas)" : "(timer-wake)");

    /* Szenzorok GPIO/perifériák init — a driver-modulok maguk konfigolnak. */
    hx711_init();
    vbat_init();
    ds18b20_init();
    user_led_init();
    user_led_blink_once(st.cold_boot ? "boot" : "timer-wake");

#if NODE_PROFILE == NODE_PROFILE_BENCH
    run_bench(&st);
#else
    run_field(&st);
#endif

    ESP_LOGI(TAG, "Ciklus kesz. Deep sleep %d s.", NODE_WAKE_PERIOD_SEC);
    user_led_blink_once("deep sleep elott");
    espnow_tx_deinit();
    vbat_prepare_sleep();
    node_enter_deep_sleep(NODE_WAKE_PERIOD_SEC);
#endif
}

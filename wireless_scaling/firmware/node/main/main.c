/*
 * main.c — a node-firmware főciklusa (fázis-szekvencer / éles egyszeri mérés)
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A C3-on a deep sleep = teljes reboot, így app_main() minden ébredéskor újraindul. A menete:
 *   1) RTC-állapot (cycle -> seq, boot_id) — node_sleep
 *   2) hidegindításkor rövid szervizablak USB flash/diagnosztikához
 *   3) NVS init + flash pending queue
 *   4) profil-függő munka:
 *        BENCH: fázis-szekvencer — baseline, összes szenzor együtt, rádió-idle, ESP-NOW TX.
 *               A szenzorok a WiFi BEKAPCSOLÁSA ELŐTT mérnek (tiszta, zajmentes táp/ADC).
 *        FIELD: egyszeri gyors mérés -> payload -> NVS commit -> WiFi/ESP-NOW -> ACK-os drain.
 *   5) deep sleep NODE_WAKE_PERIOD_SEC-ig — node_sleep
 *
 * A main CSAK vezényel; a driver-logika a modulokban van (sens_*, espnow_tx, node_sleep).
 * A hangolható paraméterek: node_config.h.
 */
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "node_config.h"
#include "protocol.h"
#include "node_sleep.h"
#include "node_buffer.h"
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

static void run_powerup_service_window(const node_wake_state_t *st)
{
#if NODE_POWERUP_SERVICE_WINDOW_SEC > 0
    if (!st->cold_boot) {
        return;
    }

    ESP_LOGW(TAG, "POWERUP_SERVICE_WINDOW start sec=%u reason=cold_boot "
                  "usb_flash_window=1 wifi=OFF",
             (unsigned)NODE_POWERUP_SERVICE_WINDOW_SEC);

    hx711_init();
    hx711_power_down();

    uint32_t remaining = NODE_POWERUP_SERVICE_WINDOW_SEC;
    while (remaining > 0) {
        uint32_t step = remaining > 5U ? 5U : remaining;
        vTaskDelay(pdMS_TO_TICKS(step * 1000U));
        remaining -= step;
        if (remaining > 0) {
            ESP_LOGI(TAG, "POWERUP_SERVICE_WINDOW remaining_s=%u",
                     (unsigned)remaining);
        }
    }

    ESP_LOGI(TAG, "POWERUP_SERVICE_WINDOW done");
#else
    (void)st;
#endif
}

/* Payload feltöltése a KÖZÖS wire-szerződés szerint (protocol.h). A CRC-t utolsóként számoljuk. */
static void build_payload(sz2_payload_t *p, const node_wake_state_t *st, uint32_t seq,
                          uint32_t sample_epoch_s, float weight, float temp, uint16_t vbat)
{
    memset(p, 0, sizeof(*p));
    p->protocol_version = SZ2_PROTO_VERSION;
    p->packet_type      = SZ2_PKT_DATA;
    p->node_id          = NODE_ID;
    espnow_tx_get_mac(p->node_mac);
    p->boot_id          = st->boot_id;
    p->seq              = seq;
    p->packet_idx       = seq;
    p->total_packets    = 0;   /* éles üzemben nincs fix futás-hossz; diagnosztikára szabad */
    p->send_timestamp_ms = sample_epoch_s;
    p->weight           = weight;
    p->temp             = temp;
    p->vbat             = vbat;
    p->crc              = sz2_payload_crc(p);

    /* A mért adat láthatóvá tétele a soros logon (temp NAN, ha nincs bekötve hőmérő). */
    ESP_LOGI(TAG, "payload: node=%u seq=%u boot=0x%08X sample_epoch_s=%u weight=%.0f temp=%.2f vbat=%u mV crc=0x%04X",
             p->node_id, (unsigned)p->seq, (unsigned)p->boot_id,
             (unsigned)p->send_timestamp_ms,
             (double)p->weight, (double)p->temp, (unsigned)p->vbat, p->crc);
}

static void build_hello(sz2_hello_t *h, const node_wake_state_t *st)
{
    memset(h, 0, sizeof(*h));
    h->protocol_version = SZ2_PROTO_VERSION;
    h->packet_type = SZ2_PKT_HELLO;
    h->node_id = NODE_ID;
    espnow_tx_get_mac(h->node_mac);
    h->boot_id = st->boot_id;
    h->hello_seq = esp_random();
    h->pending_count = node_buffer_count();
    h->next_seq = node_buffer_next_seq();
    h->wake_period_s = NODE_WAKE_PERIOD_SEC;
    h->flags = st->cold_boot ? 1U : 0U;
    h->crc = sz2_hello_crc(h);
    ESP_LOGI(TAG, "HELLO build node=%u hello_seq=%u boot=0x%08X pending=%u next_seq=%u wake=%u flags=0x%X",
             h->node_id, (unsigned)h->hello_seq, (unsigned)h->boot_id,
             (unsigned)h->pending_count, (unsigned)h->next_seq,
             (unsigned)h->wake_period_s, (unsigned)h->flags);
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
    float weight = 0.0f;
    bool hx711_ok = hx711_read_weight_checked(&weight);
    hx711_power_down();
    if (!hx711_ok) {
        ESP_LOGW(TAG, "HX711 meres hiba: weight=NAN megy a payloadba");
    }

    sz2_payload_t p;
    build_payload(&p, st, st->cycle, node_sleep_estimated_epoch_s(),
                  weight, ds18b20_read_temp(), vbat_read_mv());
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

typedef struct {
    float weight;
    float temp;
    uint16_t vbat;
    bool hx711_ok;
    bool ds18b20_started;
    uint32_t measure_ms;
} node_measurement_t;

static node_measurement_t measure_once(void)
{
    node_measurement_t m = {
        .weight = NAN,
        .temp = NAN,
        .vbat = 0,
        .hx711_ok = false,
        .ds18b20_started = false,
        .measure_ms = 0,
    };
    int64_t t0 = esp_timer_get_time();

    m.ds18b20_started = ds18b20_start_conversion();

    hx711_power_up();
    m.hx711_ok = hx711_read_weight_checked(&m.weight);
    hx711_power_down();
    if (!m.hx711_ok) {
        ESP_LOGW(TAG, "HX711 meres hiba: weight=NAN megy a payloadba");
    }

    m.vbat = vbat_read_mv();

    if (m.ds18b20_started) {
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (elapsed_ms < DS18B20_CONVERSION_MS) {
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(DS18B20_CONVERSION_MS - elapsed_ms)));
        }
        m.temp = ds18b20_read_temp_after_conversion();
    }
    if (!isfinite(m.temp)) {
        ESP_LOGW(TAG, "DS18B20 meres hiba: egyszeri blocking retry");
        m.temp = ds18b20_read_temp();
    }

    m.measure_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "MEASURE ms=%u hx711=%s ds18b20=%s started=%s weight=%.0f temp=%.2f vbat=%u",
             (unsigned)m.measure_ms, m.hx711_ok ? "OK" : "ERR",
             isfinite(m.temp) ? "OK" : "ERR",
             m.ds18b20_started ? "YES" : "NO",
             (double)m.weight, (double)m.temp, (unsigned)m.vbat);
    return m;
}

static uint32_t fallback_sleep_sec(void)
{
    uint32_t jitter = 0;
    if (NODE_SLEEP_FAIL_JITTER_MAX_SEC > 0) {
        jitter = esp_random() % (NODE_SLEEP_FAIL_JITTER_MAX_SEC + 1U);
    }
    return NODE_WAKE_PERIOD_SEC + jitter;
}

static bool ack_slot_valid(uint32_t next_slot_s)
{
    return next_slot_s >= NODE_ACK_SLOT_MIN_SEC &&
           next_slot_s <= NODE_WAKE_PERIOD_SEC + NODE_ACK_SLOT_MAX_EXTRA_SEC;
}

static void sync_epoch_from_ack(const espnow_tx_ack_result_t *ack)
{
    if (ack->clock_correction_s <= 0) {
        return;
    }

    uint32_t master_epoch_s = (uint32_t)ack->clock_correction_s;
    if (node_sleep_sync_epoch(master_epoch_s)) {
        ESP_LOGI(TAG, "CLOCK_SYNC master_epoch_s=%u", (unsigned)master_epoch_s);
    } else {
        ESP_LOGW(TAG, "CLOCK_SYNC invalid master_epoch_s=%u", (unsigned)master_epoch_s);
    }
}

static uint32_t drain_pending_records(uint32_t *next_sleep_sec)
{
    uint32_t delivered = 0;
    for (; delivered < NODE_DRAIN_MAX_RECORDS; delivered++) {
        sz2_payload_t pending;
        esp_err_t err = node_buffer_peek_oldest(&pending);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            break;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pending peek hiba: %s", esp_err_to_name(err));
            break;
        }

        espnow_tx_ack_result_t ack;
        bool ok = espnow_tx_send_with_ack(&pending, &ack);
        if (!ok) {
            ESP_LOGW(TAG, "DRAIN stop: nincs ACK seq=%u remain=%u",
                     (unsigned)pending.seq, (unsigned)node_buffer_count());
            break;
        }
        if (ack.ack_last_seq != pending.seq ||
            ack.clear_buffer_until_seq != pending.seq) {
            ESP_LOGE(TAG, "ACK contract mismatch pending=%u ack_last=%u clear_until=%u; stop, nincs torles",
                     (unsigned)pending.seq, (unsigned)ack.ack_last_seq,
                     (unsigned)ack.clear_buffer_until_seq);
            break;
        }
        sync_epoch_from_ack(&ack);
        err = node_buffer_clear_until(ack.clear_buffer_until_seq);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pending clear hiba seq=%u clear=%u: %s",
                     (unsigned)pending.seq, (unsigned)ack.clear_buffer_until_seq,
                     esp_err_to_name(err));
            break;
        }
        if (next_sleep_sec != NULL && ack_slot_valid(ack.next_slot_offset_s)) {
            *next_sleep_sec = ack.next_slot_offset_s;
        } else if (next_sleep_sec != NULL && ack.next_slot_offset_s != 0) {
            ESP_LOGW(TAG, "ACK next_slot_s figyelmen kivul: %u",
                     (unsigned)ack.next_slot_offset_s);
        }
    }
    return delivered;
}

static uint32_t run_field(const node_wake_state_t *st)
{
    ESP_LOGI(TAG, "FIELD profil: egyszeri meres, majd %d s alvas.", NODE_WAKE_PERIOD_SEC);
    uint32_t next_sleep_sec = fallback_sleep_sec();

    /* A meres es a tartos commit radiozaj nelkul, a WiFi inditasa elott tortenik. */
    uint32_t sample_epoch_s = node_sleep_estimated_epoch_s();
    node_measurement_t m = measure_once();

    uint32_t seq = node_buffer_next_seq();
    sz2_payload_t p;
    build_payload(&p, st, seq, sample_epoch_s, m.weight, m.temp, m.vbat);
    esp_err_t err = node_buffer_enqueue(&p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uj minta NEM mentheto flashbe seq=%u: %s",
                 (unsigned)seq, esp_err_to_name(err));
    }

    /* A hold mar a meres vegen aktiv, ezt a radio-fazis elott is explicit garantaljuk. */
    hx711_power_down();
    espnow_tx_init();
    if (st->cold_boot) {
        sz2_hello_t hello;
        espnow_tx_ack_result_t sync_ack;
        build_hello(&hello, st);
        if (!espnow_tx_sync_hello(&hello, &sync_ack)) {
            uint32_t retry_s = NODE_COLD_SYNC_FAIL_SLEEP_SEC;
            ESP_LOGW(TAG, "COLD_SYNC fail: minta flashben marad, rovid retry sleep %u s",
                     (unsigned)retry_s);
            return retry_s;
        }
        sync_epoch_from_ack(&sync_ack);
        if (ack_slot_valid(sync_ack.next_slot_offset_s)) {
            next_sleep_sec = sync_ack.next_slot_offset_s;
        }
        ESP_LOGI(TAG, "COLD_SYNC ok next_sleep_s=%u pending=%u",
                 (unsigned)next_sleep_sec, (unsigned)node_buffer_count());
    }

    uint32_t before = node_buffer_count();
    uint32_t delivered = drain_pending_records(&next_sleep_sec);
    ESP_LOGI(TAG, "DRAIN summary before=%u delivered=%u remain=%u cap=%u next_sleep_s=%u source=%s",
             (unsigned)before, (unsigned)delivered, (unsigned)node_buffer_count(),
             (unsigned)NODE_PENDING_CAPACITY, (unsigned)next_sleep_sec,
             delivered > 0 ? "ACK_SLOT" : "FALLBACK");
    return next_sleep_sec;
}
#endif

void app_main(void)
{
#if NODE_PROFILE == NODE_PROFILE_SLEEP_FLOOR
    node_wake_state_t st;
    node_sleep_begin_cycle(&st);
    ESP_LOGI(TAG, "=== bee_scale NODE === mode=%s id=%d ch=%u sleep=%d cycle=%u boot=0x%08X %s",
             NODE_TEST_MODE ? "TEST" : "PROD",
             NODE_ID, (unsigned)NODE_WIFI_CHANNEL, NODE_WAKE_PERIOD_SEC,
             (unsigned)st.cycle, (unsigned)st.boot_id,
             st.cold_boot ? "(hideg indulas)" : "(timer-wake)");
    run_powerup_service_window(&st);
    ESP_LOGI(TAG, "SLEEP_FLOOR profil: HX711 power-down + minimal deep sleep %d s.", NODE_WAKE_PERIOD_SEC);
    hx711_init();
    hx711_power_down();
    node_enter_deep_sleep_minimal(NODE_WAKE_PERIOD_SEC);
#else
    node_wake_state_t st;
    node_sleep_begin_cycle(&st);
    ESP_LOGI(TAG, "=== bee_scale NODE === mode=%s id=%d ch=%u sleep=%d cycle=%u boot=0x%08X %s",
             NODE_TEST_MODE ? "TEST" : "PROD",
             NODE_ID, (unsigned)NODE_WIFI_CHANNEL, NODE_WAKE_PERIOD_SEC,
             (unsigned)st.cycle, (unsigned)st.boot_id,
             st.cold_boot ? "(hideg indulas)" : "(timer-wake)");
    run_powerup_service_window(&st);

    nvs_init();
    ESP_ERROR_CHECK(node_buffer_init());

    /* Szenzorok GPIO/perifériák init — a driver-modulok maguk konfigolnak. */
    hx711_init();
    vbat_init();
    ds18b20_init();
    user_led_init();
    user_led_blink_once(st.cold_boot ? "boot" : "timer-wake");

#if NODE_PROFILE == NODE_PROFILE_BENCH
    run_bench(&st);
    uint32_t sleep_sec = NODE_WAKE_PERIOD_SEC;
#else
    uint32_t sleep_sec = run_field(&st);
#endif

    ESP_LOGI(TAG, "Ciklus kesz. Deep sleep %u s.", (unsigned)sleep_sec);
    user_led_blink_once("deep sleep elott");
    espnow_tx_deinit();
    vbat_prepare_sleep();
    hx711_power_down();
    node_enter_deep_sleep(sleep_sec);
#endif
}

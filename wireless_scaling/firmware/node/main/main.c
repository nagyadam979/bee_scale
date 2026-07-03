/*
 * main.c — a node-firmware főciklusa (fázis-szekvencer / éles egyszeri mérés)
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A C3-on a deep sleep = teljes reboot, így app_main() minden ébredéskor újraindul. A menete:
 *   1) NVS init (WiFi PHY-kalibrációhoz kell)
 *   2) RTC-állapot (cycle -> seq, boot_id) — node_sleep
 *   3) profil-függő munka:
 *        BENCH: fázis-szekvencer — minden alrendszer KÜLÖN, hosszú, egyenletes szakaszban
 *               aktív (a DMM-en komponensenként lépcső). A szenzorok a WiFi BEKAPCSOLÁSA ELŐTT
 *               mérnek (tiszta, zajmentes táp/ADC).
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
}

#if NODE_PROFILE == NODE_PROFILE_BENCH
/* ===================== BENCH: fázis-szekvencer ===================== */

typedef enum {
    PH_BASELINE = 0,  /* minden ki: az MCU puszta ébren-alapárama (referencia) */
    PH_HX711,         /* HX711 power-up + folyamatos olvasás */
    PH_VBAT,          /* NPN ON + folyamatos ADC (végig bekapcsolt osztó) */
    PH_DS18B20,       /* folyamatos hőmérséklet-konverzió */
    PH_WIFI_IDLE,     /* WiFi/ESP-NOW init kész, de NEM küld (rádió-alapáram) */
    PH_ESPNOW_TX,     /* folyamatos broadcast (TX-csúcsok + link-teszt) */
    PH_COUNT
} bench_phase_t;

static const char *phase_name(bench_phase_t ph)
{
    switch (ph) {
        case PH_BASELINE:  return "BASELINE (minden ki)";
        case PH_HX711:     return "HX711 (folyamatos olvasas)";
        case PH_VBAT:      return "VBAT (oszto be, folyamatos ADC)";
        case PH_DS18B20:   return "DS18B20 (folyamatos konverzio)";
        case PH_WIFI_IDLE: return "WIFI_IDLE (radio be, nem kuld)";
        case PH_ESPNOW_TX: return "ESPNOW_TX (folyamatos broadcast)";
        default:           return "?";
    }
}

/* Egy fázis lefuttatása BENCH_PHASE_SEC-ig, `sample_interval_ms`-onként a `sample` visszahívással. */
static void run_phase_for(uint32_t sample_interval_ms, void (*sample)(const node_wake_state_t *),
                          const node_wake_state_t *st)
{
    const uint32_t total_ms = (uint32_t)BENCH_PHASE_SEC * 1000u;
    uint32_t elapsed = 0;
    while (elapsed < total_ms) {
        if (sample) {
            sample(st);
        }
        vTaskDelay(pdMS_TO_TICKS(sample_interval_ms));
        elapsed += sample_interval_ms;
    }
}

/* --- fázis-mintavevő visszahívások (a szekvencer hívja ciklikusan) --- */
static void sample_hx711(const node_wake_state_t *st)  { (void)st; (void)hx711_read_raw(); }
static void sample_vbat(const node_wake_state_t *st)   { (void)st; (void)vbat_sample_mv(); }
static void sample_ds18b20(const node_wake_state_t *st){ (void)st; (void)ds18b20_read_temp(); }

static void sample_espnow(const node_wake_state_t *st)
{
    /* Friss (részleges) mérés -> payload -> broadcast. A BENCH-ben a szenzorok stub/valós
     * értéke megy; a cél a link-terhelés. */
    sz2_payload_t p;
    build_payload(&p, st, hx711_read_weight(), ds18b20_read_temp(), vbat_read_mv());
    espnow_tx_send(&p, NODE_TX_REPEAT);
}

static void run_bench(const node_wake_state_t *st)
{
    ESP_LOGI(TAG, "BENCH profil: %d fazis x %d s ebrenlet, majd %d s alvas.",
             (int)PH_COUNT, BENCH_PHASE_SEC, NODE_WAKE_PERIOD_SEC);

    for (bench_phase_t ph = 0; ph < PH_COUNT; ph++) {
        ESP_LOGI(TAG, "=== FAZIS %d/%d: %s ===", (int)ph + 1, (int)PH_COUNT, phase_name(ph));

        switch (ph) {
            case PH_BASELINE:
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, NULL, st);
                break;

            case PH_HX711:
                hx711_power_up();
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, sample_hx711, st);
                hx711_power_down();
                break;

            case PH_VBAT:
                vbat_enable();
                vTaskDelay(pdMS_TO_TICKS(VBAT_SETTLE_MS));   /* egyszeri RC-beallas */
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, sample_vbat, st);
                vbat_disable();
                break;

            case PH_DS18B20:
                run_phase_for(BENCH_SAMPLE_INTERVAL_MS, sample_ds18b20, st);
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

#if NODE_PROFILE == NODE_PROFILE_BENCH
    run_bench(&st);
#else
    run_field(&st);
#endif

    ESP_LOGI(TAG, "Ciklus kesz. Deep sleep %d s.", NODE_WAKE_PERIOD_SEC);
    node_enter_deep_sleep(NODE_WAKE_PERIOD_SEC);
}

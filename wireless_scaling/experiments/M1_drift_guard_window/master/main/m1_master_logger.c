/*
 * M1 — Drift-mérő (guard-window) · MASTER (beacon-logger, RX)
 * bee_scale / wireless_scaling · 2. fázis · experiments/M1_drift_guard_window/master
 *
 * CÉL
 *   Veszi a node deep-sleep beaconjait, és minden vételkor a SAJÁT órájával (esp_timer_get_time,
 *   µs) időbélyegez, majd egy gép-grep-elhető sort ír a soros logra. A két egymás utáni beacon
 *   master-oldali rx_us-különbsége vs. a node kért alvásideje (reqT_ms) = a C3 slow-clock drift.
 *   NINCS bitmap/missing/duplicate statisztika (azt az offline Python-analizátor végzi a logból).
 *   TŰZZ-ÉS-FELEJTS: a master csak passzív vevő + logger; nincs ACK, nincs slot.
 *
 * BEFAGYASZTOTT KONTRAKTUS (a Python-analizátor UGYANEZT a két sor-formátumot parse-olja):
 *   Beacon-sor (minden VALID és CRC-BAD beaconra), a token "M1RX" az IDF-prefix utáni első mező:
 *     M1RX rx_us=%lld cycle=%u node=%u boot=0x%08X reqT_ms=%u awake_ms=%u die_c=%.2f rssi=%d crc=%s
 *     (crc = "OK" vagy "BAD"; NAN die_c esetén a %.2f "nan"-t ír — ez OK)
 *   Heartbeat-sor (M1_HEARTBEAT_MS-enként, hogy lássuk: a master él beacon nélkül is):
 *     M1HB uptime_us=%lld last_cycle=%d rx_count=%u
 *     (last_cycle = az utolsó vett cycle vagy -1 ha még semmi; rx_count = összes vett VALID beacon)
 *
 * CALLBACK-FEGYELEM (az Sz2 kutatás eredménye, NEM opcionális)
 *   A recv-cb WiFi-task kontextusban fut (NEM ISR) -> sima xQueueSend(...,0). A cb CSAK rögzíti a
 *   PRECÍZ vételi időbélyeget (rx_us), kimásolja a payloadot + src_mac + rssi egy queue-elembe, és
 *   queue-ba dob. SEMMI feldolgozás/log/CRC a cb-ben. A nehéz munka a consumer-taskban (core 1).
 *
 * PHY-FIX (KRITIKUS — az Sz2 master-rel SZÓ SZERINT egyező): set_protocol 11b/g/n (LR KIZÁRVA),
 *   PS_NONE, peer_rate_config {11b, 1M_L}. Csatorna = SZ2_CHANNEL = 1. Enélkül NÉMA a link.
 *
 * BOARD: classic ESP-WROOM-32 (dual-core Xtensa, USB-UART bridge, UART0 konzol). A log háttérből
 *   OLVASHATÓ. A consumer-taskot core 1-re pinneljük, hogy a WiFi/recv-cb (core 0) ne versenyezzen.
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"

#include "sz2_protocol.h"   /* a KÖZÖS wire-header (../../common), VÁLTOZATLAN */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* WiFi/ESP-NOW csatorna. EGYEZZEN a node SZ2_CHANNEL-jével! (1..13) */
#define SZ2_CHANNEL             1

/* Vételi queue mélysége (a node M1_TX_REPEAT redundáns beaconja egy ébredésben kis burst). */
#define M1_RX_QUEUE_LEN         32

/* Heartbeat-periódus (ms): a consumer ennyi idő tétlenség után M1HB-sort ír (a queue-timeout adja). */
#define M1_HEARTBEAT_MS         15000

/* ================================================================= */

static const char *TAG = "M1-MASTER";

/* Egy vételi queue-elem: a teljes payload + a CALLBACKBEN érvényes src_mac/rssi + a PRECÍZ rx_us. */
typedef struct {
    sz2_payload_t payload;
    uint8_t       src_mac[6];
    int8_t        rssi;
    uint16_t      len;        /* a vett hossz (a consumer ellenőrzi == sizeof(payload)) */
    int64_t       rx_us;      /* a PRECÍZ vételi időbélyeg (a cb-ben rögzítve)            */
} m1_rx_item_t;

static QueueHandle_t s_rx_queue = NULL;

/* Globális számláló a heartbeathez (csak a consumer-task írja/olvassa). */
static uint32_t s_rx_count   = 0;   /* összes vett VALID (len+ver+crc OK) beacon */
static int32_t  s_last_cycle = -1;  /* az utolsó vett cycle vagy -1, ha még semmi  */

/*
 * RECV-CALLBACK. WiFi-task kontextusban fut (NEM ISR) -> sima xQueueSend(...,0). Először rögzíti a
 * PRECÍZ vételi időbélyeget (rx_us), majd kimásol + queue-ba dob. Az info->src_addr és a
 * info->rx_ctrl->rssi CSAK itt érvényes -> a queue-elembe másoljuk. SEMMI feldolgozás itt.
 */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    m1_rx_item_t item;

    item.rx_us = esp_timer_get_time();   /* a PRECÍZ vételi időbélyeg — ELSŐ dolog a cb-ben */
    item.len   = (uint16_t)len;
    if (len == (int)sizeof(sz2_payload_t)) {
        memcpy(&item.payload, data, sizeof(sz2_payload_t));
    } else {
        memset(&item.payload, 0, sizeof(sz2_payload_t));
    }
    memcpy(item.src_mac, info->src_addr, 6);
    item.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;

    /* Non-blocking: ha tele -> eldobjuk (NEM blokkolunk a WiFi-task kontextusban). */
    (void)xQueueSend(s_rx_queue, &item, 0);
}

/*
 * CONSUMER-TASK (core 1-re pinnelve): a vételi queue feldolgozása. Itt megy a NEHÉZ munka, NEM a
 * cb-ben: hossz-/verzió-/CRC-ellenőrzés, majd PONTOSAN egy M1RX-sor (CRC-BAD csomagra is, crc=BAD
 * jelzéssel). Idle-timeout (M1_HEARTBEAT_MS) -> M1HB-sor, hogy a master "él" jelzés meglegyen.
 */
static void consumer_task(void *arg)
{
    (void)arg;
    m1_rx_item_t item;

    while (1) {
        if (xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(M1_HEARTBEAT_MS)) != pdTRUE) {
            /* Heartbeat: M1_HEARTBEAT_MS óta nincs új beacon -> a master él-jelzése. */
            ESP_LOGI(TAG, "M1HB uptime_us=%lld last_cycle=%d rx_count=%u",
                     (long long)esp_timer_get_time(), (int)s_last_cycle, (unsigned)s_rx_count);
            continue;
        }

        /* Rossz hossz -> hibás csomag; CRC-t sem tudunk számolni rajta. Egy diagnosztikai sor, skip. */
        if (item.len != (uint16_t)sizeof(sz2_payload_t)) {
            ESP_LOGW(TAG, "rossz hosszusagu csomag: %u byte (vart %u) — eldobva",
                     (unsigned)item.len, (unsigned)sizeof(sz2_payload_t));
            continue;
        }

        sz2_payload_t *p = &item.payload;

        /* Verzió-szűrés (más protokoll-verziójú csomag nem értelmezhető a kontraktus szerint). */
        if (p->protocol_version != SZ2_PROTO_VERSION) {
            ESP_LOGW(TAG, "ismeretlen protocol_version=%d (vart %d) — eldobva",
                     p->protocol_version, SZ2_PROTO_VERSION);
            continue;
        }

        /* CRC: a kontraktus szerint a CRC-BAD csomagra IS írunk M1RX-sort, crc=BAD jelzéssel. */
        bool crc_ok = (sz2_payload_crc(p) == p->crc);

        ESP_LOGI(TAG,
                 "M1RX rx_us=%lld cycle=%u node=%u boot=0x%08X reqT_ms=%u awake_ms=%u die_c=%.2f rssi=%d crc=%s",
                 (long long)item.rx_us,
                 (unsigned)p->seq,                 /* cycle == seq */
                 (unsigned)p->node_id,
                 (unsigned)p->boot_id,
                 (unsigned)p->total_packets,       /* reqT_ms */
                 (unsigned)p->send_timestamp_ms,   /* awake_ms */
                 (double)p->temp,                  /* die_c (vagy nan) */
                 (int)item.rssi,
                 crc_ok ? "OK" : "BAD");

        /* A heartbeat-állapot CSAK a VALID (CRC-OK) beaconokat számolja. */
        if (crc_ok) {
            s_rx_count++;
            s_last_cycle = (int32_t)p->seq;
        }
    }
}

/*
 * WiFi + ESP-NOW init a kötelező sorrendben + a PHY-FIX (az Sz2 master-rel SZÓ SZERINT egyező):
 *   netif -> event loop -> wifi_init -> storage RAM -> mode STA -> wifi_start -> PS_NONE ->
 *   set_protocol(11b/g/n, LR KIZÁRVA) -> set_channel (START UTÁN!) -> esp_now_init ->
 *   register recv-cb -> add broadcast peer -> set_peer_rate_config(11b, 1M_L).
 */
static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* KRITIKUS a masteren: power-save ki, különben modem-sleep miatt folyamatos vételnél vesztés. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* Interop-hardening: explicit 11b/g/n, a LR (long-range) bit KIZÁRVA — az LR moduláció nem
     * interoperábilis a sima 11b/g/n-nel -> teljes vétel-némaság C3 <-> classic ESP32 között.
     * EGYEZZEN a node beállításával. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ2_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    /* Broadcast peer a szimmetriáért (vételhez peer nélkül is menne). */
    esp_now_peer_info_t peer = { 0 };
    static const uint8_t bc[ESP_NOW_ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    memcpy(peer.peer_addr, bc, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* Fix, legrobusztusabb ESP-NOW ráta (11b, 1 Mbps long-preamble) — EGYEZZEN a node-éval. */
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate    = WIFI_PHY_RATE_1M_L,
        .ersu    = false,
        .dcm     = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rc));

    /* A TÉNYLEGES vételi csatorna visszaolvasása (egyszeri, boot-kor) — csatorna-mismatch kizárása. */
    uint8_t ch_prim = 0; wifi_second_chan_t ch_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch_prim, &ch_sec);
    ESP_LOGI(TAG, "tenyleges RX csatorna prim=%u (vart SZ2_CHANNEL=%d)", ch_prim, SZ2_CHANNEL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== M1 MASTER (drift-logger, RX) — ESP-NOW beacon vetel ===");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    s_rx_queue = xQueueCreate(M1_RX_QUEUE_LEN, sizeof(m1_rx_item_t));
    configASSERT(s_rx_queue != NULL);

    wifi_espnow_init();

    /* Fejléc-sor: a board STA MAC-je + csatorna + "ready" (boot-kor). */
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "master STA MAC=%02X:%02X:%02X:%02X:%02X:%02X csatorna=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], SZ2_CHANNEL);
    ESP_LOGI(TAG, "M1 master ready (heartbeat=%d ms)", M1_HEARTBEAT_MS);

    /* Consumer a 2. magon (core 1): a WiFi/recv-cb (core 0) ne versenyezzen a feldolgozással. */
    xTaskCreatePinnedToCore(consumer_task, "m1_rx", 8192, NULL, 5, NULL, 1);
}

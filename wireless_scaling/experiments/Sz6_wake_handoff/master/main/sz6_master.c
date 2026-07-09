/*
 * Sz6 — ébredés→megszólítás→átadás protokoll · MASTER
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz6_wake_handoff/master
 *
 * CÉL
 *   Az always-on master (ESP32-S3-Zero) oldala a megbízható átadási protokollnak:
 *   veszi a node-ok DATA csomagjait, CRC-zi, seq alapján osztályoz (NEW / DUP / gap),
 *   és MINDEN érvényes csomagra app-ACK-ot küld UNICAST-tal a feladónak — a DUP-ra IS
 *   (az első ACK elveszhetett; a node addig retransmittál, amíg ACK-ot nem kap).
 *
 * SZ2-ÖRÖKSÉG (kötelező fegyelem)
 *   - recv-cb: CSAK kimásol + queue-ba dob (nem ISR, sima xQueueSend, timeout 0);
 *     a nehéz munka (CRC, statisztika, ACK-küldés, log) a consumer-taskban, core 1-en.
 *   - TX szerializáció az ACK-okra: send -> send-cb szemafor; NO_MEM -> vár-retry.
 *   - esp_wifi_set_ps(WIFI_PS_NONE): KRITIKUS a folyamatos vételhez.
 *   - PHY-interop fix: explicit 11b/g/n (LR kizárva) + peer-enkénti rate-config (11B/1M_L).
 *
 * SKÁLÁZÁS
 *   - Node-tábla node_id szerint (SZ6_MAX_NODES); a peer a src MAC-ből dinamikusan kerül
 *     fel az ESP-NOW peer-listára (ensure_peer). Az ESP-NOW peer-limit (20 titkosítatlan)
 *     bőven elég a tervezett flottának (a végleges node-szám egyelőre nyitott).
 *   - A boot_id-váltás node-resetet jelez -> az adott node seq-követése újraindul.
 *
 * KIMENET (gép-grep-elhető, a bench-kiértékelés alapja)
 *   - Minden vett csomagra:  "SZ6 RX node=.. seq=.. try=.. kind=NEW|DUP gap=.. rssi=.."
 *   - Periodikusan:          "SZ6 STAT node=.. ..." (kumulatív számlálók node-onként)
 *                            "SZ6 MSTAT uptime_s=.. rxcb=.. q_drop=.." (globális)
 *
 * BOARD: ESP32-S3-Zero, konzol a natív USB-Serial-JTAG-on. Always-on -> a port stabilan
 *   enumerál, a log háttérből gyűjthető (tools/serial_logger.py).
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_idf_version.h"

#include "protocol.h"   /* a BEFAGYASZTOTT közös wire-header (firmware/common) */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* WiFi/ESP-NOW csatorna. EGYEZZEN a node SZ6_CHANNEL-jével! (1..13) */
#define SZ6_CHANNEL             1

/* Vételi queue mélysége (burst-puffer a recv-cb és a consumer közt). */
#define SZ6_RX_QUEUE_LEN        64

/* Hány node-ot tartunk nyilván. */
#define SZ6_MAX_NODES           8

/* A send-cb szemafor timeoutja (ms) az ACK-küldésnél. */
#define SZ6_SEND_TIMEOUT_MS     50

/* ESP_ERR_ESPNOW_NO_MEM esetén ennyit várunk, majd újrapróbáljuk az ACK-ot (ms). */
#define SZ6_NO_MEM_RETRY_MS     2

/* Az ACK-ban hirdetett következő ébredési offset (s) — most csak informatív (a node fix
 * periódussal alszik); a későbbi flotta-ütemezés hordozója. */
#define SZ6_ADVERT_SLEEP_S      12

/* Periodikus STAT riport (ms). */
#define SZ6_STAT_PERIOD_MS      10000

/* ================================================================= */

static const char *TAG = "SZ6-MASTER";

/* Egy vételi queue-elem: payload + a CSAK a callbackben érvényes src MAC és RSSI kimásolva. */
typedef struct {
    sz2_payload_t payload;
    uint8_t       src_mac[6];
    int8_t        rssi;
    uint16_t      len;
} sz6_rx_item_t;

/* Node-onkénti követés + kumulatív statisztika. */
typedef struct {
    bool     in_use;
    uint8_t  node_id;
    uint8_t  mac[6];
    uint32_t boot_id;
    bool     have_seq;        /* volt-e már valid seq ebben a boot_id-ban            */
    uint32_t last_seq;        /* utolsó NEW-ként elfogadott seq                      */
    uint32_t rx_valid;        /* összes valid (CRC-helyes) vett DATA                 */
    uint32_t rx_new;          /* először látott seq                                  */
    uint32_t rx_dup;          /* retransmit-duplikátum (seq <= last_seq)             */
    uint32_t miss_cum;        /* kumulált seq-gap (végleg kimaradt mérések)          */
    uint32_t crc_err;
    uint32_t ack_sent;
    uint32_t ack_txfail;      /* ACK send-cb status FAIL (nincs MAC-ACK a node-tól)  */
    uint32_t resets;          /* boot_id-váltások száma                              */
    int32_t  rssi_min, rssi_max;
    int64_t  rssi_sum;
    uint32_t rssi_n;
    int64_t  last_rx_us;
} sz6_node_t;

static QueueHandle_t     s_rx_queue = NULL;
static SemaphoreHandle_t s_tx_done  = NULL;
static sz6_node_t        s_nodes[SZ6_MAX_NODES];

static volatile uint32_t s_q_dropped   = 0;   /* queue tele a recv-cb-ben            */
static volatile uint32_t s_diag_recv_cb = 0;  /* hányszor futott a recv_cb           */
static volatile uint32_t s_diag_len_bad = 0;
static volatile uint32_t s_diag_ver_bad = 0;
static volatile uint32_t s_ack_status_fail = 0; /* send-cb FAIL az ACK-okra (globál) */

/* SEND-CALLBACK — csak a TX-szerializációs szemafort adja (Sz2-fegyelem + verzió-guard). */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) s_ack_status_fail++;
    xSemaphoreGive(s_tx_done);
}
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) s_ack_status_fail++;
    xSemaphoreGive(s_tx_done);
}
#endif

/* RECV-CALLBACK — CSAK kimásol + queue-ba dob (WiFi-task kontextus, sima xQueueSend). */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    sz6_rx_item_t item;

    s_diag_recv_cb++;
    item.len = (uint16_t)len;
    if (len == (int)sizeof(sz2_payload_t)) {
        memcpy(&item.payload, data, sizeof(sz2_payload_t));
    } else {
        memset(&item.payload, 0, sizeof(sz2_payload_t));
    }
    memcpy(item.src_mac, info->src_addr, 6);
    item.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;

    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_q_dropped++;
    }
}

/* Peer biztosítása + a fix robusztus ráta (11b/1M_L) — az Sz2 PHY-interop fix fele. */
static void ensure_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "add_peer hiba: %s", esp_err_to_name(err));
            return;
        }
        esp_now_rate_config_t rc = {
            .phymode = WIFI_PHY_MODE_11B,
            .rate    = WIFI_PHY_RATE_1M_L,
            .ersu    = false,
            .dcm     = false,
        };
        ESP_ERROR_CHECK(esp_now_set_peer_rate_config(mac, &rc));
    }
}

/* App-ACK küldése UNICAST-tal, szerializáltan (send -> send-cb várás; NO_MEM -> vár-retry). */
static bool send_ack(const uint8_t *dest, uint8_t node_id, uint32_t seq)
{
    sz2_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.protocol_version       = SZ2_PROTO_VERSION;
    ack.packet_type            = SZ2_PKT_ACK;
    ack.node_id                = node_id;
    ack.ack_last_seq           = seq;
    ack.clear_buffer_until_seq = seq;
    ack.clock_correction_s     = 0;
    ack.next_slot_offset_s     = SZ6_ADVERT_SLEEP_S;
    ack.retry_policy           = 0;

    ensure_peer(dest);

    for (;;) {
        esp_err_t err = esp_now_send(dest, (const uint8_t *)&ack, sizeof(ack));
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(SZ6_SEND_TIMEOUT_MS));
            return true;
        } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(SZ6_NO_MEM_RETRY_MS));
        } else {
            ESP_LOGW(TAG, "ACK send hiba node=%u seq=%u: %s",
                     node_id, (unsigned)seq, esp_err_to_name(err));
            return false;
        }
    }
}

/* Node-sáv keresése/foglalása node_id alapján. */
static sz6_node_t *node_get(uint8_t node_id, const uint8_t *mac)
{
    for (int i = 0; i < SZ6_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].node_id == node_id) return &s_nodes[i];
    }
    for (int i = 0; i < SZ6_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) {
            sz6_node_t *n = &s_nodes[i];
            memset(n, 0, sizeof(*n));
            n->in_use   = true;
            n->node_id  = node_id;
            memcpy(n->mac, mac, 6);
            n->rssi_min = 127;
            n->rssi_max = -128;
            return n;
        }
    }
    return NULL;
}

/* Egy node kumulatív STAT sora — gép-grep-elhető. */
static void print_stat(const sz6_node_t *n, int64_t now_us)
{
    int rssi_avg = n->rssi_n ? (int)(n->rssi_sum / (int64_t)n->rssi_n) : 0;
    ESP_LOGI(TAG, "SZ6 STAT node=%u last_seq=%u new=%u dup=%u miss=%u crc=%u "
                  "ack=%u ackfail=%u resets=%u rssi_avg=%d age_s=%d",
             n->node_id, (unsigned)n->last_seq, (unsigned)n->rx_new, (unsigned)n->rx_dup,
             (unsigned)n->miss_cum, (unsigned)n->crc_err, (unsigned)n->ack_sent,
             (unsigned)n->ack_txfail, (unsigned)n->resets, rssi_avg,
             (int)((now_us - n->last_rx_us) / 1000000));
}

/* CONSUMER-TASK (core 1): CRC, osztályozás (NEW/DUP/gap), ACK, statisztika, periodikus STAT. */
static void consumer_task(void *arg)
{
    (void)arg;
    sz6_rx_item_t item;
    int64_t last_stat_us = esp_timer_get_time();

    while (1) {
        bool got = (xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE);

        if (got) {
            do {
                if (item.len != (uint16_t)sizeof(sz2_payload_t)) {
                    s_diag_len_bad++;
                    ESP_LOGW(TAG, "rossz hossz: %u byte (várt %u)",
                             (unsigned)item.len, (unsigned)sizeof(sz2_payload_t));
                    break;
                }
                sz2_payload_t *p = &item.payload;
                if (p->protocol_version != SZ2_PROTO_VERSION ||
                    p->packet_type != SZ2_PKT_DATA) {
                    s_diag_ver_bad++;
                    break;
                }
                sz6_node_t *n = node_get(p->node_id, item.src_mac);
                if (n == NULL) {
                    ESP_LOGW(TAG, "nincs szabad node-sáv (SZ6_MAX_NODES=%d)", SZ6_MAX_NODES);
                    break;
                }
                if (sz2_payload_crc(p) != p->crc) {
                    n->crc_err++;
                    ESP_LOGW(TAG, "CRC-hiba node=%u seq=%u", p->node_id, (unsigned)p->seq);
                    break;
                }

                /* MAC-tanulás/frissítés. */
                if (memcmp(n->mac, item.src_mac, 6) != 0) {
                    memcpy(n->mac, item.src_mac, 6);
                    ESP_LOGI(TAG, "node=%u MAC-váltás", n->node_id);
                }

                /* boot_id-váltás = node power-on reset -> seq-követés újraindul. */
                if (n->rx_valid == 0) {
                    n->boot_id = p->boot_id;
                    ESP_LOGI(TAG, "node=%u tanulva: MAC=%02X:%02X:%02X:%02X:%02X:%02X "
                                  "boot_id=0x%08X", n->node_id,
                             n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5],
                             (unsigned)p->boot_id);
                } else if (p->boot_id != n->boot_id) {
                    ESP_LOGI(TAG, "node=%u RESET (új boot_id=0x%08X) — seq-követés újraindul",
                             n->node_id, (unsigned)p->boot_id);
                    n->boot_id  = p->boot_id;
                    n->have_seq = false;
                    n->resets++;
                }

                n->rx_valid++;
                int64_t now = esp_timer_get_time();
                n->last_rx_us = now;
                if (item.rssi < n->rssi_min) n->rssi_min = item.rssi;
                if (item.rssi > n->rssi_max) n->rssi_max = item.rssi;
                n->rssi_sum += item.rssi;
                n->rssi_n++;

                /* Osztályozás: NEW (első látás) vs DUP (retransmit / elveszett ACK).
                 * gap = a mostani és az utolsó NEW seq közti kimaradt mérések száma. */
                uint32_t gap = 0;
                bool is_new;
                if (!n->have_seq) {
                    is_new = true;
                    n->have_seq = true;
                    n->last_seq = p->seq;
                } else if (p->seq > n->last_seq) {
                    is_new = true;
                    gap = p->seq - n->last_seq - 1;
                    n->miss_cum += gap;
                    n->last_seq = p->seq;
                } else {
                    is_new = false;   /* seq <= last_seq: retransmit-duplikátum */
                }
                if (is_new) n->rx_new++; else n->rx_dup++;

                /* App-ACK MINDEN valid csomagra (DUP-ra is — az első ACK elveszhetett). */
                uint32_t fail_before = s_ack_status_fail;
                bool sent = send_ack(item.src_mac, p->node_id, p->seq);
                if (sent) {
                    n->ack_sent++;
                    if (s_ack_status_fail != fail_before) n->ack_txfail++;
                }

                ESP_LOGI(TAG, "SZ6 RX node=%u seq=%u try=%u kind=%s gap=%u rssi=%d",
                         p->node_id, (unsigned)p->seq, (unsigned)p->packet_idx,
                         is_new ? "NEW" : "DUP", (unsigned)gap, (int)item.rssi);
                if (!isfinite(p->weight)) {
                    ESP_LOGW(TAG, "SZ6 SENSOR_WARN node=%u seq=%u sensor=HX711 status=TIMEOUT kind=%s rssi=%d",
                             p->node_id, (unsigned)p->seq, is_new ? "NEW" : "DUP",
                             (int)item.rssi);
                }
            } while (0);
        }

        /* Periodikus STAT (akkor is, ha épp csomagok jönnek). */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_stat_us >= (int64_t)SZ6_STAT_PERIOD_MS * 1000) {
            last_stat_us = now_us;
            for (int i = 0; i < SZ6_MAX_NODES; i++) {
                if (s_nodes[i].in_use) print_stat(&s_nodes[i], now_us);
            }
            ESP_LOGI(TAG, "SZ6 MSTAT uptime_s=%u rxcb=%u q_drop=%u len_bad=%u ver_bad=%u",
                     (unsigned)(now_us / 1000000), (unsigned)s_diag_recv_cb,
                     (unsigned)s_q_dropped, (unsigned)s_diag_len_bad,
                     (unsigned)s_diag_ver_bad);
        }
    }
}

/* WiFi + ESP-NOW init a kötelező sorrendben (Sz2-minta + PS_NONE). */
static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* KRITIKUS a masteren: power-save ki, különben modem-sleep csomagvesztést okoz. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* Interop-hardening (Sz2): explicit 11b/g/n, az LR bit KIZÁRVA. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ6_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    /* Broadcast peer + rate-config (a node discovery-broadcastjainak szimmetriájáért). */
    static const uint8_t bc[ESP_NOW_ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    ensure_peer(bc);

    uint8_t ch_prim = 0; wifi_second_chan_t ch_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch_prim, &ch_sec);
    uint8_t my_mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, my_mac);
    ESP_LOGI(TAG, "master MAC=%02X:%02X:%02X:%02X:%02X:%02X csatorna=%u",
             my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5], ch_prim);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sz6 MASTER — always-on RX + app-ACK ===");
    ESP_LOGI(TAG, "csatorna=%d queue_len=%d max_nodes=%d stat_period=%dms",
             SZ6_CHANNEL, SZ6_RX_QUEUE_LEN, SZ6_MAX_NODES, SZ6_STAT_PERIOD_MS);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    memset(s_nodes, 0, sizeof(s_nodes));

    s_rx_queue = xQueueCreate(SZ6_RX_QUEUE_LEN, sizeof(sz6_rx_item_t));
    s_tx_done  = xSemaphoreCreateBinary();
    configASSERT(s_rx_queue != NULL && s_tx_done != NULL);

    wifi_espnow_init();

    /* Consumer a 2. magon (core 1): a WiFi/recv-cb (core 0) ne versenyezzen a feldolgozással. */
    xTaskCreatePinnedToCore(consumer_task, "sz6_rx", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Master kész, várja a node-okat...");
}

/*
 * Sz2b — master WiFi STA + ESP-NOW koegzisztencia · MASTER
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz2b_coexistence_packet_loss/master
 *
 * EREDET: az Sz6 (wake→megszólítás→átadás, PASS) master-forrásának adaptációja. A vételi
 * pipeline (recv-cb -> queue -> consumer core 1-en, app-ACK DUP-ra IS, dedup/gap/statisztika)
 * VÁLTOZATLAN. Az ÚJ réteg a D1 döntés tárgya: EGYIDEJŰ WiFi STA + ESP-NOW üzem.
 *
 * ÜZEMMÓDOK (build-flag, platformio.ini env-enként) — a tesztterv fázisai:
 *   SZ2B_WIFI_MODE=0  ESP-NOW only (baseline; fix csatorna, mint az Sz6-ban)
 *   SZ2B_WIFI_MODE=1  STA-connect + ESP-NOW, forgalom nélkül
 *   SZ2B_WIFI_MODE=2  STA + periodikus gateway-forgalom (HTTP GET) + ESP-NOW
 *
 * CSATORNA-KEZELÉS (az Sz2b kulcsproblémája)
 *   STA-üzemben a csatornát az AP diktálja: az asszociáció UTÁN esp_wifi_set_channel-t
 *   hívni TILOS (törné az asszociációt) — ezért MODE>=1-ben egyáltalán nem állítunk
 *   csatornát, az ESP-NOW peerek channel=0-val (aktuális csatorna) kerülnek fel. A node-ok
 *   csatorna-scan discovery-vel találják meg a mastert (lásd node-forrás). A tényleges
 *   csatornát a WIFI CONNECTED esemény + az MSTAT logolja.
 *
 * WIFI-RECONNECT: esemény-alapú (STA_DISCONNECTED -> reconnect-task, exponenciális backoff
 *   1..32 s + random). A reconnect NEM állítja le az ESP-NOW vételt (a radio az AP-keresés
 *   pillanataiban ugrál csatornát — az emiatt kimaradó DATA-t a node app-retry-ja fedi).
 *
 * SZ2-ÖRÖKSÉG (kötelező fegyelem): recv-cb csak kimásol + queue-ba dob; ACK-TX szerializáció
 *   send-cb szemaforral; NO_MEM vár-újrapróbál; esp_wifi_set_ps(WIFI_PS_NONE) — STA mellett
 *   IS (hálózati tápon a fogyasztás nem szempont, az ESP-NOW RX-hez kritikus); PHY-fix
 *   (explicit 11b/g/n LR nélkül + esp_now_set_peer_rate_config 11B/1M_L minden peerre).
 *
 * KIMENET (gép-grep-elhető, a bench-kiértékelés alapja)
 *   - Minden vett csomagra:  "SZ2B RX node=.. seq=.. try=.. kind=NEW|DUP gap=.. rssi=.."
 *   - Periodikusan:          "SZ2B STAT node=.. ..." (kumulatív számlálók node-onként)
 *                            "SZ2B MSTAT ... wifi=.. ch=.. ap_rssi=.. gw_ok=.. reconn=.."
 *   - WiFi-eseményekre:      "SZ2B WIFI CONNECTED/GOT_IP/DISCONNECT/RECONNECT ..."
 *
 * BOARD: ESP32-S3-Zero (kétmagos: WiFi/recv-cb core 0, consumer core 1), konzol a natív
 *   USB-Serial-JTAG-on. Always-on -> stabil port, háttér-log (tools/serial_logger.py).
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_idf_version.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "protocol.h"   /* a BEFAGYASZTOTT közös wire-header (firmware/common) */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* Üzemmód — build-flag adja (platformio.ini env-enként), lásd a fejlécet. */
#ifndef SZ2B_WIFI_MODE
#define SZ2B_WIFI_MODE          0
#endif

/* WiFi hitelesítők — build-flag adja (creds.ini, gitignored; minta: creds.ini.example). */
#if SZ2B_WIFI_MODE >= 1
#ifndef SZ2B_WIFI_SSID
#error "SZ2B_WIFI_SSID hianyzik (creds.ini -> build_flags)"
#endif
#ifndef SZ2B_WIFI_PASS
#error "SZ2B_WIFI_PASS hianyzik (creds.ini -> build_flags)"
#endif
#endif

/* MODE=0 (baseline) fix ESP-NOW csatornája. MODE>=1-ben NINCS csatorna-állítás! */
#define SZ2B_CHANNEL            1

/* Vételi queue mélysége (burst-puffer a recv-cb és a consumer közt). */
#define SZ2B_RX_QUEUE_LEN       64

/* Hány node-ot tartunk nyilván. */
#define SZ2B_MAX_NODES          8

/* A send-cb szemafor timeoutja (ms) az ACK-küldésnél. */
#define SZ2B_SEND_TIMEOUT_MS    50

/* ESP_ERR_ESPNOW_NO_MEM esetén ennyit várunk, majd újrapróbáljuk az ACK-ot (ms). */
#define SZ2B_NO_MEM_RETRY_MS    2

/* Az ACK-ban hirdetett következő ébredési offset (s) — informatív (Sz6-örökség). */
#define SZ2B_ADVERT_SLEEP_S     12

/* Periodikus STAT riport (ms). */
#define SZ2B_STAT_PERIOD_MS     10000

/* --- WiFi STA réteg (MODE>=1) --- */
/* Reconnect exponenciális backoff sávja (ms) + random adalék. */
#define SZ2B_RECONN_BACKOFF_MIN_MS   1000
#define SZ2B_RECONN_BACKOFF_MAX_MS   32000
#define SZ2B_RECONN_JITTER_MS        500

/* --- Gateway-forgalom (MODE=2): periodikus HTTP GET a router root-jára --- */
#define SZ2B_TRAFFIC_PERIOD_MS       5000
/* Socket timeout (connect/send/recv) ms-ben. */
#define SZ2B_TRAFFIC_TIMEOUT_MS      3000

/* ================================================================= */

static const char *TAG = "SZ2B-MASTER";

/* Egy vételi queue-elem: payload + a CSAK a callbackben érvényes src MAC és RSSI kimásolva. */
typedef struct {
    sz2_payload_t payload;
    uint8_t       src_mac[6];
    int8_t        rssi;
    uint16_t      len;
} sz2b_rx_item_t;

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
} sz2b_node_t;

static QueueHandle_t     s_rx_queue = NULL;
static SemaphoreHandle_t s_tx_done  = NULL;
static sz2b_node_t       s_nodes[SZ2B_MAX_NODES];

static volatile uint32_t s_q_dropped    = 0;   /* queue tele a recv-cb-ben           */
static volatile uint32_t s_diag_recv_cb = 0;   /* hányszor futott a recv_cb          */
static volatile uint32_t s_diag_len_bad = 0;
static volatile uint32_t s_diag_ver_bad = 0;
static volatile uint32_t s_ack_status_fail = 0; /* send-cb FAIL az ACK-okra (globál) */

/* --- WiFi STA állapot (MODE>=1) --- */
static volatile bool     s_wifi_assoc   = false;  /* asszociálva (CONNECTED)          */
static volatile bool     s_wifi_ip      = false;  /* van IP (GOT_IP)                  */
static volatile uint32_t s_disconn_cnt  = 0;      /* DISCONNECTED események           */
static volatile uint32_t s_got_ip_cnt   = 0;      /* GOT_IP események                 */
static volatile uint32_t s_backoff_ms   = SZ2B_RECONN_BACKOFF_MIN_MS;
static volatile uint32_t s_gw_addr      = 0;      /* gateway IP (network byte order)  */
static SemaphoreHandle_t s_reconn_sem   = NULL;

/* --- Gateway-forgalom számlálók (MODE=2) --- */
static volatile uint32_t s_gw_ok      = 0;
static volatile uint32_t s_gw_fail    = 0;
static volatile uint32_t s_gw_skip    = 0;   /* kihagyva, mert nem volt WiFi/IP       */
static volatile uint32_t s_gw_last_ms = 0;   /* utolsó sikeres GET időtartama         */

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
    sz2b_rx_item_t item;

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

/* Peer biztosítása + a fix robusztus ráta (11b/1M_L) — az Sz2 PHY-interop fix fele.
 * channel=0 = MINDIG az aktuális csatorna (STA-üzemben az AP-é) — SOSEM fix érték! */
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
    ack.next_slot_offset_s     = SZ2B_ADVERT_SLEEP_S;
    ack.retry_policy           = 0;

    ensure_peer(dest);

    for (;;) {
        esp_err_t err = esp_now_send(dest, (const uint8_t *)&ack, sizeof(ack));
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(SZ2B_SEND_TIMEOUT_MS));
            return true;
        } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(SZ2B_NO_MEM_RETRY_MS));
        } else {
            ESP_LOGW(TAG, "ACK send hiba node=%u seq=%u: %s",
                     node_id, (unsigned)seq, esp_err_to_name(err));
            return false;
        }
    }
}

/* Node-sáv keresése/foglalása node_id alapján. */
static sz2b_node_t *node_get(uint8_t node_id, const uint8_t *mac)
{
    for (int i = 0; i < SZ2B_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].node_id == node_id) return &s_nodes[i];
    }
    for (int i = 0; i < SZ2B_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) {
            sz2b_node_t *n = &s_nodes[i];
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
static void print_stat(const sz2b_node_t *n, int64_t now_us)
{
    int rssi_avg = n->rssi_n ? (int)(n->rssi_sum / (int64_t)n->rssi_n) : 0;
    ESP_LOGI(TAG, "SZ2B STAT node=%u last_seq=%u new=%u dup=%u miss=%u crc=%u "
                  "ack=%u ackfail=%u resets=%u rssi_avg=%d age_s=%d",
             n->node_id, (unsigned)n->last_seq, (unsigned)n->rx_new, (unsigned)n->rx_dup,
             (unsigned)n->miss_cum, (unsigned)n->crc_err, (unsigned)n->ack_sent,
             (unsigned)n->ack_txfail, (unsigned)n->resets, rssi_avg,
             (int)((now_us - n->last_rx_us) / 1000000));
}

/* Globális MSTAT sor — a WiFi/koegzisztencia állapottal bővítve (Sz2b). */
static void print_mstat(int64_t now_us)
{
    uint8_t ch_prim = 0; wifi_second_chan_t ch_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch_prim, &ch_sec);

    const char *wifi_state = "OFF";
    int ap_rssi = 0;
#if SZ2B_WIFI_MODE >= 1
    wifi_state = s_wifi_ip ? "IP" : (s_wifi_assoc ? "ASSOC" : "DOWN");
    if (s_wifi_assoc) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ap_rssi = ap.rssi;
    }
#endif
    ESP_LOGI(TAG, "SZ2B MSTAT uptime_s=%u rxcb=%u q_drop=%u len_bad=%u ver_bad=%u "
                  "wifi=%s ch=%u ap_rssi=%d disconn=%u got_ip=%u "
                  "gw_ok=%u gw_fail=%u gw_skip=%u gw_last_ms=%u",
             (unsigned)(now_us / 1000000), (unsigned)s_diag_recv_cb,
             (unsigned)s_q_dropped, (unsigned)s_diag_len_bad, (unsigned)s_diag_ver_bad,
             wifi_state, (unsigned)ch_prim, ap_rssi,
             (unsigned)s_disconn_cnt, (unsigned)s_got_ip_cnt,
             (unsigned)s_gw_ok, (unsigned)s_gw_fail, (unsigned)s_gw_skip,
             (unsigned)s_gw_last_ms);
}

/* CONSUMER-TASK (core 1): CRC, osztályozás (NEW/DUP/gap), ACK, statisztika, periodikus STAT. */
static void consumer_task(void *arg)
{
    (void)arg;
    sz2b_rx_item_t item;
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
                sz2b_node_t *n = node_get(p->node_id, item.src_mac);
                if (n == NULL) {
                    ESP_LOGW(TAG, "nincs szabad node-sáv (SZ2B_MAX_NODES=%d)", SZ2B_MAX_NODES);
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

                ESP_LOGI(TAG, "SZ2B RX node=%u seq=%u try=%u kind=%s gap=%u rssi=%d",
                         p->node_id, (unsigned)p->seq, (unsigned)p->packet_idx,
                         is_new ? "NEW" : "DUP", (unsigned)gap, (int)item.rssi);
            } while (0);
        }

        /* Periodikus STAT (akkor is, ha épp csomagok jönnek). */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_stat_us >= (int64_t)SZ2B_STAT_PERIOD_MS * 1000) {
            last_stat_us = now_us;
            for (int i = 0; i < SZ2B_MAX_NODES; i++) {
                if (s_nodes[i].in_use) print_stat(&s_nodes[i], now_us);
            }
            print_mstat(now_us);
        }
    }
}

#if SZ2B_WIFI_MODE >= 1
/* WIFI-ESEMÉNYKEZELŐ (event-loop task, core 0): állapot-követés + reconnect-jelzés.
 * FONTOS: itt semmi nehéz munka — a reconnect-várakozás a reconnect_task-ban van. */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();   /* első kapcsolódási kísérlet azonnal */
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = (const wifi_event_sta_connected_t *)data;
        s_wifi_assoc = true;
        ESP_LOGI(TAG, "SZ2B WIFI CONNECTED ssid=%s ch=%u", (const char *)ev->ssid,
                 (unsigned)ev->channel);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_wifi_assoc = false;
        s_wifi_ip    = false;
        s_disconn_cnt++;
        ESP_LOGW(TAG, "SZ2B WIFI DISCONNECT reason=%u total=%u",
                 (unsigned)ev->reason, (unsigned)s_disconn_cnt);
        xSemaphoreGive(s_reconn_sem);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_gw_addr    = ev->ip_info.gw.addr;
        s_wifi_ip    = true;
        s_got_ip_cnt++;
        s_backoff_ms = SZ2B_RECONN_BACKOFF_MIN_MS;   /* stabil kapcsolat -> backoff nulláz */
        uint8_t ch = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);
        ESP_LOGI(TAG, "SZ2B WIFI GOT_IP ip=" IPSTR " gw=" IPSTR " ch=%u",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), (unsigned)ch);
    }
}

/* RECONNECT-TASK: DISCONNECTED-re exponenciális backoff (+ jitter) után újrapróbál.
 * Az ESP-NOW vétel közben ÉL — csak az AP-kereső pillanatokban ugrál a radio. */
static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_reconn_sem, portMAX_DELAY);
        if (s_wifi_assoc) continue;   /* közben visszajött (elkésett esemény) */
        uint32_t wait_ms = s_backoff_ms + (esp_random() % SZ2B_RECONN_JITTER_MS);
        ESP_LOGI(TAG, "SZ2B WIFI RECONNECT wait_ms=%u", (unsigned)wait_ms);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        if (!s_wifi_assoc) {
            uint32_t next = s_backoff_ms * 2;
            s_backoff_ms = (next > SZ2B_RECONN_BACKOFF_MAX_MS)
                           ? SZ2B_RECONN_BACKOFF_MAX_MS : next;
            esp_wifi_connect();
        }
    }
}
#endif /* SZ2B_WIFI_MODE >= 1 */

#if SZ2B_WIFI_MODE >= 2
/* Egy HTTP GET a gateway (router) root-jára, sima lwip sockettel.
 * Siker: TCP connect + legalább 1 byte válasz a timeouton belül. Nem-blokkoló connect +
 * select — a blokkoló lwip-connect timeoutja nem hangolható SO_SNDTIMEO-val. */
static bool http_get_gateway(uint32_t gw_addr)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(80);
    dst.sin_addr.s_addr = gw_addr;

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return false;

    bool ok = false;
    do {
        fcntl(s, F_SETFL, O_NONBLOCK);
        int r = connect(s, (struct sockaddr *)&dst, sizeof(dst));
        if (r != 0 && errno != EINPROGRESS) break;

        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv = { .tv_sec  = SZ2B_TRAFFIC_TIMEOUT_MS / 1000,
                              .tv_usec = (SZ2B_TRAFFIC_TIMEOUT_MS % 1000) * 1000 };
        if (select(s + 1, NULL, &wf, NULL, &tv) <= 0) break;
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        if (soerr != 0) break;

        /* vissza blokkolóra, send/recv timeouttal */
        fcntl(s, F_SETFL, 0);
        struct timeval iotv = tv;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &iotv, sizeof(iotv));
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &iotv, sizeof(iotv));

        const char req[] = "GET / HTTP/1.0\r\nHost: gw\r\nConnection: close\r\n\r\n";
        if (send(s, req, sizeof(req) - 1, 0) < 0) break;
        char buf[128];
        int n = recv(s, buf, sizeof(buf), 0);
        ok = (n > 0);
    } while (0);

    close(s);
    return ok;
}

/* FORGALOM-TASK (core 0, a WiFi/lwip mellett — a core 1 a consumeré): periodikus GET a
 * gateway-re, siker/hiba/skip számlálókkal. Ez adja a VALÓDI koegzisztencia-terhelést. */
static void traffic_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SZ2B_TRAFFIC_PERIOD_MS));
        if (!s_wifi_ip || s_gw_addr == 0) {
            s_gw_skip++;
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        bool ok = http_get_gateway(s_gw_addr);
        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (ok) {
            s_gw_ok++;
            s_gw_last_ms = ms;
        } else {
            s_gw_fail++;
            ESP_LOGW(TAG, "SZ2B GW FAIL ms=%u ok=%u fail=%u",
                     (unsigned)ms, (unsigned)s_gw_ok, (unsigned)s_gw_fail);
        }
    }
}
#endif /* SZ2B_WIFI_MODE >= 2 */

/* WiFi + ESP-NOW init a kötelező sorrendben (Sz2-minta + PS_NONE).
 * MODE>=1: STA-config + connect; MODE=0: fix csatorna (Sz6-baseline). */
static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if SZ2B_WIFI_MODE >= 1
    esp_netif_create_default_wifi_sta();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

#if SZ2B_WIFI_MODE >= 1
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, SZ2B_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, SZ2B_WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());
    /* KRITIKUS a masteren: power-save ki — STA mellett IS (hálózati táp, az RX az úr). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* Interop-hardening (Sz2): explicit 11b/g/n, az LR bit KIZÁRVA. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
#if SZ2B_WIFI_MODE == 0
    /* Baseline: fix csatorna, mint az Sz6-ban. STA-üzemben (MODE>=1) TILOS — az AP diktál. */
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ2B_CHANNEL, WIFI_SECOND_CHAN_NONE));
#endif

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
    ESP_LOGI(TAG, "master MAC=%02X:%02X:%02X:%02X:%02X:%02X csatorna=%u wifi_mode=%d",
             my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5],
             ch_prim, SZ2B_WIFI_MODE);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sz2b MASTER — always-on RX + app-ACK + WiFi STA (mode=%d) ===",
             SZ2B_WIFI_MODE);
    ESP_LOGI(TAG, "queue_len=%d max_nodes=%d stat_period=%dms traffic_period=%dms",
             SZ2B_RX_QUEUE_LEN, SZ2B_MAX_NODES, SZ2B_STAT_PERIOD_MS,
             (int)(SZ2B_WIFI_MODE >= 2 ? SZ2B_TRAFFIC_PERIOD_MS : 0));

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    memset(s_nodes, 0, sizeof(s_nodes));

    s_rx_queue = xQueueCreate(SZ2B_RX_QUEUE_LEN, sizeof(sz2b_rx_item_t));
    s_tx_done  = xSemaphoreCreateBinary();
    configASSERT(s_rx_queue != NULL && s_tx_done != NULL);
#if SZ2B_WIFI_MODE >= 1
    s_reconn_sem = xSemaphoreCreateCounting(8, 0);
    configASSERT(s_reconn_sem != NULL);
#endif

    wifi_espnow_init();

    /* Consumer a 2. magon (core 1): a WiFi/recv-cb (core 0) ne versenyezzen a feldolgozással. */
    xTaskCreatePinnedToCore(consumer_task, "sz2b_rx", 8192, NULL, 5, NULL, 1);
#if SZ2B_WIFI_MODE >= 1
    xTaskCreatePinnedToCore(reconnect_task, "sz2b_reconn", 4096, NULL, 4, NULL, 0);
#endif
#if SZ2B_WIFI_MODE >= 2
    xTaskCreatePinnedToCore(traffic_task, "sz2b_traffic", 4096, NULL, 3, NULL, 0);
#endif

    ESP_LOGI(TAG, "Master kész, várja a node-okat...");
}

/*
 * espnow_tx.c — WiFi (STA) + ESP-NOW init a PHY-fixszel, app-ACK-os node átadás.
 */
#include <stdbool.h>
#include <string.h>

#include "espnow_tx.h"
#include "node_config.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "espnow_tx";

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static SemaphoreHandle_t s_tx_done = NULL;
static SemaphoreHandle_t s_ack_sem = NULL;
static bool s_inited = false;

static RTC_DATA_ATTR uint8_t s_master_mac[ESP_NOW_ETH_ALEN];
static RTC_DATA_ATTR uint8_t s_master_known;

static volatile uint32_t s_ack_wait_seq;
static volatile uint8_t s_ack_wait_node;
static volatile uint32_t s_ack_wait_boot;
static volatile uint32_t s_tx_status_fail;
static sz2_ack_t s_ack;
static uint8_t s_ack_src[ESP_NOW_ETH_ALEN];

static void drain_sem(SemaphoreHandle_t sem)
{
    while (sem != NULL && xSemaphoreTake(sem, 0) == pdTRUE) {
    }
}

static uint32_t rand_between(uint32_t lo, uint32_t hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (esp_random() % (hi - lo + 1));
}

static bool ensure_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "add_peer hiba %02X:%02X:%02X:%02X:%02X:%02X: %s",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     esp_err_to_name(err));
            return false;
        }
    }

    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate = WIFI_PHY_RATE_1M_L,
        .ersu = false,
        .dcm = false,
    };
    esp_err_t err = esp_now_set_peer_rate_config(mac, &rc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "peer rate config hiba: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        s_tx_status_fail++;
    }
    xSemaphoreGive(s_tx_done);
}
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) {
        s_tx_status_fail++;
    }
    xSemaphoreGive(s_tx_done);
}
#endif

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != (int)sizeof(sz2_ack_t) || s_ack_sem == NULL) {
        return;
    }

    sz2_ack_t ack;
    memcpy(&ack, data, sizeof(ack));
    if (ack.protocol_version != SZ2_PROTO_VERSION || ack.packet_type != SZ2_PKT_ACK) {
        return;
    }
    if (ack.node_id != s_ack_wait_node || ack.ack_last_seq != s_ack_wait_seq ||
        ack.boot_id != s_ack_wait_boot) {
        return;
    }
    /* Első broadcast ACK tanítja meg a master MAC-jét. Utána egy másik forrásból
     * érkező, egyébként jól formált ACK nem törölhet rekordot. */
    if (s_master_known && memcmp(s_master_mac, info->src_addr, ESP_NOW_ETH_ALEN) != 0) {
        ESP_LOGW(TAG, "ACK ignored from untrusted source");
        return;
    }

    s_ack = ack;
    memcpy(s_ack_src, info->src_addr, ESP_NOW_ETH_ALEN);
    xSemaphoreGive(s_ack_sem);
}

void espnow_tx_init(void)
{
    if (s_inited) {
        return;
    }

    s_tx_done = xSemaphoreCreateBinary();
    s_ack_sem = xSemaphoreCreateBinary();
    configASSERT(s_tx_done != NULL && s_ack_sem != NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(NODE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    int8_t txp_before = 0;
    int8_t txp_after = 0;
    esp_wifi_get_max_tx_power(&txp_before);
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(NODE_TX_POWER));
    esp_wifi_get_max_tx_power(&txp_after);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(ensure_peer(BROADCAST_MAC) ? ESP_OK : ESP_FAIL);
    if (s_master_known) {
        ensure_peer(s_master_mac);
    }

    s_inited = true;

    uint8_t mac[6] = { 0 };
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "ESP-NOW ready STA=%02X:%02X:%02X:%02X:%02X:%02X ch=%u expected=%u txp=%d->%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)ch, (unsigned)NODE_WIFI_CHANNEL, (int)txp_before, (int)txp_after);
}

void espnow_tx_get_mac(uint8_t mac_out[6])
{
    if (s_inited && esp_wifi_get_mac(WIFI_IF_STA, mac_out) == ESP_OK) {
        return;
    }
    if (esp_read_mac(mac_out, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(mac_out, 0, 6);
    }
}

static bool send_bytes_serialized(const uint8_t *dest, const void *data, size_t len)
{
    if (!ensure_peer(dest)) {
        return false;
    }

    /* Korlátos NO_MEM retry: ha a WiFi stack TX queue-ja tartósan beragad, a node
     * feladja és alszik, nem pörög ébren végtelen hurokban (akkumerülés). */
    for (int attempt = 0; attempt < NODE_NO_MEM_MAX_RETRY; attempt++) {
        drain_sem(s_tx_done);
        esp_err_t err = esp_now_send(dest, (const uint8_t *)data, len);
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(NODE_SEND_TIMEOUT_MS));
            return true;
        }
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(NODE_NO_MEM_RETRY_MS));
            continue;
        }
        ESP_LOGW(TAG, "esp_now_send hiba: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "esp_now_send: tartos NO_MEM, feladva %d probalkozas utan",
             NODE_NO_MEM_MAX_RETRY);
    return false;
}

void espnow_tx_send(const sz2_payload_t *p, int repeat)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "espnow_tx_send init elott hivva, kihagyva");
        return;
    }

    for (int rep = 0; rep < repeat; rep++) {
        send_bytes_serialized(BROADCAST_MAC, p, sizeof(*p));
    }
}

static void learn_master_mac(const uint8_t mac[ESP_NOW_ETH_ALEN])
{
    if (!s_master_known || memcmp(s_master_mac, mac, ESP_NOW_ETH_ALEN) != 0) {
        memcpy(s_master_mac, mac, ESP_NOW_ETH_ALEN);
        s_master_known = 1;
        ensure_peer(s_master_mac);
        ESP_LOGI(TAG, "master MAC tanulva: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

bool espnow_tx_send_with_ack(const sz2_payload_t *p, espnow_tx_ack_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
        result->dest_mode = '?';
    }
    if (!s_inited) {
        ESP_LOGW(TAG, "ACK-os kuldes init elott hivva, kihagyva");
        return false;
    }

    if (NODE_PRE_TX_JITTER_MAX_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(rand_between(0, NODE_PRE_TX_JITTER_MAX_MS)));
    }

    for (uint32_t attempt = 1; attempt <= NODE_ACK_MAX_ATTEMPTS; attempt++) {
        const uint8_t *dest = BROADCAST_MAC;
        char dest_mode = s_master_known ? 'F' : 'B';
        if (s_master_known && attempt <= NODE_UNICAST_ATTEMPTS) {
            dest = s_master_mac;
            dest_mode = 'U';
        }

        sz2_payload_t tx = *p;
        tx.packet_idx = attempt;
        tx.crc = sz2_payload_crc(&tx);

        s_ack_wait_node = tx.node_id;
        s_ack_wait_seq = tx.seq;
        s_ack_wait_boot = tx.boot_id;
        drain_sem(s_ack_sem);

        uint32_t fail_before = s_tx_status_fail;
        uint32_t ack_timeout_ms = attempt == 1U ? NODE_DATA_FIRST_ACK_TIMEOUT_MS
                                               : NODE_ACK_TIMEOUT_MS;
        bool sent = send_bytes_serialized(dest, &tx, sizeof(tx));
        if (sent && xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(ack_timeout_ms)) == pdTRUE) {
            learn_master_mac(s_ack_src);
            if (result) {
                result->ack_last_seq = s_ack.ack_last_seq;
                result->clear_buffer_until_seq = s_ack.clear_buffer_until_seq;
                result->clock_correction_s = s_ack.clock_correction_s;
                result->next_slot_offset_s = s_ack.next_slot_offset_s;
                result->retry_policy = s_ack.retry_policy;
                memcpy(result->master_mac, s_ack_src, ESP_NOW_ETH_ALEN);
                result->attempts = attempt;
                result->dest_mode = dest_mode;
            }
            ESP_LOGI(TAG, "ACK OK node=%u seq=%u attempt=%u dest=%c wait_ms=%u clear_until=%u next_slot_s=%u txfail_delta=%u",
                     tx.node_id, (unsigned)tx.seq, (unsigned)attempt, dest_mode,
                     (unsigned)ack_timeout_ms,
                     (unsigned)s_ack.clear_buffer_until_seq,
                     (unsigned)s_ack.next_slot_offset_s,
                     (unsigned)(s_tx_status_fail - fail_before));
            return true;
        }

        if (attempt < NODE_ACK_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(rand_between(NODE_RETRY_JITTER_MIN_MS,
                                                  NODE_RETRY_JITTER_MAX_MS)));
        }
    }

    ESP_LOGW(TAG, "ACK FAIL node=%u seq=%u attempts=%u first_wait_ms=%u retry_wait_ms=%u tx_status_fail=%u",
             p->node_id, (unsigned)p->seq, (unsigned)NODE_ACK_MAX_ATTEMPTS,
             (unsigned)NODE_DATA_FIRST_ACK_TIMEOUT_MS, (unsigned)NODE_ACK_TIMEOUT_MS,
             (unsigned)s_tx_status_fail);
    return false;
}

bool espnow_tx_sync_hello(const sz2_hello_t *hello, espnow_tx_ack_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
        result->dest_mode = '?';
    }
    if (!s_inited) {
        ESP_LOGW(TAG, "HELLO init elott hivva, kihagyva");
        return false;
    }

    for (uint32_t attempt = 1; attempt <= NODE_ACK_MAX_ATTEMPTS; attempt++) {
        sz2_hello_t tx = *hello;
        tx.crc = sz2_hello_crc(&tx);

        s_ack_wait_node = tx.node_id;
        s_ack_wait_seq = tx.hello_seq;
        s_ack_wait_boot = tx.boot_id;
        drain_sem(s_ack_sem);

        uint32_t fail_before = s_tx_status_fail;
        bool sent = send_bytes_serialized(BROADCAST_MAC, &tx, sizeof(tx));
        if (sent && xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(NODE_ACK_TIMEOUT_MS)) == pdTRUE) {
            learn_master_mac(s_ack_src);
            if (result) {
                result->ack_last_seq = s_ack.ack_last_seq;
                result->clear_buffer_until_seq = s_ack.clear_buffer_until_seq;
                result->clock_correction_s = s_ack.clock_correction_s;
                result->next_slot_offset_s = s_ack.next_slot_offset_s;
                result->retry_policy = s_ack.retry_policy;
                memcpy(result->master_mac, s_ack_src, ESP_NOW_ETH_ALEN);
                result->attempts = attempt;
                result->dest_mode = 'B';
            }
            ESP_LOGI(TAG, "HELLO ACK OK node=%u hello_seq=%u attempt=%u next_slot_s=%u txfail_delta=%u",
                     tx.node_id, (unsigned)tx.hello_seq, (unsigned)attempt,
                     (unsigned)s_ack.next_slot_offset_s,
                     (unsigned)(s_tx_status_fail - fail_before));
            return true;
        }

        if (attempt < NODE_ACK_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(rand_between(NODE_RETRY_JITTER_MIN_MS,
                                                  NODE_RETRY_JITTER_MAX_MS)));
        }
    }

    ESP_LOGW(TAG, "HELLO ACK FAIL node=%u hello_seq=%u attempts=%u tx_status_fail=%u",
             hello->node_id, (unsigned)hello->hello_seq, (unsigned)NODE_ACK_MAX_ATTEMPTS,
             (unsigned)s_tx_status_fail);
    return false;
}

void espnow_tx_deinit(void)
{
    if (!s_inited) {
        return;
    }

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_tx_done) {
        vSemaphoreDelete(s_tx_done);
        s_tx_done = NULL;
    }
    if (s_ack_sem) {
        vSemaphoreDelete(s_ack_sem);
        s_ack_sem = NULL;
    }
    s_inited = false;
    ESP_LOGI(TAG, "ESP-NOW/WiFi leallitva deep sleep elott");
}

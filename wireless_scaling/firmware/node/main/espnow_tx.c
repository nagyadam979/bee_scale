/*
 * espnow_tx.c — WiFi (STA) + ESP-NOW init a PHY-fixszel és tűzz-és-felejts broadcast küldés.
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * EREDET: az M1 (m1_node_beacon.c) éles-mért wifi_espnow_init + TX-hurok, SZÓ SZERINT azonos
 *   PHY-fixszel (Sz2/M1-ben validált). A csatorna a node_config.h-ból (NODE_WIFI_CHANNEL).
 *
 * PHY-FIX (KRITIKUS — enélkül NÉMA a link): esp_wifi_set_protocol 11b/g/n (LR KIZÁRVA) a start
 *   UTÁN, és esp_now_set_peer_rate_config {11b, 1M_L} az add_peer UTÁN. Enélkül a C3-node és az
 *   S3-master eltérő modulációt/rátát választhat -> a frame fizikailag dekódolhatatlan = vétel-némaság.
 *
 * Az init a kötelező sorrendben: netif -> event loop -> wifi_init -> storage RAM -> mode STA ->
 *   wifi_start -> set_protocol -> set_channel (START UTÁN!) -> esp_now_init -> register send-cb ->
 *   add broadcast peer -> set_peer_rate_config. STA-asszociáció NINCS (az ESP-NOW connectionless).
 *   NVS-t feltételez (a hívó main.c előtte inicializálja).
 */
#include <string.h>
#include "espnow_tx.h"
#include "node_config.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "espnow_tx";

/* Broadcast cím — minden ESP-NOW vevő veszi (a master peer nélkül is). */
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* TX-szerializáció: a send-cb adja meg; egy in-flight küldés. */
static SemaphoreHandle_t s_tx_done = NULL;
static bool s_inited = false;

/*
 * SEND-CALLBACK (WiFi-task kontextus, NEM ISR -> sima xSemaphoreGive). Egyetlen feladata a
 * TX-szerializációs szemafor felszabadítása. VERZIÓ-GUARD: az IDF v5.4-ben a szignatúra
 * esp_now_send_info_t-re változott (az M1-gyel azonos kezelés).
 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    (void)status;
    xSemaphoreGive(s_tx_done);
}
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    (void)status;
    xSemaphoreGive(s_tx_done);
}
#endif

void espnow_tx_init(void)
{
    if (s_inited) {
        return;   /* idempotens: BENCH-ben a WIFI_IDLE fázis hívja egyszer */
    }

    s_tx_done = xSemaphoreCreateBinary();
    configASSERT(s_tx_done != NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* PHY-fix: explicit 11b/g/n, a LR (long-range) bit KIZÁRVA. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    /* A csatornát a START UTÁN kell beállítani (előtte felülíródna). */
    ESP_ERROR_CHECK(esp_wifi_set_channel(NODE_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    peer.channel = 0;             /* 0 = aktuális csatorna */
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* Fix, legrobusztusabb ráta (11b, 1 Mbps long-preamble) — PHY-mismatch ellen. add_peer UTÁN. */
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate    = WIFI_PHY_RATE_1M_L,
        .ersu    = false,
        .dcm     = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(BROADCAST_MAC, &rc));

    s_inited = true;

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "espnow_tx_init kesz: STA MAC %02X:%02X:%02X:%02X:%02X:%02X, ch=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], NODE_WIFI_CHANNEL);
}

void espnow_tx_get_mac(uint8_t mac_out[6])
{
    if (!s_inited) {
        memset(mac_out, 0, 6);   /* init előtt még nincs érvényes STA MAC */
        return;
    }
    esp_wifi_get_mac(WIFI_IF_STA, mac_out);
}

void espnow_tx_send(const sz2_payload_t *p, int repeat)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "espnow_tx_send init elott hivva — kihagyva");
        return;
    }

    for (int rep = 0; rep < repeat; rep++) {
        for (;;) {
            esp_err_t err = esp_now_send(BROADCAST_MAC, (const uint8_t *)p, sizeof(*p));
            if (err == ESP_OK) {
                xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(50));   /* egy in-flight küldés */
                break;
            } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
                vTaskDelay(pdMS_TO_TICKS(2));                   /* TX-FIFO tele -> újra */
            } else {
                ESP_LOGW(TAG, "esp_now_send hiba (rep=%d): %s", rep, esp_err_to_name(err));
                break;
            }
        }
    }
}

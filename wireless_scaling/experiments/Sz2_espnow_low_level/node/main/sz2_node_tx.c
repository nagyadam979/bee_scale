/*
 * Sz2 — ESP-NOW alacsony szint · NODE (TX)
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz2_espnow_low_level/node
 *
 * CÉL
 *   A node (ESP32-C3 SuperMini) oldali ESP-NOW SEND-fegyelem kialakítása: az a réteg, amire
 *   a teljes rádiós protokoll épül. A node broadcastként elküld SZ2_TOTAL_PACKETS (alap 1000)
 *   fix-méretű `sz2_payload_t` csomagot a masternek, és úgy, hogy a transfer veszteség- és
 *   lefagyásmentes legyen (pass: a master 0 elveszett, 0 lefagyás riportja).
 *
 *   A CHUNK LÉNYEGE itt: TX szerializáció. Egyszerre EGY in-flight küldés van — az
 *   esp_now_send() UTÁN a TX-task a send-callback szemaforára (`s_tx_done`) BLOKKOL, és csak
 *   annak visszatérése után küldi a következő csomagot. Ez előzi meg a belső ESP-NOW TX-FIFO
 *   túltöltését (ESP_ERR_ESPNOW_NO_MEM). NO_MEM esetén várunk-újrapróbálunk (nem dobunk,
 *   nem pörgünk).
 *
 * CALLBACK-FEGYELEM (a kutatás eredménye, NEM opcionális)
 *   Az ESP-NOW callbackek WiFi-task kontextusban futnak, NEM ISR-ben. Ezért a send-cb-ben
 *   `xSemaphoreGive(s_tx_done)` (NEM a *FromISR variáns) — a projekt korábbi jegyzete tévesen
 *   xQueueSendFromISR-t említ; tudatosan a sima API-t használjuk. A send-cb SEMMI mást nem
 *   csinál (a status broadcastnél gyakorlatilag mindig SUCCESS; ez MAC-szintű, NEM app-szintű
 *   kézbesítés).
 *
 * VERZIÓ-GUARD
 *   A send-cb szignatúrája ESP-IDF v5.4-ben változott (esp_now_send_info_t). A guard mindkét
 *   ágat lefedi, hogy a C3 és a classic ESP32 (eltérő IDF-verzió) is forduljon.
 *
 * BOARD: ESP32-C3 SuperMini. Konzol: natív USB-Serial-JTAG. FONTOS: a node logja ezen a
 *   fejlesztőgépen háttérből NEM olvasható (JTAG-CDC); a felhasználó a SAJÁT termináljában
 *   nézi: `pio device monitor -p <PORT> -b 115200`.
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_idf_version.h"

#include "sz2_protocol.h"   /* a KÖZÖS header (../../common) */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* WiFi/ESP-NOW csatorna. EGYEZZEN a master SZ2_CHANNEL-jével! (1..13) */
#define SZ2_CHANNEL             1

/* A node azonosítója (docs: unit_id). A bench-en 1; több node esetén egyedi. */
#define SZ2_NODE_ID             1

/* Hány csomagot küldjön a stressz-futás (a pass-feltétel alapja: ennyi vett a masteren). */
#define SZ2_TOTAL_PACKETS       1000

/* A send-cb szemafor timeoutja (ms). Ennyit vár a TX-task egy csomag MAC-szintű
 * lezárására; timeout esetén tovább lép (és számolja a timeout-ot a riporthoz). */
#define SZ2_SEND_TIMEOUT_MS     50

/* ESP_ERR_ESPNOW_NO_MEM esetén ennyit várunk, majd UGYANARRA a seq-re újrapróbálunk (ms). */
#define SZ2_NO_MEM_RETRY_MS     2

/* Opcionális csomag-közi késleltetés (ms). 0 = back-to-back (a legkeményebb stressz). */
#define SZ2_INTER_PACKET_DELAY_MS  0

/* Hány csomagonként írjon egy haladás-sort (a forró ciklust ne lassítsa minden csomag log-ja). */
#define SZ2_PROGRESS_EVERY      100

/* Ha true: a teljes futás végén újrakezdi (a végtelenségig ismétel). Alap: false (egyszer fut). */
#define SZ2_REPEAT              false

/* ================================================================= */

static const char *TAG = "SZ2-NODE";

/* Broadcast cím — minden ESP-NOW vevő veszi (a master peer nélkül is). */
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* TX szerializáció: a send-cb ezt adja vissza, a TX-task erre vár (egy in-flight küldés). */
static SemaphoreHandle_t s_tx_done = NULL;

/* Diagnosztika a végső összesítőhöz. */
static volatile uint32_t s_send_fail_count = 0;   /* esp_now_send() nem-NO_MEM hibák    */
static volatile uint32_t s_tx_status_fail  = 0;   /* send-cb status == FAIL             */

/*
 * SEND-CALLBACK. WiFi-task kontextusban fut (NEM ISR) -> sima xSemaphoreGive, NEM *FromISR.
 * Egyetlen feladata: a TX-szerializációs szemafor felszabadítása, hogy a TX-task küldhesse a
 * következő csomagot. A status broadcastnél gyakorlatilag mindig SUCCESS (MAC-szintű, nem
 * app-szintű kézbesítés) — a FAIL-t csak diagnosztikának számoljuk.
 *
 * VERZIÓ-GUARD: az IDF v5.4-ben a szignatúra esp_now_send_info_t-re változott.
 */
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

/*
 * WiFi + ESP-NOW init a kötelező sorrendben (lásd Sz2 spec / ESP-IDF ESP-NOW best practice):
 *   netif -> event loop -> wifi_init -> storage RAM -> mode STA -> wifi_start ->
 *   set_channel (START UTÁN!) -> esp_now_init -> register cb -> add broadcast peer.
 * STA-asszociáció (esp_wifi_connect) NINCS — az ESP-NOW connectionless.
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
    /* A csatornát a wifi START UTÁN kell beállítani (előtte felülíródna). */
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ2_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    /* Broadcast peer az aktuális csatornán (channel=0 = aktuális), STA interfész, titkosítás nélkül. */
    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

/*
 * TX-task: a stressz-futás. seq = 0..SZ2_TOTAL_PACKETS-1. Minden seq-re egy fix-méretű
 * payloadot tölt, CRC-t számol, és EGYESÉVEL (szerializáltan) elküldi: send -> wait send-cb.
 * NO_MEM esetén delay + retry UGYANARRA a seq-re. A dummy mérési mezők lassan változnak,
 * hogy a master oldalon ne legyenek azonosak (de a teszt szempontjából tetszőlegesek).
 */
static void tx_task(void *arg)
{
    (void)arg;

    /* A node saját STA MAC-je (provisioning: a master ezt tanulja a node_id-hez). */
    uint8_t my_mac[6] = { 0 };
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, my_mac));
    ESP_LOGI(TAG, "Node STA MAC: %02X:%02X:%02X:%02X:%02X:%02X (node_id=%d, csatorna=%d)",
             my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5],
             SZ2_NODE_ID, SZ2_CHANNEL);

    /* boot_id: bootonkent valtozo -> a master ebbol latja a node-resetet (seq ujraindul). */
    uint32_t boot_id = esp_random();
    ESP_LOGI(TAG, "boot_id=0x%08X, kuldendo csomagok: %d", (unsigned)boot_id, SZ2_TOTAL_PACKETS);

    do {
        uint32_t no_mem_retries = 0;
        int64_t  t_start = esp_timer_get_time();

        for (uint32_t seq = 0; seq < SZ2_TOTAL_PACKETS; seq++) {
            sz2_payload_t p;
            memset(&p, 0, sizeof(p));
            p.protocol_version  = SZ2_PROTO_VERSION;
            p.packet_type       = SZ2_PKT_DATA;
            p.node_id           = SZ2_NODE_ID;
            memcpy(p.node_mac, my_mac, 6);
            p.boot_id           = boot_id;
            p.seq               = seq;
            p.packet_idx        = seq;                 /* Sz2: packet_idx == seq */
            p.total_packets     = SZ2_TOTAL_PACKETS;
            p.send_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            /* Dummy mérési mezők — lassan változó, hogy ne legyen konstans (a teszt indifferens). */
            p.weight            = 1000.0f + (float)(seq % 500);
            p.temp              = 20.0f + (float)(seq % 100) * 0.1f;
            p.vbat              = (uint16_t)(3700 - (seq % 200));
            p.crc               = sz2_payload_crc(&p);

            /* Küldés + szerializáció: NO_MEM-re vár-újrapróbál; siker után a send-cb-re blokkol. */
            for (;;) {
                esp_err_t err = esp_now_send(BROADCAST_MAC, (const uint8_t *)&p, sizeof(p));
                if (err == ESP_OK) {
                    /* Egy in-flight küldés: a send-cb visszatéréséig várunk. */
                    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(SZ2_SEND_TIMEOUT_MS)) != pdTRUE) {
                        /* Nem jött send-cb a timeoutban — tovább lépünk, de számoljuk.
                         * (Ritka; a szemafor felszabadulhat később egy lépéssel csúszva — a
                         *  stressz-teszt szempontjából a master riportja a mérvadó.) */
                        s_send_fail_count++;
                    }
                    break;   /* ezt a seq-et elküldtük */
                } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
                    /* Belső TX-FIFO tele -> várunk, majd UGYANEZT a seq-et újraküldjük. */
                    no_mem_retries++;
                    vTaskDelay(pdMS_TO_TICKS(SZ2_NO_MEM_RETRY_MS));
                    /* nem break -> retry */
                } else {
                    /* Egyéb hiba (pl. peer not found) -> logoljuk és tovább (ne ragadjunk be). */
                    s_send_fail_count++;
                    ESP_LOGW(TAG, "esp_now_send hiba seq=%u: %s", (unsigned)seq, esp_err_to_name(err));
                    break;
                }
            }

            if (SZ2_PROGRESS_EVERY > 0 && (seq % SZ2_PROGRESS_EVERY) == 0) {
                ESP_LOGI(TAG, "TX haladas: seq=%u/%d (no_mem_retry=%u)",
                         (unsigned)seq, SZ2_TOTAL_PACKETS, (unsigned)no_mem_retries);
            }

#if SZ2_INTER_PACKET_DELAY_MS > 0
            vTaskDelay(pdMS_TO_TICKS(SZ2_INTER_PACKET_DELAY_MS));
#endif
        }

        int64_t elapsed_ms = (esp_timer_get_time() - t_start) / 1000;
        ESP_LOGI(TAG, "=== SZ2 NODE TX KESZ ===");
        ESP_LOGI(TAG, "elkuldve=%d, no_mem_retry=%u, send_cb_timeout/egyeb=%u, tx_status_fail=%u, ido=%lld ms",
                 SZ2_TOTAL_PACKETS, (unsigned)no_mem_retries,
                 (unsigned)s_send_fail_count, (unsigned)s_tx_status_fail, (long long)elapsed_ms);
        if (elapsed_ms > 0) {
            ESP_LOGI(TAG, "becsult TX sebesseg: ~%lld csomag/s",
                     (long long)((int64_t)SZ2_TOTAL_PACKETS * 1000 / elapsed_ms));
        }
    } while (SZ2_REPEAT);

    /* Vége — a task megáll (a board ébren marad, a master kiírja a vételi riportot). */
    ESP_LOGI(TAG, "TX befejezve, a task var (reset az ujrainditashoz).");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Sz2 NODE (TX) — ESP-NOW low-level send ===");

    /* NVS kell a WiFi-hez (kalibrációs/PHY adatok). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    /* TX-szerializációs szemafor: bináris, kezdetben ÜRES (a send-cb adja meg). */
    s_tx_done = xSemaphoreCreateBinary();
    configASSERT(s_tx_done != NULL);

    wifi_espnow_init();

    /* A TX-task külön taskban — a C3 egymagos, de így a task-kontextus tiszta. */
    xTaskCreate(tx_task, "sz2_tx", 4096, NULL, 5, NULL);
}

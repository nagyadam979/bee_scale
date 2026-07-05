/*
 * Sz6 — ébredés→megszólítás→átadás protokoll · NODE
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz6_wake_handoff/node
 *
 * CÉL
 *   A node (ESP32-C3 SuperMini) MEGBÍZHATÓ ciklusa, tisztán a rádiós protokollra fókuszálva
 *   (semmi periféria, a mérés szimulált):
 *     deep sleep ébredés → WiFi/ESP-NOW init → pre-TX jitter → DATA küldés → app-ACK várás
 *     (timeout + retry + jitter) → eredmény-log → rövid deep sleep.
 *
 * MEGSZÓLÍTÁS / ÁTADÁS (a protokoll magja)
 *   - DISCOVERY: amíg a node nem ismeri a master MAC-jét, BROADCAST-tal küld. A master
 *     app-ACK-ja UNICAST a node-nak -> az ACK src-jéből a node MEGTANULJA a master MAC-jét,
 *     és RTC-memóriában őrzi (deep sleepet túléli, power-ont nem).
 *   - CÍMZETT ÜZEM: a tanult master MAC-re UNICAST megy (MAC-szintű retry + kisebb éter-zaj).
 *   - VISSZAESÉS: ha az unicast próbák nem hoznak app-ACK-ot, a maradék próbák BROADCAST-ra
 *     váltanak (master-csere/áthelyezés ellen), és siker esetén újratanulja a MAC-et.
 *   - Az app-ACK a kézbesítés bizonyítéka (a MAC-szintű ACK broadcastnál nincs, unicastnál
 *     sem app-szintű). A retransmit-duplikátumot a master a seq alapján szűri, de a dup-ra
 *     IS ACK-ol (az első ACK elveszhetett).
 *
 * SKÁLÁZÁS (több node)
 *   - Egy forrás, SZ6_NODE_ID build-flag (platformio.ini: node1/node2 env).
 *   - Pre-TX jitter + retry-jitter + alvásidő-jitter -> két azonos periódusú node nem ragad
 *     tartós fázis-ütközésben; az ütközést az app-retry úgyis felépíti.
 *
 * SEQ / BOOT_ID SZEMANTIKA
 *   - A seq RTC_DATA_ATTR -> deep sleep alatt is monoton nő (ciklusonként +1, sikertelen
 *     ciklusnál IS). Egy teljesen sikertelen ciklus a masternél végleges seq-gap = elveszett
 *     mérés — a bench pass-feltétele, hogy ilyen NE legyen.
 *   - A boot_id power-on resetkor generálódik (esp_random) és RTC-ben él túl minden deep
 *     sleepet -> a master a boot_id-váltásból látja a valódi node-resetet (seq újraindul).
 *
 * SZ2-ÖRÖKSÉG (kötelező fegyelem)
 *   - TX szerializáció: send -> send-cb szemafor (egy in-flight küldés), NO_MEM -> vár-retry.
 *   - PHY-interop fix: explicit 11b/g/n (LR kizárva) + esp_now_set_peer_rate_config(11B/1M_L)
 *     minden peerre, mindkét oldalon.
 *   - recv-cb: csak kimásol + jelez, semmi nehéz munka.
 *
 * BOARD: ESP32-C3 SuperMini. Konzol: natív USB-Serial-JTAG. Deep sleep alatt a port NEM
 *   enumerál -> flash az awake-ablakban (tools/flash_catch.sh), a log a serial_logger.py-jal
 *   az awake-ablakokból gyűjthető. A H1-féle USB-pad-lekapcsolást SZÁNDÉKOSAN nem használjuk
 *   (ez kommunikációs bench, kell a konzol; fogyasztás itt nem szempont).
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
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_idf_version.h"

#include "protocol.h"   /* a BEFAGYASZTOTT közös wire-header (firmware/common) */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* WiFi/ESP-NOW csatorna. EGYEZZEN a master SZ6_CHANNEL-jével! (1..13) */
#define SZ6_CHANNEL                 1

/* A node azonosítója — build-flag adja (platformio.ini env-enként), fallback 1. */
#ifndef SZ6_NODE_ID
#define SZ6_NODE_ID                 1
#endif

/* RÖVIDÍTETT bench-ciklus: deep sleep hossza (s). Éles szimulációnál majd percek. */
#define SZ6_SLEEP_S                 12

/* Alvásidő-jitter (+/- ms): két azonos periódusú node ne ragadjon tartós fázis-ütközésben. */
#define SZ6_SLEEP_JITTER_MS         1000

/* Ébredés utáni, küldés előtti random jitter felső határa (ms) — ütközés-dekorreláció. */
#define SZ6_PRE_TX_JITTER_MAX_MS    150

/* App-ACK várakozási idő próbánként (ms). A teljes RTT padon ~pár ms; a tartalék bőséges. */
#define SZ6_ACK_TIMEOUT_MS          120

/* Összes küldési próba ciklusonként (1 eredeti + retransmitek). */
#define SZ6_MAX_TRIES               5

/* Ennyi sikertelen UNICAST próba után vált a maradék próba BROADCAST-ra (re-discovery). */
#define SZ6_UNICAST_TRIES           2

/* Retry előtti random várakozás sávja (ms) — ütközés-feloldás. */
#define SZ6_RETRY_JITTER_MIN_MS     20
#define SZ6_RETRY_JITTER_MAX_MS     120

/* A send-cb szemafor timeoutja (ms) — MAC-szintű lezárás várása. */
#define SZ6_SEND_TIMEOUT_MS         50

/* ESP_ERR_ESPNOW_NO_MEM esetén ennyit várunk, majd UGYANAZT a próbát ismételjük (ms). */
#define SZ6_NO_MEM_RETRY_MS         2

/* Deep sleep előtti ébrenmaradás (ms): USB-CDC log-flush + a flash-elkapó ablak széle. */
#define SZ6_LINGER_MS               700

/* ================================================================= */

static const char *TAG = "SZ6-NODE";

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* --- RTC-memóriában élő állapot (deep sleepet túléli, power-ont nem) --- */
static RTC_DATA_ATTR uint32_t rtc_boot_id      = 0;      /* power-onkor generált futás-azonosító */
static RTC_DATA_ATTR uint32_t rtc_seq          = 0;      /* monoton mérés-sorszám (ciklusonként +1) */
static RTC_DATA_ATTR uint8_t  rtc_master_mac[6];         /* a tanult master MAC                   */
static RTC_DATA_ATTR uint8_t  rtc_master_known = 0;      /* 0/1: van-e érvényes tanult MAC        */
static RTC_DATA_ATTR uint32_t rtc_ok_cycles    = 0;      /* ACK-kal zárult ciklusok               */
static RTC_DATA_ATTR uint32_t rtc_fail_cycles  = 0;      /* ACK NÉLKÜL zárult ciklusok (= adatvesztés) */
static RTC_DATA_ATTR uint32_t rtc_tries_sum    = 0;      /* összes küldési próba (átlag-try-hoz)  */

/* TX szerializáció (Sz2-fegyelem): a send-cb adja, a küldő erre vár. */
static SemaphoreHandle_t s_tx_done = NULL;

/* App-ACK jelzés: a recv-cb tölti + adja, a fő folyamat erre vár. */
static SemaphoreHandle_t s_ack_sem = NULL;
static volatile uint32_t s_ack_wait_seq = 0;   /* melyik seq ACK-ját várjuk éppen        */
static uint8_t           s_ack_src[6];         /* az ACK feladója (= a master MAC-je)    */

static volatile uint32_t s_tx_status_fail = 0; /* send-cb status FAIL (unicastnál: nincs MAC-ACK) */

/* SEND-CALLBACK — WiFi-task kontextus (NEM ISR): csak a szerializációs szemafort adja.
 * Verzió-guard: az IDF v5.4-ben a szignatúra esp_now_send_info_t-re változott. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) s_tx_status_fail++;
    xSemaphoreGive(s_tx_done);
}
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) s_tx_status_fail++;
    xSemaphoreGive(s_tx_done);
}
#endif

/* RECV-CALLBACK — WiFi-task kontextus: csak szűr + kimásol + szemafort ad. A várt ACK:
 * jó hossz, jó verzió, ACK típus, az ÉN node_id-m, és a MOST várt seq visszaigazolása. */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != (int)sizeof(sz2_ack_t)) return;
    const sz2_ack_t *ack = (const sz2_ack_t *)data;
    if (ack->protocol_version != SZ2_PROTO_VERSION) return;
    if (ack->packet_type != SZ2_PKT_ACK) return;
    if (ack->node_id != SZ6_NODE_ID) return;
    if (ack->ack_last_seq != s_ack_wait_seq) return;
    memcpy((void *)s_ack_src, info->src_addr, 6);
    xSemaphoreGive(s_ack_sem);
}

/* Peer felvétele/biztosítása + a fix robusztus ráta (11b/1M_L) rákonfigurálása.
 * A rate-config az Sz2 PHY-interop fixének fele; a másik fele a set_protocol az initben. */
static void ensure_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { 0 };
        memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
        peer.channel = 0;              /* 0 = aktuális csatorna */
        peer.ifidx   = WIFI_IF_STA;
        peer.encrypt = false;
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate    = WIFI_PHY_RATE_1M_L,
        .ersu    = false,
        .dcm     = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(mac, &rc));
}

/* WiFi + ESP-NOW init a kötelező sorrendben (Sz2-minta):
 * netif -> event loop -> wifi_init -> storage RAM -> mode STA -> start -> PS_NONE ->
 * protocol (LR kizárva) -> set_channel (START UTÁN!) -> esp_now_init -> cb-k -> peerek. */
static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Az ACK-vétel a küldés utáni ~100 ms-ban jön — modem-sleep ne nyelje el. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* Interop-hardening (Sz2): explicit 11b/g/n, az LR bit KIZÁRVA. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ6_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    ensure_peer(BROADCAST_MAC);
    if (rtc_master_known) {
        ensure_peer(rtc_master_mac);
    }
}

/* Egy DATA csomag elküldése szerializáltan (send -> send-cb várás; NO_MEM -> vár-retry). */
static void send_serialized(const uint8_t *dest, const sz2_payload_t *p)
{
    for (;;) {
        esp_err_t err = esp_now_send(dest, (const uint8_t *)p, sizeof(*p));
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(SZ6_SEND_TIMEOUT_MS));
            return;
        } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(SZ6_NO_MEM_RETRY_MS));
        } else {
            ESP_LOGW(TAG, "esp_now_send hiba: %s", esp_err_to_name(err));
            return;
        }
    }
}

/* random szám [lo..hi] zárt intervallumban. */
static uint32_t rand_between(uint32_t lo, uint32_t hi)
{
    return lo + (esp_random() % (hi - lo + 1));
}

static void cycle_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();

    uint8_t my_mac[6] = { 0 };
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, my_mac));

    /* Ez a ciklus mérése — a seq már app_main-ben nőtt. Szimulált mérési értékek. */
    sz2_payload_t p;
    memset(&p, 0, sizeof(p));
    p.protocol_version = SZ2_PROTO_VERSION;
    p.packet_type      = SZ2_PKT_DATA;
    p.node_id          = SZ6_NODE_ID;
    memcpy(p.node_mac, my_mac, 6);
    p.boot_id          = rtc_boot_id;
    p.seq              = rtc_seq;
    p.total_packets    = 0;
    p.weight           = 25000.0f + SZ6_NODE_ID * 1000.0f + (float)(rtc_seq % 100);
    p.temp             = 20.0f + (float)(rtc_seq % 50) * 0.1f;
    p.vbat             = 3700;

    s_ack_wait_seq = rtc_seq;

    /* Pre-TX jitter: az azonos pillanatban ébredő node-ok széthúzása. */
    vTaskDelay(pdMS_TO_TICKS(rand_between(0, SZ6_PRE_TX_JITTER_MAX_MS)));

    bool ok = false;
    uint32_t tries = 0;
    char dest_mode = '?';

    for (uint32_t t = 1; t <= SZ6_MAX_TRIES && !ok; t++) {
        tries = t;
        /* Cél-kiválasztás: tanult master + a keret elején -> UNICAST; különben BROADCAST. */
        const uint8_t *dest;
        if (rtc_master_known && t <= SZ6_UNICAST_TRIES) {
            dest = rtc_master_mac;
            dest_mode = 'U';
        } else {
            dest = BROADCAST_MAC;
            dest_mode = (rtc_master_known && tries > SZ6_UNICAST_TRIES) ? 'F' : 'B';
        }

        p.packet_idx        = t;   /* diagnosztika: hányadik próba ment át (master logolja) */
        p.send_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        p.crc               = sz2_payload_crc(&p);

        send_serialized(dest, &p);

        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(SZ6_ACK_TIMEOUT_MS)) == pdTRUE) {
            ok = true;
            /* Átadás lezárva — a master MAC-jének (újra)tanulása az ACK feladójából. */
            if (!rtc_master_known || memcmp(rtc_master_mac, s_ack_src, 6) != 0) {
                memcpy(rtc_master_mac, s_ack_src, 6);
                rtc_master_known = 1;
                ensure_peer(rtc_master_mac);
                ESP_LOGI(TAG, "master tanulva: %02X:%02X:%02X:%02X:%02X:%02X",
                         s_ack_src[0], s_ack_src[1], s_ack_src[2],
                         s_ack_src[3], s_ack_src[4], s_ack_src[5]);
            }
        } else if (t < SZ6_MAX_TRIES) {
            /* Ütközés-feloldó random várakozás a retransmit előtt. */
            vTaskDelay(pdMS_TO_TICKS(rand_between(SZ6_RETRY_JITTER_MIN_MS,
                                                  SZ6_RETRY_JITTER_MAX_MS)));
        }
    }

    if (ok) rtc_ok_cycles++; else rtc_fail_cycles++;
    rtc_tries_sum += tries;

    uint32_t awake_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    /* GÉP-GREP-ELHETŐ ciklus-összesítő — a bench kiértékelés erre a sorra épül. */
    ESP_LOGI(TAG, "SZ6 CYCLE node=%d seq=%u result=%s tries=%u dest=%c awake_ms=%u "
                  "ok_total=%u fail_total=%u txfail=%u",
             SZ6_NODE_ID, (unsigned)rtc_seq, ok ? "OK" : "FAIL", (unsigned)tries,
             dest_mode, (unsigned)awake_ms, (unsigned)rtc_ok_cycles,
             (unsigned)rtc_fail_cycles, (unsigned)s_tx_status_fail);

    /* Konzol-flush + flash-elkapó ablak, majd tiszta rádió-leállás és alvás. */
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(SZ6_LINGER_MS));
    esp_now_deinit();
    esp_wifi_stop();

    int32_t jitter = (int32_t)rand_between(0, 2 * SZ6_SLEEP_JITTER_MS) - SZ6_SLEEP_JITTER_MS;
    int64_t sleep_us = (int64_t)SZ6_SLEEP_S * 1000000 + (int64_t)jitter * 1000;
    esp_deep_sleep((uint64_t)sleep_us);
}

void app_main(void)
{
    /* Új futás detektálása. NEM a wakeup cause-t nézzük, hanem a reset-okot: az RTC-memória
     * MINDEN nem-power-on resetet túlél (pl. a host port-nyitása által kiváltott
     * USB_UART_CHIP_RESET-et is — lásd a bench tanulságát), ilyenkor a seq/boot_id/master-MAC
     * folytatódik. Csak valódi power-on/brownout nullaz. */
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rtc_boot_id == 0) {
        rtc_boot_id      = esp_random();
        rtc_seq          = 0;
        rtc_master_known = 0;
        rtc_ok_cycles    = 0;
        rtc_fail_cycles  = 0;
        rtc_tries_sum    = 0;
        ESP_LOGI(TAG, "=== Sz6 NODE — POWER-ON, boot_id=0x%08X node_id=%d csatorna=%d "
                      "sleep=%ds ===", (unsigned)rtc_boot_id, SZ6_NODE_ID, SZ6_CHANNEL,
                 SZ6_SLEEP_S);
    }
    rtc_seq++;   /* minden ciklus (ébredés) új mérés-sorszám — sikertelen ciklusnál is nő */

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    s_tx_done = xSemaphoreCreateBinary();
    s_ack_sem = xSemaphoreCreateBinary();
    configASSERT(s_tx_done != NULL && s_ack_sem != NULL);

    wifi_espnow_init();

    xTaskCreate(cycle_task, "sz6_cycle", 4096, NULL, 5, NULL);
}

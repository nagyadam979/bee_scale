/*
 * Sz2b — master WiFi STA + ESP-NOW koegzisztencia · NODE
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz2b_coexistence_packet_loss/node
 *
 * EREDET: az Sz6 (wake→megszólítás→átadás, PASS) node-forrásának adaptációja. A protokoll
 * (discovery→unicast, app-ACK, retry+jitter, seq/boot_id szemantika) VÁLTOZATLAN — az
 * Sz6 README dokumentálja. Az ÚJ réteg a CSATORNA-KÖVETÉS:
 *
 * CSATORNA-SCAN DISCOVERY (az Sz2b kulcsproblémája)
 *   A master STA-ként az AP-hoz kapcsolódik -> a csatornát az AP diktálja (1..13 bármi,
 *   akár változhat is). A node ezért NEM fix csatornán küld, hanem:
 *     - ha van RTC-ben cache-elt csatorna (deep sleepet túléli): azon próbálkozik a normál
 *       Sz6 try-protokollal (unicast a tanult masterre, broadcast-fallback);
 *     - ha nincs cache, VAGY SZ2B_RESCAN_AFTER_FAILS egymást követő ciklus ACK nélkül
 *       zárult: VÉGIGPÁSZTÁZZA az 1..13 csatornákat — csatornánként broadcast DATA + rövid
 *       ACK-várás; ahol ACK jön, azt a csatornát cache-eli (és a master MAC-et tanulja).
 *   A re-scan a SIKERTELEN normál próbák UTÁN, MÉG UGYANABBAN a ciklusban fut le, hogy a
 *   mérés (seq) lehetőleg ne vesszen el, csak az awake-idő nőjön a hibás esetben.
 *
 * SEQ / BOOT_ID SZEMANTIKA (Sz6): a seq RTC-ben, ciklusonként +1 (sikertelen ciklusnál IS)
 *   -> a masternél a végleges seq-gap = elveszett mérés. A boot_id csak valódi power-on/
 *   brownout resetnél generálódik újra (esp_reset_reason() gating — az USB-reset túléli).
 *
 * SZ2-ÖRÖKSÉG (kötelező fegyelem): TX szerializáció send-cb szemaforral, NO_MEM vár-retry,
 *   PHY-fix (explicit 11b/g/n LR nélkül + esp_now_set_peer_rate_config 11B/1M_L minden
 *   peerre), recv-cb csak szűr+kimásol+szemafort ad.
 *
 * BOARD: ESP32-C3 SuperMini. Konzol: natív USB-Serial-JTAG (deep sleep alatt NEM enumerál
 *   -> flash: ../../Sz6_wake_handoff/tools/flash_catch.sh). ⚠️ node2: SZ2B_TX_POWER=34
 *   (8,5 dBm) KÖTELEZŐ — a példány antenna-hibás, teljes poweren ~4% a kézbesítése (Sz6).
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

/* A node azonosítója — build-flag adja (platformio.ini env-enként), fallback 1. */
#ifndef SZ2B_NODE_ID
#define SZ2B_NODE_ID                1
#endif

/* Max TX-teljesítmény 0.25 dBm egységben (84 = 21 dBm, 34 = 8,5 dBm). A C3 SuperMini
 * ismert antenna-illesztési hibája PÉLDÁNYFÜGGŐ (Sz6: node2 csak 34-gyel megy). */
#ifndef SZ2B_TX_POWER
#define SZ2B_TX_POWER               84
#endif

/* RÖVIDÍTETT bench-ciklus: deep sleep hossza (s). Éles szimulációnál majd percek. */
#define SZ2B_SLEEP_S                12

/* Alvásidő-jitter (+/- ms): két azonos periódusú node ne ragadjon tartós fázis-ütközésben. */
#define SZ2B_SLEEP_JITTER_MS        1000

/* Ébredés utáni, küldés előtti random jitter felső határa (ms) — ütközés-dekorreláció. */
#define SZ2B_PRE_TX_JITTER_MAX_MS   150

/* App-ACK várakozási idő próbánként (ms) a NORMÁL (cache-elt csatornás) próbáknál. */
#define SZ2B_ACK_TIMEOUT_MS         120

/* Összes NORMÁL küldési próba ciklusonként (1 eredeti + retransmitek). */
#define SZ2B_MAX_TRIES              5

/* Ennyi sikertelen UNICAST próba után vált a maradék próba BROADCAST-ra (re-discovery). */
#define SZ2B_UNICAST_TRIES          2

/* Retry előtti random várakozás sávja (ms) — ütközés-feloldás. */
#define SZ2B_RETRY_JITTER_MIN_MS    20
#define SZ2B_RETRY_JITTER_MAX_MS    120

/* --- Csatorna-scan discovery --- */
/* Pásztázott csatornasáv (EU: 1..13). */
#define SZ2B_SCAN_CH_FIRST          1
#define SZ2B_SCAN_CH_LAST           13
/* ACK-várás csatornánként a scan alatt (ms). A padon az ACK RTT ~pár ms; a 80 ms bő
 * ráhagyás úgy, hogy egy teljes sweep ~1,2 s maradjon. */
#define SZ2B_SCAN_ACK_TIMEOUT_MS    80
/* Hány teljes 1..13 sweep fér egy ciklusba, mielőtt feladjuk (FAIL + alvás). */
#define SZ2B_SCAN_SWEEPS            3
/* Ennyi EGYMÁST KÖVETŐ, ACK nélkül zárult NORMÁL ciklus után indul re-scan (az 1 hibás
 * ciklus lehet sima ütközés — arra kár teljes sweepet égetni). */
#define SZ2B_RESCAN_AFTER_FAILS     2

/* A send-cb szemafor timeoutja (ms) — MAC-szintű lezárás várása. */
#define SZ2B_SEND_TIMEOUT_MS        50

/* ESP_ERR_ESPNOW_NO_MEM esetén ennyit várunk, majd UGYANAZT a próbát ismételjük (ms). */
#define SZ2B_NO_MEM_RETRY_MS        2

/* Deep sleep előtti ébrenmaradás (ms): USB-CDC log-flush + a flash-elkapó ablak széle. */
#define SZ2B_LINGER_MS              2000

/* Hányszor ismételje a CYCLE összesítő sort a linger alatt (host-open utáni elkapáshoz). */
#define SZ2B_CYCLE_LOG_REPEAT       5

/* ================================================================= */

static const char *TAG = "SZ2B-NODE";

static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* --- RTC-memóriában élő állapot (deep sleepet túléli, power-ont nem) --- */
static RTC_DATA_ATTR uint32_t rtc_boot_id      = 0;   /* power-onkor generált futás-azonosító */
static RTC_DATA_ATTR uint32_t rtc_seq          = 0;   /* monoton mérés-sorszám (ciklusonként +1) */
static RTC_DATA_ATTR uint8_t  rtc_master_mac[6];      /* a tanult master MAC                   */
static RTC_DATA_ATTR uint8_t  rtc_master_known = 0;   /* 0/1: van-e érvényes tanult MAC        */
static RTC_DATA_ATTR uint8_t  rtc_channel      = 0;   /* a tanult csatorna (0 = ismeretlen -> scan) */
static RTC_DATA_ATTR uint8_t  rtc_noack_streak = 0;   /* egymást követő ACK nélküli ciklusok   */
static RTC_DATA_ATTR uint32_t rtc_ok_cycles    = 0;   /* ACK-kal zárult ciklusok               */
static RTC_DATA_ATTR uint32_t rtc_fail_cycles  = 0;   /* ACK NÉLKÜL zárult ciklusok (= adatvesztés) */
static RTC_DATA_ATTR uint32_t rtc_tries_sum    = 0;   /* összes küldési próba (átlag-try-hoz)  */
static RTC_DATA_ATTR uint32_t rtc_scan_cycles  = 0;   /* hány ciklusban futott csatorna-scan   */

/* TX szerializáció (Sz2-fegyelem): a send-cb adja, a küldő erre vár. */
static SemaphoreHandle_t s_tx_done = NULL;

/* App-ACK jelzés: a recv-cb tölti + adja, a fő folyamat erre vár. */
static SemaphoreHandle_t s_ack_sem = NULL;
static volatile uint32_t s_ack_wait_seq = 0;   /* melyik seq ACK-ját várjuk éppen        */
static uint8_t           s_ack_src[6];         /* az ACK feladója (= a master MAC-je)    */
static volatile int8_t   s_ack_rssi = -128;    /* az utolsó vett ACK RSSI-je (scan-hoz)  */

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
    if (ack->node_id != SZ2B_NODE_ID) return;
    if (ack->ack_last_seq != s_ack_wait_seq) return;
    memcpy((void *)s_ack_src, info->src_addr, 6);
    s_ack_rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : -128;
    xSemaphoreGive(s_ack_sem);
}

/* Peer felvétele/biztosítása + a fix robusztus ráta (11b/1M_L) rákonfigurálása.
 * channel=0 = MINDIG az aktuális csatorna — a scan alatt csatornát váltunk, a peer követi. */
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

/* WiFi + ESP-NOW init a kötelező sorrendben (Sz2-minta). A csatornát ITT MÉG NEM állítjuk
 * fixre — a cycle_task dönti el (cache-elt csatorna vagy scan). */
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

    int8_t txp_before = 0;
    esp_wifi_get_max_tx_power(&txp_before);
    esp_wifi_set_max_tx_power(SZ2B_TX_POWER);
    int8_t txp_after = 0;
    esp_wifi_get_max_tx_power(&txp_after);
    ESP_LOGI(TAG, "max_tx_power: %d -> %d (0.25 dBm egyseg)", (int)txp_before, (int)txp_after);

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
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(SZ2B_SEND_TIMEOUT_MS));
            return;
        } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(SZ2B_NO_MEM_RETRY_MS));
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

/* A master MAC-jének (újra)tanulása a vett ACK feladójából. */
static void learn_master_from_ack(void)
{
    if (!rtc_master_known || memcmp(rtc_master_mac, s_ack_src, 6) != 0) {
        memcpy(rtc_master_mac, s_ack_src, 6);
        rtc_master_known = 1;
        ensure_peer(rtc_master_mac);
        ESP_LOGI(TAG, "master tanulva: %02X:%02X:%02X:%02X:%02X:%02X",
                 s_ack_src[0], s_ack_src[1], s_ack_src[2],
                 s_ack_src[3], s_ack_src[4], s_ack_src[5]);
    }
}

/* NORMÁL átadás a cache-elt csatornán — az Sz6 try-protokoll változatlanul:
 * unicast a tanult masterre, SZ2B_UNICAST_TRIES után broadcast-fallback, retry-jitter.
 * A packet_idx a cikluson belüli PRÓBA-sorszám (a scan-próbákkal folytatólagos). */
static bool try_deliver(sz2_payload_t *p, uint32_t *tries, char *dest_mode)
{
    for (uint32_t t = 1; t <= SZ2B_MAX_TRIES; t++) {
        (*tries)++;
        const uint8_t *dest;
        if (rtc_master_known && t <= SZ2B_UNICAST_TRIES) {
            dest = rtc_master_mac;
            *dest_mode = 'U';
        } else {
            dest = BROADCAST_MAC;
            *dest_mode = (rtc_master_known && t > SZ2B_UNICAST_TRIES) ? 'F' : 'B';
        }

        p->packet_idx        = *tries;
        p->send_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        p->crc               = sz2_payload_crc(p);

        send_serialized(dest, p);

        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(SZ2B_ACK_TIMEOUT_MS)) == pdTRUE) {
            learn_master_from_ack();
            return true;
        }
        if (t < SZ2B_MAX_TRIES) {
            vTaskDelay(pdMS_TO_TICKS(rand_between(SZ2B_RETRY_JITTER_MIN_MS,
                                                  SZ2B_RETRY_JITTER_MAX_MS)));
        }
    }
    return false;
}

/* CSATORNA-SCAN DISCOVERY: 1..13 csatornán broadcast DATA + rövid ACK-várás. Az ACK-nál
 * NEM állunk meg (P3 tanulság: erős jelnél a szomszéd csatorna átszűrődése miatt az ACK
 * a ROSSZ — a valóditól ±1-2 — csatornán is hallható, és az első találat félretanít):
 * a TELJES sweep végén a LEGERŐSEBB ACK-RSSI-jű csatornát cache-eljük — a valódi csatornán
 * az ACK szűrő-csillapítás nélkül, mindig erősebben jön. A DATA már az első ACK-kal
 * kézbesítve (a master a DUP-okat szűri és újra-ACK-olja). Max SZ2B_SCAN_SWEEPS sweep. */
static bool scan_deliver(sz2_payload_t *p, uint32_t *tries, char *dest_mode)
{
    *dest_mode = 'S';
    rtc_scan_cycles++;
    for (int sweep = 1; sweep <= SZ2B_SCAN_SWEEPS; sweep++) {
        int best_ch   = 0;
        int best_rssi = -128;
        for (int ch = SZ2B_SCAN_CH_FIRST; ch <= SZ2B_SCAN_CH_LAST; ch++) {
            ESP_ERROR_CHECK(esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE));
            /* Elkésett (csatornaváltás közben érkezett) ACK jelzésének ürítése, hogy a
             * mostani ablakban CSAK az ezen a csatornán hallott ACK számítson. */
            while (xSemaphoreTake(s_ack_sem, 0) == pdTRUE) { }
            (*tries)++;
            p->packet_idx        = *tries;
            p->send_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            p->crc               = sz2_payload_crc(p);

            send_serialized(BROADCAST_MAC, p);

            if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(SZ2B_SCAN_ACK_TIMEOUT_MS)) == pdTRUE) {
                int r = (int)s_ack_rssi;
                ESP_LOGI(TAG, "scan ACK: ch=%d rssi=%d (sweep=%d)", ch, r, sweep);
                if (r > best_rssi) {
                    best_rssi = r;
                    best_ch   = ch;
                }
            }
        }
        if (best_ch != 0) {
            rtc_channel = (uint8_t)best_ch;
            learn_master_from_ack();
            ESP_ERROR_CHECK(esp_wifi_set_channel(best_ch, WIFI_SECOND_CHAN_NONE));
            ESP_LOGI(TAG, "scan OK: csatorna=%d rssi=%d (sweep=%d)", best_ch, best_rssi, sweep);
            return true;
        }
        ESP_LOGW(TAG, "scan sweep %d/%d: nincs ACK egyik csatornan sem",
                 sweep, SZ2B_SCAN_SWEEPS);
    }
    return false;
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
    p.node_id          = SZ2B_NODE_ID;
    memcpy(p.node_mac, my_mac, 6);
    p.boot_id          = rtc_boot_id;
    p.seq              = rtc_seq;
    p.total_packets    = 0;
    p.weight           = 25000.0f + SZ2B_NODE_ID * 1000.0f + (float)(rtc_seq % 100);
    p.temp             = 20.0f + (float)(rtc_seq % 50) * 0.1f;
    p.vbat             = 3700;

    s_ack_wait_seq = rtc_seq;

    /* Pre-TX jitter: az azonos pillanatban ébredő node-ok széthúzása. */
    vTaskDelay(pdMS_TO_TICKS(rand_between(0, SZ2B_PRE_TX_JITTER_MAX_MS)));

    bool ok = false;
    bool scanned = false;
    uint32_t tries = 0;
    char dest_mode = '?';

    /* 1) Normál próbák a cache-elt csatornán (ha van). */
    bool have_ch = (rtc_channel >= SZ2B_SCAN_CH_FIRST && rtc_channel <= SZ2B_SCAN_CH_LAST);
    if (have_ch) {
        ESP_ERROR_CHECK(esp_wifi_set_channel(rtc_channel, WIFI_SECOND_CHAN_NONE));
        ok = try_deliver(&p, &tries, &dest_mode);
    }

    /* 2) Scan: nincs cache, VAGY a streak elérte a küszöböt (a mostani hibával együtt).
     * Még ebben a ciklusban fut, hogy a seq lehetőleg ne vesszen el. */
    if (!ok) {
        bool need_scan = !have_ch;
        if (have_ch) {
            rtc_noack_streak++;
            if (rtc_noack_streak >= SZ2B_RESCAN_AFTER_FAILS) need_scan = true;
        }
        if (need_scan) {
            scanned = true;
            ok = scan_deliver(&p, &tries, &dest_mode);
            if (!ok) {
                /* Sikertelen sweep-ek: a cache-t eldobjuk -> a következő ciklus azonnal
                 * scannel (master-kiesés alatt ez a leggyorsabb visszatalálás). */
                rtc_channel = 0;
            }
        }
    }

    if (ok) {
        rtc_ok_cycles++;
        rtc_noack_streak = 0;
    } else {
        rtc_fail_cycles++;
    }
    rtc_tries_sum += tries;

    uint32_t awake_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    /* GÉP-GREP-ELHETŐ ciklus-összesítő — a bench kiértékelés erre a sorra épül.
     * Ismételve a linger-ablakban: a USJ-konzol kimenete host-open ELŐTT elveszik,
     * a host-logger csak az enumeráció után ~0,5-1 s-mal nyit. A parse (node,seq)-re dedupol. */
    for (int r = 0; r < SZ2B_CYCLE_LOG_REPEAT; r++) {
        ESP_LOGI(TAG, "SZ2B CYCLE node=%d seq=%u result=%s tries=%u dest=%c ch=%u scan=%d "
                      "streak=%u awake_ms=%u ok_total=%u fail_total=%u scans_total=%u txfail=%u",
                 SZ2B_NODE_ID, (unsigned)rtc_seq, ok ? "OK" : "FAIL", (unsigned)tries,
                 dest_mode, (unsigned)rtc_channel, scanned ? 1 : 0,
                 (unsigned)rtc_noack_streak, (unsigned)awake_ms, (unsigned)rtc_ok_cycles,
                 (unsigned)rtc_fail_cycles, (unsigned)rtc_scan_cycles,
                 (unsigned)s_tx_status_fail);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(SZ2B_LINGER_MS / SZ2B_CYCLE_LOG_REPEAT));
    }
    esp_now_deinit();
    esp_wifi_stop();

    int32_t jitter = (int32_t)rand_between(0, 2 * SZ2B_SLEEP_JITTER_MS) - SZ2B_SLEEP_JITTER_MS;
    int64_t sleep_us = (int64_t)SZ2B_SLEEP_S * 1000000 + (int64_t)jitter * 1000;
    esp_deep_sleep((uint64_t)sleep_us);
}

#ifdef SZ2B_RXTEST
/* =================== DIAGNOSZTIKAI RX-TESZT (promiscuous sniffer) ===================
 * Build: env node2rx (platformio.ini) — a node NEM alszik, a megadott csatorna forgalmát
 * számolja és minden frame src MAC + RSSI párosát logolja. Az Sz6 SZ6_RXTEST mintája,
 * paraméterezhető csatornával (SZ2B_SNIFF_CHANNEL, default 8 = a dlink AP csatornája).
 * CÉL (Sz2b): C3↔C3 link-budget kontrollmérés — a node1 broadcastjait egy MÁSIK C3
 * mennyivel erősebben hallja, mint az S3-Zero master (gyanú: az S3-Zero kerámia-antenna
 * a link-budget szűk keresztmetszete). A master ACK-jainak RSSI-je (21 dBm TX) a
 * S3→C3 irányt is méri. */
#ifndef SZ2B_SNIFF_CHANNEL
#define SZ2B_SNIFF_CHANNEL 8
#endif
static volatile uint32_t s_sniff_total = 0;
static volatile uint32_t s_sniff_espnow = 0;

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *d = pkt->payload;
    (void)type;
    s_sniff_total++;
    uint8_t ftype = (d[0] >> 2) & 0x3;
    uint8_t fsub  = (d[0] >> 4) & 0xF;
    /* Csak a management/action frame-eket logoljuk soronként (ESP-NOW = vendor action). */
    if (ftype == 0 && fsub == 0xD) {
        bool is_espnow = (pkt->rx_ctrl.sig_len > 27 && d[24] == 0x7F &&
                          d[25] == 0x18 && d[26] == 0xfe && d[27] == 0x34);
        if (is_espnow) s_sniff_espnow++;
        ESP_LOGI(TAG, "SNIFF FRAME src=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d espnow=%d",
                 d[10], d[11], d[12], d[13], d[14], d[15],
                 (int)pkt->rx_ctrl.rssi, is_espnow ? 1 : 0);
    }
}

void app_main(void)
{
    ESP_LOGW(TAG, "=== Sz2b NODE — DIAG RX-TESZT (sniffer, csatorna=%d) ===",
             SZ2B_SNIFF_CHANNEL);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(e);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ2B_SNIFF_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffer_cb));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGW(TAG, "SNIFF STAT total=%u espnow=%u",
                 (unsigned)s_sniff_total, (unsigned)s_sniff_espnow);
    }
}
#else
void app_main(void)
{
    /* Új futás detektálása. NEM a wakeup cause-t nézzük, hanem a reset-okot: az RTC-memória
     * MINDEN nem-power-on resetet túlél (pl. a host port-nyitása által kiváltott
     * USB_UART_CHIP_RESET-et is — Sz6 tanulság), ilyenkor a seq/boot_id/master-MAC/csatorna
     * folytatódik. Csak valódi power-on/brownout nullaz. */
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT || rtc_boot_id == 0) {
        rtc_boot_id      = esp_random();
        rtc_seq          = 0;
        rtc_master_known = 0;
        rtc_channel      = 0;
        rtc_noack_streak = 0;
        rtc_ok_cycles    = 0;
        rtc_fail_cycles  = 0;
        rtc_tries_sum    = 0;
        rtc_scan_cycles  = 0;
        ESP_LOGI(TAG, "=== Sz2b NODE — POWER-ON, boot_id=0x%08X node_id=%d sleep=%ds "
                      "(csatorna: scan) ===", (unsigned)rtc_boot_id, SZ2B_NODE_ID,
                 SZ2B_SLEEP_S);
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

    xTaskCreate(cycle_task, "sz2b_cycle", 4096, NULL, 5, NULL);
}
#endif /* SZ2B_RXTEST */

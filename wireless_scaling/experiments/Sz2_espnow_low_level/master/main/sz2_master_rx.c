/*
 * Sz2 — ESP-NOW alacsony szint · MASTER (RX)
 * bee_scale / wireless_scaling · 2. fázis · experiments/Sz2_espnow_low_level/master
 *
 * CÉL
 *   A master (classic ESP-WROOM-32) oldali ESP-NOW RECV-fegyelem + a stressz-futás
 *   kiértékelése: veszi a node broadcast-csomagjait, CRC-zi, node-onként seq-statisztikát
 *   vezet (vett / hiányzó / duplikált / out-of-order / queue-drop / crc-error), és a végén
 *   egy JÓL OLVASHATÓ, gép-grep-elhető "=== SZ2 RIPORT ===" összesítőt nyomtat. Ez a sor a
 *   PASS/FAIL alapja: 1000 vett, 0 missing, 0 dropped, 0 crash = PASS.
 *
 * CALLBACK-FEGYELEM (a kutatás eredménye, NEM opcionális)
 *   Az ESP-NOW recv-cb WiFi-task kontextusban fut (NEM ISR) -> sima xQueueSend(q,&item,0)
 *   (non-blocking, timeout 0), NEM a *FromISR variáns (a projekt korábbi jegyzete tévesen
 *   xQueueSendFromISR-t említ; tudatosan a sima API). A recv-cb CSAK kimásolja a payloadot
 *   (érték szerint, fix struct memcpy — NINCS per-csomag malloc), a src MAC-et és az RSSI-t
 *   (mindkettő CSAK a callbackben érvényes!) egy queue-elembe, és queue-ba dob. SEMMI
 *   feldolgozás/log/CRC a callbackben. A nehéz munka a consumer-taskban (másik magon) megy.
 *   Ha a queue tele -> dropped_count++ (volatile), a logolás a consumerből.
 *
 * VERZIÓ-GUARD (recv)
 *   Az ESP-IDF v5.x egységesen a `const esp_now_recv_info_t *info` szignatúrát használja
 *   (a régi const uint8_t *mac_addr forma v5 előtti) -> itt a v5-ös formát használjuk.
 *
 * MASTER-SPECIFIKUS KRITIKUS BEÁLLÍTÁS
 *   esp_wifi_set_ps(WIFI_PS_NONE): különben a modem-sleep folyamatos vételnél csomagvesztést
 *   okoz. A masteren ez KRITIKUS (a node-on ártalmatlan lenne).
 *
 * BOARD: classic ESP-WROOM-32 (dual-core Xtensa, USB-UART bridge, UART0 konzol). A log
 *   háttérből OLVASHATÓ -> a végső riport itt jelenik meg. A consumer-taskot core 1-re
 *   pinneljük, hogy a WiFi/recv-cb (core 0) ne versenyezzen a feldolgozással.
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
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"

#include "sz2_protocol.h"   /* a KÖZÖS header (../../common) */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* WiFi/ESP-NOW csatorna. EGYEZZEN a node SZ2_CHANNEL-jével! (1..13) */
#define SZ2_CHANNEL             1

/* Vételi queue mélysége (vételi burst pufferelése a recv-cb és a consumer közt). */
#define SZ2_RX_QUEUE_LEN        64

/* Hány node-ot tartunk nyilván (a bench 1 node; ez tartótábla a jövőre). */
#define SZ2_MAX_NODES           4

/* A várt csomagszám node-onként (a hiányzó/bitmap mérete). A payload total_packets-ből is
 * frissül; ez a felső korlát / fallback. */
#define SZ2_EXPECT_TOTAL        1000

/* Ha ennyi ideig (ms) NINCS új csomag, a master kinyomtatja a VÉGSŐ összesítőt (idle-riport). */
#define SZ2_IDLE_REPORT_MS      2000

/* Hány vett csomagonként írjon egy rövid haladás-sort. */
#define SZ2_PROGRESS_EVERY      100

/* Ha a hiányzó seq-ek száma <= ennyi, kilistázzuk őket a riportban (különben csak a db-ot). */
#define SZ2_MISSING_LIST_MAX    32

/* ================================================================= */

static const char *TAG = "SZ2-MASTER";

/* A bitmap mérete byte-ban (SZ2_EXPECT_TOTAL bit). */
#define SZ2_BITMAP_BYTES   ((SZ2_EXPECT_TOTAL + 7) / 8)

/* Egy vételi queue-elem: a teljes payload + a CALLBACKBEN érvényes src MAC és RSSI kimásolva. */
typedef struct {
    sz2_payload_t payload;
    uint8_t       src_mac[6];
    int8_t        rssi;
    uint16_t      len;        /* a vett hossz (a consumer ellenőrzi == sizeof(payload)) */
} sz2_rx_item_t;

/* Node-onkénti statisztika. */
typedef struct {
    bool     in_use;
    uint8_t  node_id;
    uint8_t  mac[6];
    uint32_t boot_id;
    uint32_t total_packets;       /* a payloadból tanult várt db                     */
    uint32_t received_unique;     /* egyedi (bitmap szerint elsőre látott) seq-ek     */
    uint32_t duplicate_seq;       /* már látott seq újra                              */
    uint32_t out_of_order;        /* seq < eddigi max_seq                             */
    uint32_t crc_error;           /* rossz CRC (NEM számít az adat-statisztikába)     */
    uint32_t max_seq;             /* a legnagyobb látott seq                          */
    int64_t  first_rx_us;
    int64_t  last_rx_us;
    int32_t  rssi_min;
    int32_t  rssi_max;
    int64_t  rssi_sum;
    uint32_t rssi_n;
    bool     reported;            /* a végső riportot kiírtuk-e már erre a node-ra    */
    uint8_t  seen[SZ2_BITMAP_BYTES];
} sz2_node_stat_t;

static QueueHandle_t   s_rx_queue = NULL;
static volatile uint32_t s_dropped_count = 0;   /* queue tele a recv-cb-ben (volatile)      */
static sz2_node_stat_t s_nodes[SZ2_MAX_NODES];

/* DIAGNOSZTIKAI szamlalok — hol halnak meg a csomagok (a nema continue-utak lathatova tetele). */
static volatile uint32_t s_diag_recv_cb   = 0;  /* hanyszor futott a recv_cb (= esp_now atadta)*/
static volatile uint32_t s_diag_len_bad   = 0;  /* rossz hosszusagu csomag                     */
static volatile uint32_t s_diag_ver_bad   = 0;  /* rossz protocol_version                       */
static volatile uint32_t s_diag_crc_bad   = 0;  /* CRC-hiba                                     */
static volatile uint32_t s_diag_ok        = 0;  /* len+ver+crc OK, feldolgozva                  */

/*
 * RECV-CALLBACK. WiFi-task kontextusban fut (NEM ISR) -> sima xQueueSend(...,0).
 * CSAK kimásol + queue-ba dob. Az info->src_addr és info->rx_ctrl->rssi CSAK itt érvényes
 * (lokális) -> a queue-elembe másoljuk. A payloadot ÉRTÉK szerint (fix struct memcpy).
 * Ha a queue tele -> s_dropped_count++ (a logolás a consumerből, NEM innen).
 */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    sz2_rx_item_t item;

    s_diag_recv_cb++;   /* DIAG: az esp_now atadott egy csomagot a callbacknek */
    item.len = (uint16_t)len;
    /* Csak akkor másoljuk a payloadot, ha a hossz pontosan stimmel (különben hibás csomag). */
    if (len == (int)sizeof(sz2_payload_t)) {
        memcpy(&item.payload, data, sizeof(sz2_payload_t));
    } else {
        memset(&item.payload, 0, sizeof(sz2_payload_t));
    }
    memcpy(item.src_mac, info->src_addr, 6);
    item.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;

    /* Non-blocking: ha tele -> dropped (NEM blokkolunk a WiFi-task kontextusban). */
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_dropped_count++;
    }
}

/* Egy node-statisztika sávjának megkeresése/lefoglalása node_id alapján. */
static sz2_node_stat_t *node_stat_get(uint8_t node_id, const uint8_t *mac)
{
    for (int i = 0; i < SZ2_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].node_id == node_id) {
            return &s_nodes[i];
        }
    }
    /* Új sáv. */
    for (int i = 0; i < SZ2_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) {
            sz2_node_stat_t *n = &s_nodes[i];
            memset(n, 0, sizeof(*n));
            n->in_use   = true;
            n->node_id  = node_id;
            memcpy(n->mac, mac, 6);
            n->rssi_min = 127;
            n->rssi_max = -128;
            n->first_rx_us = -1;
            return n;
        }
    }
    return NULL;   /* nincs szabad sáv (SZ2_MAX_NODES kicsi) */
}

/* A node végső összesítőjének kiírása — ez a gép-grep-elhető PASS/FAIL sor. */
static void print_report(sz2_node_stat_t *n)
{
    uint32_t expect = n->total_packets ? n->total_packets : SZ2_EXPECT_TOTAL;
    if (expect > SZ2_EXPECT_TOTAL) {
        expect = SZ2_EXPECT_TOTAL;   /* a bitmap felső korlátja */
    }

    /* Hiányzó seq-ek a [0..expect-1] tartományból a bitmap alapján. */
    uint32_t missing = 0;
    char miss_list[256];
    int   miss_pos = 0;
    miss_list[0] = '\0';
    for (uint32_t s = 0; s < expect; s++) {
        bool seen = (n->seen[s >> 3] >> (s & 7)) & 1;
        if (!seen) {
            missing++;
            if (missing <= SZ2_MISSING_LIST_MAX && miss_pos < (int)sizeof(miss_list) - 12) {
                miss_pos += snprintf(miss_list + miss_pos, sizeof(miss_list) - miss_pos,
                                     "%s%u", (miss_pos ? "," : ""), (unsigned)s);
            }
        }
    }

    int64_t elapsed_ms = (n->last_rx_us > 0 && n->first_rx_us > 0)
                         ? (n->last_rx_us - n->first_rx_us) / 1000 : 0;
    int     rssi_avg   = n->rssi_n ? (int)(n->rssi_sum / (int64_t)n->rssi_n) : 0;
    bool    pass       = (n->received_unique == expect) && (missing == 0) &&
                         (s_dropped_count == 0) && (n->crc_error == 0);

    ESP_LOGI(TAG, "=== SZ2 RIPORT ===");
    ESP_LOGI(TAG, "node_id=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X boot_id=0x%08X",
             n->node_id, n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5],
             (unsigned)n->boot_id);
    ESP_LOGI(TAG, "expected=%u received=%u missing=%u duplicate=%u out_of_order=%u",
             (unsigned)expect, (unsigned)n->received_unique, (unsigned)missing,
             (unsigned)n->duplicate_seq, (unsigned)n->out_of_order);
    ESP_LOGI(TAG, "dropped_queue=%u crc_error=%u",
             (unsigned)s_dropped_count, (unsigned)n->crc_error);
    if (missing > 0) {
        if (missing <= SZ2_MISSING_LIST_MAX) {
            ESP_LOGI(TAG, "missing_seq=[%s]", miss_list);
        } else {
            ESP_LOGI(TAG, "missing_seq=[%s,...] (osszesen %u, csak az elso %d listazva)",
                     miss_list, (unsigned)missing, SZ2_MISSING_LIST_MAX);
        }
    }
    ESP_LOGI(TAG, "elapsed_ms=%lld pkt_per_s=%lld rssi_min=%d rssi_avg=%d rssi_max=%d",
             (long long)elapsed_ms,
             (long long)(elapsed_ms > 0 ? (int64_t)n->received_unique * 1000 / elapsed_ms : 0),
             (int)n->rssi_min, rssi_avg, (int)n->rssi_max);
    /* Diagnosztikai bontas (a nema eldobasi utak lathatova tetele): a recv_cb hanyszor futott
     * (= az esp_now ennyi csomagot adott at), es a feldolgozasban hol estek ki. Egy egeszseges
     * one-shot futasban: recv_cb == ok == expected, a tobbi 0. (A szamlalok GLOBALISAK, friss
     * master-boot-tol szamolnak; ezert a tesztet friss masterrel kell inditani.) */
    ESP_LOGI(TAG, "DIAG: recv_cb=%u ok=%u len_bad=%u ver_bad=%u crc_bad=%u",
             (unsigned)s_diag_recv_cb, (unsigned)s_diag_ok, (unsigned)s_diag_len_bad,
             (unsigned)s_diag_ver_bad, (unsigned)s_diag_crc_bad);
    ESP_LOGI(TAG, "PASS_FAIL=%s", pass ? "PASS" : "FAIL");
    ESP_LOGI(TAG, "=== SZ2 RIPORT VEGE ===");
}

/*
 * CONSUMER-TASK (core 1-re pinnelve): a vételi queue feldolgozása. Itt megy a NEHÉZ munka,
 * NEM a callbackben: CRC-ellenőrzés, node_id<->MAC tanulás, boot_id-reset detektálás,
 * seq-statisztika (bitmap), riport. Idle-timeout (SZ2_IDLE_REPORT_MS) vagy a teljes
 * mennyiség elérésekor kiírja a végső összesítőt.
 */
static void consumer_task(void *arg)
{
    (void)arg;
    sz2_rx_item_t item;
    uint32_t progress = 0;

    while (1) {
        if (xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(SZ2_IDLE_REPORT_MS)) != pdTRUE) {
            /* Idle: ha van olyan node, ami kapott csomagot és még nem riportoltunk -> riport. */
            for (int i = 0; i < SZ2_MAX_NODES; i++) {
                sz2_node_stat_t *n = &s_nodes[i];
                if (n->in_use && !n->reported && n->received_unique > 0) {
                    ESP_LOGI(TAG, "(idle %d ms — nincs uj csomag, vegso osszesito)", SZ2_IDLE_REPORT_MS);
                    print_report(n);
                    n->reported = true;
                }
            }
            continue;
        }

        /* Rossz hossz -> hibás csomag, eldobjuk (nem számítjuk node-statba). */
        if (item.len != (uint16_t)sizeof(sz2_payload_t)) {
            s_diag_len_bad++;
            ESP_LOGW(TAG, "rossz hosszusagu csomag: %u byte (vart %u)",
                     (unsigned)item.len, (unsigned)sizeof(sz2_payload_t));
            continue;
        }

        sz2_payload_t *p = &item.payload;

        /* Verzió-szűrés. */
        if (p->protocol_version != SZ2_PROTO_VERSION) {
            s_diag_ver_bad++;
            ESP_LOGW(TAG, "ismeretlen protocol_version=%d (vart %d) — eldobva",
                     p->protocol_version, SZ2_PROTO_VERSION);
            continue;
        }

        /* CRC-ellenőrzés a node_id elérése ELŐTT — de a node-statba csak a node ismeretében
         * tudjuk a crc_error-t számolni, ezért a stat-sávot előbb lekérjük. */
        sz2_node_stat_t *n = node_stat_get(p->node_id, item.src_mac);
        if (n == NULL) {
            ESP_LOGW(TAG, "nincs szabad node-sav (SZ2_MAX_NODES=%d) — csomag eldobva", SZ2_MAX_NODES);
            continue;
        }

        uint16_t want_crc = sz2_payload_crc(p);
        if (want_crc != p->crc) {
            n->crc_error++;
            s_diag_crc_bad++;
            /* DIAG: az elso par CRC-hibanal irjuk ki a szamolt vs vett CRC-t + a payload elejet. */
            if (s_diag_crc_bad <= 3) {
                ESP_LOGW(TAG, "DIAG CRC-HIBA: szamolt=0x%04X vett=0x%04X | node_id=%u seq=%u len=%u",
                         want_crc, p->crc, p->node_id, (unsigned)p->seq, (unsigned)item.len);
            }
            continue;   /* rossz CRC -> NEM számít az adat-statisztikába */
        }
        s_diag_ok++;

        /* Provisioning: MAC-tanulás / MAC-váltás log. */
        if (memcmp(n->mac, item.src_mac, 6) != 0) {
            ESP_LOGI(TAG, "node_id=%d MAC-valtas: %02X:%02X:%02X:%02X:%02X:%02X",
                     n->node_id, item.src_mac[0], item.src_mac[1], item.src_mac[2],
                     item.src_mac[3], item.src_mac[4], item.src_mac[5]);
            memcpy(n->mac, item.src_mac, 6);
        }

        /* boot_id-váltás -> node-reset: seq-tracking RESET az adott node-ra. */
        if (n->received_unique == 0 && n->boot_id == 0) {
            /* első valid csomag: rögzítjük a boot_id-t és logoljuk a tanulást. */
            n->boot_id = p->boot_id;
            ESP_LOGI(TAG, "node_id=%d tanulva: MAC=%02X:%02X:%02X:%02X:%02X:%02X boot_id=0x%08X total=%u",
                     n->node_id, item.src_mac[0], item.src_mac[1], item.src_mac[2],
                     item.src_mac[3], item.src_mac[4], item.src_mac[5],
                     (unsigned)p->boot_id, (unsigned)p->total_packets);
        } else if (p->boot_id != n->boot_id) {
            ESP_LOGI(TAG, "node_id=%d RESET (uj boot_id 0x%08X) — seq-tracking ujraindul",
                     n->node_id, (unsigned)p->boot_id);
            uint8_t  saved_id = n->node_id;
            uint8_t  saved_mac[6];
            memcpy(saved_mac, n->mac, 6);
            memset(n, 0, sizeof(*n));
            n->in_use   = true;
            n->node_id  = saved_id;
            memcpy(n->mac, saved_mac, 6);
            n->rssi_min = 127;
            n->rssi_max = -128;
            n->first_rx_us = -1;
            n->boot_id  = p->boot_id;
        }

        n->total_packets = p->total_packets;

        /* Időbélyegek + RSSI. */
        int64_t now = esp_timer_get_time();
        if (n->first_rx_us < 0) {
            n->first_rx_us = now;
        }
        n->last_rx_us = now;
        if (item.rssi < n->rssi_min) n->rssi_min = item.rssi;
        if (item.rssi > n->rssi_max) n->rssi_max = item.rssi;
        n->rssi_sum += item.rssi;
        n->rssi_n++;

        /* out-of-order: az aktuális seq < eddigi max_seq. */
        if (n->received_unique > 0 && p->seq < n->max_seq) {
            n->out_of_order++;
        }
        if (p->seq > n->max_seq) {
            n->max_seq = p->seq;
        }

        /* Seq bitmap: unique vs. duplicate. (A bitmap a [0..SZ2_EXPECT_TOTAL-1] tartományt fedi.) */
        if (p->seq < SZ2_EXPECT_TOTAL) {
            uint8_t  *byte = &n->seen[p->seq >> 3];
            uint8_t   mask = (uint8_t)(1u << (p->seq & 7));
            if (*byte & mask) {
                n->duplicate_seq++;
            } else {
                *byte |= mask;
                n->received_unique++;
            }
        } else {
            /* seq a bitmap-tartományon kívül (total_packets > SZ2_EXPECT_TOTAL) — csak számoljuk. */
            n->received_unique++;
        }
        n->reported = false;   /* új csomag jött -> a riportot újra ki kell írni a végén */

        if (SZ2_PROGRESS_EVERY > 0 && (++progress % SZ2_PROGRESS_EVERY) == 0) {
            ESP_LOGI(TAG, "RX haladas: node=%d received=%u/%u (dup=%u drop=%u crc_err=%u rssi=%d)",
                     n->node_id, (unsigned)n->received_unique,
                     (unsigned)(n->total_packets ? n->total_packets : SZ2_EXPECT_TOTAL),
                     (unsigned)n->duplicate_seq, (unsigned)s_dropped_count,
                     (unsigned)n->crc_error, (int)item.rssi);
        }

        /* Teljes mennyiség elérve -> azonnali végső riport (nem várjuk meg az idle-t). */
        uint32_t expect = n->total_packets ? n->total_packets : SZ2_EXPECT_TOTAL;
        if (expect > SZ2_EXPECT_TOTAL) expect = SZ2_EXPECT_TOTAL;
        if (!n->reported && n->received_unique >= expect) {
            print_report(n);
            n->reported = true;
        }
    }
}

/*
 * WiFi + ESP-NOW init a kötelező sorrendben (azonos a node-éval, + PS_NONE a masteren):
 *   netif -> event loop -> wifi_init -> storage RAM -> mode STA -> wifi_start -> PS_NONE ->
 *   set_channel (START UTÁN!) -> esp_now_init -> register recv-cb -> add broadcast peer.
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
    /* Interop-hardening: explicit 11b/g/n protokoll, a LR (long-range) bit KIZÁRVA — az LR
     * moduláció nem interoperábilis a sima 11b/g/n-nel -> teljes vétel-némasághoz vezetne
     * C3 <-> classic ESP32 között. EGYEZZEN a node beállításával. */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ2_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    /* Broadcast peer a szimmetriáért / jövőbeli unicasthoz (vételhez peer nélkül is menne). */
    esp_now_peer_info_t peer = { 0 };
    static const uint8_t bc[ESP_NOW_ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    memcpy(peer.peer_addr, bc, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* Fix, legrobusztusabb ESP-NOW ráta (11b, 1 Mbps long-preamble) — EGYEZZEN a node-éval,
     * hogy a két chip azonos PHY-módon/rátán kommunikáljon. esp_now_add_peer UTÁN. */
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate    = WIFI_PHY_RATE_1M_L,
        .ersu    = false,
        .dcm     = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rc));

    /* A TENYLEGES veteli csatorna visszaolvasasa (egyszeri, boot-kor) — csatorna-mismatch kizarasa. */
    uint8_t ch_prim = 0; wifi_second_chan_t ch_sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch_prim, &ch_sec);
    ESP_LOGI(TAG, "tenyleges RX csatorna prim=%u (vart SZ2_CHANNEL=%d)", ch_prim, SZ2_CHANNEL);
}

#ifdef SZ2_SNIFFER
/* =================== DIAGNOSZTIKAI PROMISCUOUS SNIFFER ===================
 * Build: PLATFORMIO_BUILD_FLAGS=-DSZ2_SNIFFER pio run -t upload
 * Cel: eldonteni, hogy a node ESP-NOW frame-jei FIZIKAILAG megerkeznek-e a master
 * antennajara az 1-es csatornan. Ha igen -> a baj az ESP-NOW RX-retegben/peer-kezelesben;
 * ha nem -> radio/csatorna a node oldalon. Minden vett 802.11 frame src MAC-jat (addr2) +
 * RSSI-t szamoljuk; az ESPNOW vendor action frame (type=MGMT, cat=127, OUI 18:fe:34) kulon. */
static volatile uint32_t s_sniff_total = 0;
static volatile uint32_t s_sniff_action = 0;
static volatile uint32_t s_sniff_espnow = 0;
static uint8_t s_last_src[6];
static int8_t  s_last_rssi;

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *d = p->payload;
    s_sniff_total++;
    uint8_t ftype = (d[0] >> 2) & 0x3;   /* 0=mgmt */
    uint8_t fsub  = (d[0] >> 4) & 0xF;   /* 0xD=action */
    memcpy(s_last_src, d + 10, 6);       /* addr2 = transmitter */
    s_last_rssi = p->rx_ctrl.rssi;
    if (ftype == 0 && fsub == 0xD) {
        s_sniff_action++;
        /* action frame body: addr-ek utan (24 byte MAC hdr) cat(1). cat==127 (0x7F)=vendor. */
        if (p->rx_ctrl.sig_len > 24 && d[24] == 0x7F) {
            /* OUI a kovetkezo 3 byte: Espressif = 18:fe:34 */
            if (p->rx_ctrl.sig_len > 27 && d[25] == 0x18 && d[26] == 0xfe && d[27] == 0x34) {
                s_sniff_espnow++;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGW(TAG, "=== Sz2 MASTER — DIAG PROMISCUOUS SNIFFER (csatorna=%d) ===", SZ2_CHANNEL);
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    } else { ESP_ERROR_CHECK(e); }
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(SZ2_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffer_cb));
    uint8_t cp = 0; wifi_second_chan_t cs = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&cp, &cs);
    ESP_LOGW(TAG, "SNIFFER aktiv, csatorna prim=%u. Szamlalok 1Hz-en...", cp);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGW(TAG, "SNIFF total=%u action=%u ESPNOW=%u | utolso src=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d",
                 (unsigned)s_sniff_total, (unsigned)s_sniff_action, (unsigned)s_sniff_espnow,
                 s_last_src[0], s_last_src[1], s_last_src[2], s_last_src[3], s_last_src[4], s_last_src[5],
                 (int)s_last_rssi);
    }
}
#else
void app_main(void)
{
    ESP_LOGI(TAG, "=== Sz2 MASTER (RX) — ESP-NOW low-level recv ===");
    ESP_LOGI(TAG, "csatorna=%d, queue_len=%d, vart total=%d", SZ2_CHANNEL, SZ2_RX_QUEUE_LEN, SZ2_EXPECT_TOTAL);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    memset(s_nodes, 0, sizeof(s_nodes));

    s_rx_queue = xQueueCreate(SZ2_RX_QUEUE_LEN, sizeof(sz2_rx_item_t));
    configASSERT(s_rx_queue != NULL);

    wifi_espnow_init();

    /* Consumer a 2. magon (core 1): a WiFi/recv-cb (core 0) ne versenyezzen a feldolgozással. */
    xTaskCreatePinnedToCore(consumer_task, "sz2_rx", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Master kesz, varja a csomagokat...");
}
#endif /* SZ2_SNIFFER */

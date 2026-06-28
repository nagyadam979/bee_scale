/*
 * M1 — Drift-mérő (guard-window) · NODE (deep-sleep beacon, TX)
 * bee_scale / wireless_scaling · 2. fázis · experiments/M1_drift_guard_window/node
 *
 * CÉL / MIT MÉRÜNK
 *   Az ESP32-C3 belső RC slow-clockjának deep-sleep DRIFT-je. A node M1_WAKE_PERIOD_SEC
 *   másodpercenként ébred a timer-wakeupból, küld EGY ESP-NOW broadcast beacont (RTC-ben
 *   megőrzött cycle# + boot_id + a kért alvásidő + a node ébren-ideje + a C3 belső die-hőmérséklet),
 *   majd visszaalszik. A master a SAJÁT órájával időbélyegez minden vételkor -> a master log két
 *   beacon közti VALÓS ideje vs. a kért M1_WAKE_PERIOD_SEC = a slow-clock drift.
 *   TŰZZ-ÉS-FELEJTS broadcast: NINCS ACK, NINCS slot, NINCS rendezvous (az Sz3 hatásköre).
 *
 * RTC-MEGŐRZÉS (a deep sleep teljes reboot a C3-on, de az RTC_DATA_ATTR memória túléli a wakeöt)
 *   s_rtc_cycle  : abszolút ébredés-számláló (0-ról hideg indulásnál, ++ minden timer-wakeön).
 *   s_rtc_boot_id: bootonkénti (= hideg-indulásonkénti) azonosító -> a master ebből látja a resetet.
 *   s_rtc_magic  : érvényesség-jel; csak ha == M1_RTC_MAGIC, akkor folytatjuk a számlálót.
 *
 * WIRE-FORMÁTUM / BEFAGYASZTOTT KONTRAKTUS (a Python-analizátor UGYANEZRE kódol — SZÓ SZERINT):
 *   payload = sz2_payload_t (változatlan), M1-szemantika:
 *     protocol_version = SZ2_PROTO_VERSION (1)
 *     packet_type      = SZ2_PKT_DATA (1)
 *     node_id          = M1_NODE_ID
 *     node_mac[6]      = saját STA MAC
 *     boot_id          = s_rtc_boot_id (hideg-indulásnál esp_random, timer-wake közt megőrzött)
 *     seq              = s_rtc_cycle  (cycle#, abszolút RTC-számláló)
 *     packet_idx       = s_rtc_cycle  (ugyanaz)
 *     total_packets    = M1_WAKE_PERIOD_SEC * 1000  (a KÉRT alvásidő ms-ban; reqT_ms)
 *     send_timestamp_ms= awake_ms (esp_timer ms az ELSŐ send előtt)
 *     weight           = 0.0f (nem használt)
 *     temp             = C3 belső die-hőmérséklet °C (vagy NAN)
 *     vbat             = 0 (nem használt)
 *     crc              = sz2_payload_crc(&p)
 *
 * PHY-FIX (KRITIKUS — enélkül NÉMA a link C3 <-> classic ESP32 közt; az Sz2-vel SZÓ SZERINT egyező)
 *   set_protocol 11b/g/n (LR KIZÁRVA) az esp_wifi_start() UTÁN, és esp_now_set_peer_rate_config
 *   {11b, 1M_L} az add_peer UTÁN. Csatorna = SZ2_CHANNEL = 1, mindkét oldalon azonos.
 *
 * BOARD: ESP32-C3 SuperMini. Konzol: natív USB-Serial-JTAG. A node logja a fejlesztőgépen
 *   háttérből NEM olvasható (JTAG-CDC); az igazi adatforrás a MASTER log. A node csak
 *   ébredésenként egy rövid INFO-sort ír a saját USB-konzoljára.
 *
 * HANGOLHATÓ PARAMÉTEREK — a fájl tetején, kommentálva (CLAUDE.md elv).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>          /* NAN */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_idf_version.h"
#include "esp_rom_sys.h"                 /* esp_rom_delay_us() */
#include "driver/temperature_sensor.h"   /* C3 belső die-hőmérséklet (esp_driver_tsens) */
#include "soc/soc_caps.h"                /* SOC_USB_SERIAL_JTAG_SUPPORTED */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "esp_private/sleep_console.h"   /* sleep_console_usj_pad_backup_and_disable() */
#endif

#include "sz2_protocol.h"   /* a KÖZÖS wire-header (../../common), VÁLTOZATLAN */

/* ===================== HANGOLHATÓ PARAMÉTEREK ===================== */

/* A kért alvásidő másodpercben. Ebből reqT_ms = M1_WAKE_PERIOD_SEC * 1000 (-> total_packets). */
#define M1_WAKE_PERIOD_SEC      30

/* A node azonosítója (docs: unit_id). A bench-en 1; több node esetén egyedi. */
#define M1_NODE_ID              1

/* WiFi/ESP-NOW csatorna. EGYEZZEN a master SZ2_CHANNEL-jével! (1..13) */
#define SZ2_CHANNEL             1

/* Ugyanazt a beacont ennyiszer küldi el back-to-back EGY ébredésben — olcsó redundancia a
 * felügyelet nélküli (éjszakai) futáshoz. A master/analizátor (boot_id,cycle)-re dedupol. */
#define M1_TX_REPEAT            3

/* A send-cb szemafor timeoutja (ms): ennyit vár egy beacon MAC-szintű lezárására. */
#define M1_SEND_TIMEOUT_MS      50

/* ESP_ERR_ESPNOW_NO_MEM esetén ennyit várunk, majd UGYANARRA a beaconra újrapróbálunk (ms). */
#define M1_NO_MEM_RETRY_MS      2

/* ================================================================= */

static const char *TAG = "M1-NODE";

/* RTC-memória érvényesség-jele: csak ha ezt találjuk wakeup után, folytatjuk a számlálót. */
#define M1_RTC_MAGIC            0xBEE5CA1Eu

/* Broadcast cím — minden ESP-NOW vevő veszi (a master peer nélkül is). */
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

/* TX szerializáció: a send-cb ezt adja vissza, a TX-folyam erre vár (egy in-flight küldés). */
static SemaphoreHandle_t s_tx_done = NULL;

/* RTC-megőrzött állapot (timer-wakeöt túléli; hideg indulásnál szemét -> a magic dönt). */
RTC_DATA_ATTR static uint32_t s_rtc_magic;
RTC_DATA_ATTR static uint32_t s_rtc_cycle;
RTC_DATA_ATTR static uint32_t s_rtc_boot_id;

/*
 * SEND-CALLBACK. WiFi-task kontextusban fut (NEM ISR) -> sima xSemaphoreGive, NEM *FromISR.
 * Egyetlen feladata a TX-szerializációs szemafor felszabadítása. A status broadcastnél
 * gyakorlatilag mindig SUCCESS (MAC-szintű, nem app-szintű kézbesítés).
 * VERZIÓ-GUARD: az IDF v5.4-ben a szignatúra esp_now_send_info_t-re változott.
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

/*
 * WiFi + ESP-NOW init a kötelező sorrendben + a PHY-FIX (az Sz2 node-éval SZÓ SZERINT egyező):
 *   netif -> event loop -> wifi_init -> storage RAM -> mode STA -> wifi_start ->
 *   set_protocol(11b/g/n, LR KIZÁRVA) -> set_channel (START UTÁN!) -> esp_now_init ->
 *   register send-cb -> add broadcast peer -> set_peer_rate_config(11b, 1M_L).
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
    /* Interop-hardening: explicit 11b/g/n, a LR (long-range) bit KIZÁRVA. Az Espressif-proprietary
     * LR-moduláció NEM interoperábilis a sima 11b/g/n-nel -> ha az egyik chipen aktív lenne, a másik
     * számára a frame fizikailag dekódolhatatlan = teljes vétel-némaság (C3 <-> classic ESP32). */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
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

    /* Fix, legrobusztusabb ESP-NOW ráta (11b, 1 Mbps long-preamble) -> a C3 és a classic ESP32
     * garantáltan AZONOS rátán/modulációban beszél (PHY-mismatch ellen). add_peer UTÁN. */
    esp_now_rate_config_t rc = {
        .phymode = WIFI_PHY_MODE_11B,
        .rate    = WIFI_PHY_RATE_1M_L,
        .ersu    = false,
        .dcm     = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(BROADCAST_MAC, &rc));
}

/*
 * A C3 belső die-hőmérséklet best-effort olvasása. BÁRMELYIK hívás hibája -> NAN (a beacont NEM
 * állítja le; a temp mező csak diagnosztika a drift-hez). Install/enable/get/disable/uninstall.
 */
static float read_die_temp_c(void)
{
    temperature_sensor_handle_t h = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    float die_c = NAN;

    if (temperature_sensor_install(&cfg, &h) != ESP_OK) {
        return NAN;
    }
    if (temperature_sensor_enable(h) == ESP_OK) {
        if (temperature_sensor_get_celsius(h, &die_c) != ESP_OK) {
            die_c = NAN;
        }
        temperature_sensor_disable(h);
    }
    temperature_sensor_uninstall(h);
    return die_c;
}

void app_main(void)
{
    /* NVS kell a WiFi-hez (kalibrációs/PHY adatok). */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    /* 1) RTC cycle/boot_id kezelés: timer-wakeup + érvényes magic -> folytatás; különben hideg start. */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER && s_rtc_magic == M1_RTC_MAGIC) {
        s_rtc_cycle++;
    } else {
        s_rtc_cycle   = 0;
        s_rtc_boot_id = esp_random();
        s_rtc_magic   = M1_RTC_MAGIC;
    }

    /* 2) C3 belső die-hőmérséklet (best-effort; hiba -> NAN). */
    float die_c = read_die_temp_c();

    ESP_LOGI(TAG, "=== M1 NODE beacon === cycle=%u boot=0x%08X die_c=%.2f reqT_ms=%u (wakeup_cause=%d)",
             (unsigned)s_rtc_cycle, (unsigned)s_rtc_boot_id, die_c,
             (unsigned)(M1_WAKE_PERIOD_SEC * 1000u), (int)cause);

    /* 3) TX-szerializációs szemafor: bináris, kezdetben ÜRES (a send-cb adja meg). */
    s_tx_done = xSemaphoreCreateBinary();
    configASSERT(s_tx_done != NULL);

    /* 4) WiFi + ESP-NOW init a PHY-fixszel. */
    wifi_espnow_init();

    /* A node saját STA MAC-je (a payload node_mac mezőjébe). */
    uint8_t my_mac[6] = { 0 };
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, my_mac));

    /* 5) Payload feltöltése (BEFAGYASZTOTT KONTRAKTUS). */
    sz2_payload_t p;
    memset(&p, 0, sizeof(p));
    p.protocol_version  = SZ2_PROTO_VERSION;
    p.packet_type       = SZ2_PKT_DATA;
    p.node_id           = M1_NODE_ID;
    memcpy(p.node_mac, my_mac, 6);
    p.boot_id           = s_rtc_boot_id;
    p.seq               = s_rtc_cycle;
    p.packet_idx        = s_rtc_cycle;                       /* M1: == cycle */
    p.total_packets     = (uint32_t)M1_WAKE_PERIOD_SEC * 1000u;  /* reqT_ms */
    p.weight            = 0.0f;                              /* nem használt */
    p.temp              = die_c;                             /* C3 die-hőmérséklet (vagy NAN) */
    p.vbat              = 0;                                 /* nem használt */
    /* send_timestamp_ms az ELSŐ send előtt (lásd lent), majd CRC. */

    /* 6) awake_ms az ELSŐ send előtt; CRC; M1_TX_REPEAT-szer back-to-back küldés. */
    uint32_t awake_ms = (uint32_t)(esp_timer_get_time() / 1000);
    p.send_timestamp_ms = awake_ms;
    p.crc = sz2_payload_crc(&p);

    for (int rep = 0; rep < M1_TX_REPEAT; rep++) {
        for (;;) {
            esp_err_t err = esp_now_send(BROADCAST_MAC, (const uint8_t *)&p, sizeof(p));
            if (err == ESP_OK) {
                /* Egy in-flight küldés: a send-cb-re blokkolunk (timeout esetén tovább lépünk). */
                xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(M1_SEND_TIMEOUT_MS));
                break;
            } else if (err == ESP_ERR_ESPNOW_NO_MEM) {
                /* Belső TX-FIFO tele -> várunk, majd UGYANEZT a beacont újraküldjük. */
                vTaskDelay(pdMS_TO_TICKS(M1_NO_MEM_RETRY_MS));
            } else {
                ESP_LOGW(TAG, "esp_now_send hiba (rep=%d): %s", rep, esp_err_to_name(err));
                break;
            }
        }
    }

    ESP_LOGI(TAG, "beacon elkuldve %dx (cycle=%u, awake_ms=%u). Alvas: %d s.",
             M1_TX_REPEAT, (unsigned)s_rtc_cycle, (unsigned)awake_ms, M1_WAKE_PERIOD_SEC);

    /* 7) Log kiürítése + rövid delay, MIELŐTT a USB-JTAG pad lekapcsol (különben az utolsó sorok elvesznek). */
    fflush(stdout);
    esp_rom_delay_us(20000);   /* ~20 ms a soros kiírás befejezésére */

    /*
     * USB-Serial-JTAG pad LEKAPCSOLÁSA deep sleephez (KRITIKUS a fogyasztásra a C3-on): a USJ pad
     * deep sleepben is bekapcsolva marad (~500-800 µA), az IDF ezt csak light sleepnél kapcsolja
     * le automatikusan. (Reset-ből ébredünk deep sleep után, ezért pad-restore nem szükséges.)
     */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    sleep_console_usj_pad_backup_and_disable();
#endif

    /* 8) Timer-wakeup + deep sleep. Wakeup után az app_main() újraindul (a C3-on deep sleep = reboot),
     *    az s_rtc_* a fenti RTC_DATA_ATTR memóriában megmarad. */
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)M1_WAKE_PERIOD_SEC * 1000000ULL));
    esp_deep_sleep_start();
}

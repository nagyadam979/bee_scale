/* Sz2b RSSI-PROBE — antenna-diagnosztika: 1 board <-> 1 referencia-AP.
 *
 * MIÉRT: az Sz2b P3 soakban az S3-Zero masternél ~30-40 dB link-budget deficit gyanúja merült
 * fel (RSSI −61..−86 @ ~1 m). A node<->master mérés KÉT ismeretlen antennát tartalmaz; ez a
 * probe kizárja a másik ESP32-t: minden boardot ugyanahhoz a routerhez (dlink-EDA4) mérünk,
 * így a boardok RSSI-távolság görbéi közvetlenül összevethetők.
 *
 * MIT CSINÁL:
 *   - WiFi STA-ként csatlakozik (SZ2B_WIFI_SSID/PASS — a master creds.ini-jéből, build-flag).
 *   - Másodpercenként mintázza az AP RSSI-jét (esp_wifi_sta_get_ap_info) és egy greppelhető
 *     "PROBE ..." sort küld HÁROM csatornára:
 *       1. soros konzol (USB-n tesztelésnél),
 *       2. telnet-kliensek (TCP :23 — tápról futva ez a fő csatorna),
 *       3. UDP-broadcast (:5005 — IP-felderítés serial nélkül: `nc -ukl 5005`).
 *   - Csúszóablakos átlag/min/max (10 minta), hogy a pillanatnyi fading kisimuljon.
 *   - Reconnect: DISCONNECTED eseményre azonnali újrapróbálkozás (a probe-ot pont a
 *     jel-határig visszük — gyors visszacsatlakozás kell, backoff nem).
 *
 * MÉRÉSI IRÁNY: az RSSI a LEFELÉ irány (AP -> board, a board vevőjén mért teljesítmény).
 * A TX-power flag (C3: 8,5 dBm) csak a felfelé irányt (telnet/DHCP) érinti, a mérést nem.
 *
 * Sor-formátum (CRLF, telnet-barát):
 *   PROBE name=<env> seq=<n> up=<s> wifi=<DOWN|ASSOC|IP> ip=<a.b.c.d> ch=<n> rssi=<dBm>
 *         avg10=<dBm.tized> min10=<dBm> max10=<dBm> disc=<n>
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

#ifndef PROBE_NAME
#error "PROBE_NAME hianyzik (platformio.ini env build_flags)"
#endif
#if !defined(SZ2B_WIFI_SSID) || !defined(SZ2B_WIFI_PASS)
#error "WiFi hitelesitok hianyoznak - a master/creds.ini-t az extra_configs huzza be"
#endif

#define PROBE_TELNET_PORT   23
#define PROBE_UDP_PORT      5005
#define PROBE_PERIOD_MS     1000
#define PROBE_MAX_CLIENTS   4
#define PROBE_WIN           10   /* csúszóablak (minta) az avg/min/max-hoz */

static const char *TAG = "rssi_probe";

/* WiFi-állapot — az eseménykezelő írja, a mérőhurok olvassa. */
static volatile bool s_got_ip;
static volatile uint32_t s_disconnects;
static char s_ip_str[16] = "-";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
        ESP_LOGI(TAG, "PROBE WIFI CONNECTED ssid=%s ch=%u bssid=" MACSTR,
                 (const char *)e->ssid, e->channel, MAC2STR(e->bssid));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        s_got_ip = false;
        s_disconnects++;
        ESP_LOGW(TAG, "PROBE WIFI DISCONNECTED reason=%u disc=%" PRIu32,
                 e->reason, s_disconnects);
        /* Azonnali retry: a probe-ot szándékosan visszük a jel-határig, gyors
         * visszatérés kell. (A connect-próba maga is ~1-2 s, nem pörög üresben.) */
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        ESP_LOGI(TAG, "PROBE WIFI IP ip=%s gw=" IPSTR "  ->  telnet %s %d",
                 s_ip_str, IP2STR(&e->ip_info.gw), s_ip_str, PROBE_TELNET_PORT);
    }
}

/* Nem-blokkolóra állítás — accept utáni kliens-socketekre és a listenerre is. */
static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void app_main(void)
{
    /* --- NVS (a WiFi-nek kell) --- */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* --- WiFi STA (a master p2/p3 mintája, ESP-NOW nélkül) --- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, SZ2B_WIFI_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, SZ2B_WIFI_PASS, sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* PS_NONE: a beacon-vétel folyamatos -> az RSSI-minta mindig friss (tápfogyasztás
     * itt irreleváns, hálózati tápról fut). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#ifdef PROBE_TX_POWER
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(PROBE_TX_POWER));
#endif

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    int8_t txp = 0;
    esp_wifi_get_max_tx_power(&txp);
    ESP_LOGI(TAG, "PROBE BOOT name=%s mac=" MACSTR " txpower=%d(0.25dBm) ssid=%s",
             PROBE_NAME, MAC2STR(mac), txp, SZ2B_WIFI_SSID);

    /* --- Telnet-listener (:23) + UDP-broadcast socket (:5005) --- */
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in la = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(PROBE_TELNET_PORT),
    };
    bind(listen_fd, (struct sockaddr *)&la, sizeof(la));
    listen(listen_fd, 2);
    set_nonblock(listen_fd);

    int udp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in ba = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
        .sin_port = htons(PROBE_UDP_PORT),
    };

    int clients[PROBE_MAX_CLIENTS];
    for (int i = 0; i < PROBE_MAX_CLIENTS; i++) clients[i] = -1;

    /* --- Mérőhurok: 1 s ütem --- */
    int8_t win[PROBE_WIN];
    int win_n = 0, win_i = 0;
    bool was_assoc = false;
    uint32_t seq = 0;
    char line[192];
    char rxbuf[64];
    TickType_t next = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(PROBE_PERIOD_MS));
        seq++;

        /* Új telnet-kliens felvétele (nem-blokkoló accept). */
        struct sockaddr_in ca;
        socklen_t cal = sizeof(ca);
        int cfd = accept(listen_fd, (struct sockaddr *)&ca, &cal);
        if (cfd >= 0) {
            int slot = -1;
            for (int i = 0; i < PROBE_MAX_CLIENTS; i++) if (clients[i] < 0) { slot = i; break; }
            if (slot < 0) {
                close(cfd);
            } else {
                set_nonblock(cfd);
                clients[slot] = cfd;
                const char *banner = "RSSI probe " PROBE_NAME
                                     " - 1 sor/mp. Kilepes: Ctrl-] majd q (telnet) / Ctrl-C (nc).\r\n";
                send(cfd, banner, strlen(banner), 0);
                ESP_LOGI(TAG, "PROBE CLIENT connected slot=%d", slot);
            }
        }
        /* Kliens-RX eldobása (telnet IAC-negociáció stb.) + halott kliens lecsukása. */
        for (int i = 0; i < PROBE_MAX_CLIENTS; i++) {
            if (clients[i] < 0) continue;
            int n = recv(clients[i], rxbuf, sizeof(rxbuf), 0);
            if (n == 0 || (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
                close(clients[i]);
                clients[i] = -1;
                ESP_LOGI(TAG, "PROBE CLIENT closed slot=%d", i);
            }
        }

        /* RSSI-minta + ablak-statisztika. */
        wifi_ap_record_t ap;
        bool assoc = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (assoc && !was_assoc) { win_n = 0; win_i = 0; }  /* új link -> ablak reset */
        was_assoc = assoc;

        uint32_t up_s = (uint32_t)(esp_log_timestamp() / 1000);
        int len;
        if (!assoc) {
            len = snprintf(line, sizeof(line),
                           "PROBE name=%s seq=%" PRIu32 " up=%" PRIu32
                           " wifi=DOWN ip=- ch=- rssi=NA disc=%" PRIu32 "\r\n",
                           PROBE_NAME, seq, up_s, s_disconnects);
        } else {
            win[win_i] = ap.rssi;
            win_i = (win_i + 1) % PROBE_WIN;
            if (win_n < PROBE_WIN) win_n++;
            int sum = 0, mn = 127, mx = -128;
            for (int i = 0; i < win_n; i++) {
                sum += win[i];
                if (win[i] < mn) mn = win[i];
                if (win[i] > mx) mx = win[i];
            }
            int avg10 = (sum * 10) / win_n;  /* tized-dBm, float nélkül */
            len = snprintf(line, sizeof(line),
                           "PROBE name=%s seq=%" PRIu32 " up=%" PRIu32
                           " wifi=%s ip=%s ch=%u rssi=%d avg10=%d.%d min10=%d max10=%d"
                           " disc=%" PRIu32 "\r\n",
                           PROBE_NAME, seq, up_s, s_got_ip ? "IP" : "ASSOC", s_ip_str,
                           ap.primary, ap.rssi, avg10 / 10, abs(avg10 % 10), mn, mx,
                           s_disconnects);
        }

        /* 1) soros konzol  2) telnet-kliensek  3) UDP-broadcast */
        fputs(line, stdout);
        for (int i = 0; i < PROBE_MAX_CLIENTS; i++) {
            if (clients[i] < 0) continue;
            if (send(clients[i], line, len, 0) < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                close(clients[i]);
                clients[i] = -1;
            }
        }
        if (s_got_ip) {
            sendto(udp_fd, line, len, 0, (struct sockaddr *)&ba, sizeof(ba));
        }
    }
}

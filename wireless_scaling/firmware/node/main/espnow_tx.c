/*
 * espnow_tx.c — lásd espnow_tx.h.
 * STUB: az interfész él, a valós WiFi/ESP-NOW init + PHY-fix + TX hurok az M1-ből kerül be
 * a modul-feltöltéskor. Egyelőre csak logol, hogy a váz fordul és a szekvencer végigfut.
 */
#include <string.h>
#include "espnow_tx.h"
#include "esp_log.h"

static const char *TAG = "espnow_tx";

void espnow_tx_init(void)
{
    ESP_LOGW(TAG, "[stub] espnow_tx_init — TODO: WiFi STA + ESP-NOW init + PHY-fix (M1-bol)");
}

void espnow_tx_get_mac(uint8_t mac_out[6])
{
    memset(mac_out, 0, 6);   /* TODO: esp_wifi_get_mac(WIFI_IF_STA, ...) */
}

void espnow_tx_send(const sz2_payload_t *p, int repeat)
{
    (void)p;
    ESP_LOGW(TAG, "[stub] espnow_tx_send x%d — TODO: esp_now_send broadcast (M1-bol)", repeat);
}

/*
 * espnow_tx.h — WiFi + ESP-NOW init (PHY-fix) és broadcast küldés
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A node tűzz-és-felejts broadcastja (Sz2/M1 minta). A PHY-fix (11b/g/n LR nélkül +
 * esp_now_set_peer_rate_config {11b,1M_L}) KRITIKUS a C3-node <-> S3-master link működéséhez.
 *
 * STÁTUSZ: STUB — az interfész végleges, az implementáció az M1 wifi_espnow_init + TX
 * hurokból kerül be a modul-feltöltéskor.
 */
#ifndef ESPNOW_TX_H
#define ESPNOW_TX_H

#include <stdint.h>
#include "protocol.h"

/* WiFi (STA) + ESP-NOW inicializálás a PHY-fixszel, broadcast peer felvétele. NVS-t feltételez. */
void espnow_tx_init(void);

/* A node saját STA MAC-je (a payload node_mac mezőjéhez). init UTÁN hívható. */
void espnow_tx_get_mac(uint8_t mac_out[6]);

/* A payload elküldése broadcastként `repeat`-szer back-to-back (send-cb-re szerializálva). */
void espnow_tx_send(const sz2_payload_t *p, int repeat);

#endif /* ESPNOW_TX_H */

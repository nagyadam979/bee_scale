/*
 * espnow_tx.h — WiFi + ESP-NOW init (PHY-fix), broadcast és app-ACK-os átadás
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

#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

typedef struct {
    uint32_t ack_last_seq;
    uint32_t clear_buffer_until_seq;
    int32_t clock_correction_s;
    uint32_t next_slot_offset_s;
    uint8_t retry_policy;
    uint8_t master_mac[6];
    uint32_t attempts;
    char dest_mode;
} espnow_tx_ack_result_t;

/* WiFi (STA) + ESP-NOW inicializálás a PHY-fixszel, broadcast peer felvétele. NVS-t feltételez. */
void espnow_tx_init(void);

/* A node saját STA MAC-je (a payload node_mac mezőjéhez). init UTÁN hívható. */
void espnow_tx_get_mac(uint8_t mac_out[6]);

/* A payload elküldése broadcastként `repeat`-szer back-to-back (send-cb-re szerializálva). */
void espnow_tx_send(const sz2_payload_t *p, int repeat);

/* App-ACK-os átadás. true csak master ACK után; a MAC send-cb önmagában nem siker. */
bool espnow_tx_send_with_ack(const sz2_payload_t *p, espnow_tx_ack_result_t *result);

/* Cold-boot HELLO/SYNC kézfogás. true csak master ACK/SYNC után. */
bool espnow_tx_sync_hello(const sz2_hello_t *hello, espnow_tx_ack_result_t *result);

/* ESP-NOW + WiFi leállítása deep sleep előtt. Ha nem volt init, no-op. */
void espnow_tx_deinit(void);

#endif /* ESPNOW_TX_H */

#ifndef RADIO_HOST_H
#define RADIO_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"

#define ESP_NOW_ETH_ALEN 6
typedef void *SemaphoreHandle_t;
typedef struct { const uint8_t *src_addr; } esp_now_recv_info_t;

extern unsigned g_sem_gives;
static inline int xSemaphoreGive(SemaphoreHandle_t sem)
{ if (sem) ++g_sem_gives; return 1; }

typedef struct {
    bool in_use;
    uint8_t node_id;
    uint8_t mac[6];
    uint32_t boot_id;
    bool have_boot_id;
    bool have_seq;
    uint32_t last_seq;
    uint32_t rx_valid, rx_new, rx_dup, miss_cum, crc_err, ack_sent, ack_txfail, resets;
    int32_t rssi_min, rssi_max;
    int64_t rssi_sum;
    uint32_t rssi_n;
    int64_t last_rx_us;
} host_node_state_t;

void prod_node_ack_reset(void);
void prod_node_ack_wait(uint8_t node, uint32_t seq);
bool prod_node_ack_inject(const uint8_t src[6], const void *data, int len,
                          sz2_ack_t *accepted, uint8_t accepted_src[6]);
void prod_master_registry_reset(void);
host_node_state_t *prod_master_node_get(uint8_t node, const uint8_t mac[6]);

#endif

/*
 * node_buffer.h — flash-backed pending queue a node mérésekhez.
 *
 * Minden mérés rádiózás előtt NVS-be kerül a `nodebuf` partíción. A queue oldest-first
 * olvasható, és csak master app-ACK után törlődik clear_buffer_until_seq alapján.
 */
#ifndef NODE_BUFFER_H
#define NODE_BUFFER_H

#include <stdint.h>
#include "esp_err.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t node_buffer_init(void);
uint32_t node_buffer_next_seq(void);
uint32_t node_buffer_count(void);

esp_err_t node_buffer_enqueue(const sz2_payload_t *payload);
esp_err_t node_buffer_peek_oldest(sz2_payload_t *payload_out);
esp_err_t node_buffer_clear_until(uint32_t seq);

#ifdef __cplusplus
}
#endif

#endif /* NODE_BUFFER_H */

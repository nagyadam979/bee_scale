/*
 * nvs_flash.h — host-oldali stub (node_queue_sim tesztpad).
 */
#ifndef MOCK_NVS_FLASH_H
#define MOCK_NVS_FLASH_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nvs_flash_init_partition(const char *partition_label);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_NVS_FLASH_H */

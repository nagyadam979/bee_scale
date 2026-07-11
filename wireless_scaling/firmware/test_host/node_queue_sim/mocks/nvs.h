/*
 * nvs.h — host-oldali stub az ESP-IDF NVS API-hoz (node_queue_sim tesztpad).
 * A node_buffer.c által használt felület + a valós IDF hibakód-értékek.
 * Az implementáció a nvs_sim.c-ben van (kulcs->blob map megosztott memóriában,
 * áramszünet-injektálással).
 */
#ifndef MOCK_NVS_H
#define MOCK_NVS_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_NVS_BASE              0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED   (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND         (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_TYPE_MISMATCH     (ESP_ERR_NVS_BASE + 0x03)
#define ESP_ERR_NVS_READ_ONLY         (ESP_ERR_NVS_BASE + 0x04)
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE  (ESP_ERR_NVS_BASE + 0x05)
#define ESP_ERR_NVS_INVALID_NAME      (ESP_ERR_NVS_BASE + 0x06)
#define ESP_ERR_NVS_INVALID_HANDLE    (ESP_ERR_NVS_BASE + 0x07)
#define ESP_ERR_NVS_REMOVE_FAILED     (ESP_ERR_NVS_BASE + 0x08)
#define ESP_ERR_NVS_KEY_TOO_LONG      (ESP_ERR_NVS_BASE + 0x09)
#define ESP_ERR_NVS_PAGE_FULL         (ESP_ERR_NVS_BASE + 0x0a)
#define ESP_ERR_NVS_INVALID_STATE     (ESP_ERR_NVS_BASE + 0x0b)
#define ESP_ERR_NVS_INVALID_LENGTH    (ESP_ERR_NVS_BASE + 0x0c)

#define NVS_KEY_NAME_MAX_SIZE 16

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE,
} nvs_open_mode_t;

typedef enum {
    NVS_TYPE_U8   = 0x01,
    NVS_TYPE_I8   = 0x11,
    NVS_TYPE_U16  = 0x02,
    NVS_TYPE_I16  = 0x12,
    NVS_TYPE_U32  = 0x04,
    NVS_TYPE_I32  = 0x14,
    NVS_TYPE_U64  = 0x08,
    NVS_TYPE_I64  = 0x18,
    NVS_TYPE_STR  = 0x21,
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY  = 0xff,
} nvs_type_t;

typedef struct nvs_opaque_iterator_t *nvs_iterator_t;

typedef struct {
    char namespace_name[16];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
} nvs_entry_info_t;

esp_err_t nvs_open_from_partition(const char *part_name, const char *namespace_name,
                                  nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
void      nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);

esp_err_t nvs_entry_find_in_handle(nvs_handle_t handle, nvs_type_t type,
                                   nvs_iterator_t *output_iterator);
esp_err_t nvs_entry_next(nvs_iterator_t *iterator);
esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info);
void      nvs_release_iterator(nvs_iterator_t iterator);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_NVS_H */

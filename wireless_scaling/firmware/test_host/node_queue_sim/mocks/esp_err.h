/*
 * esp_err.h — host-oldali stub az ESP-IDF hibakódokhoz (node_queue_sim tesztpad).
 * Az értékek a valós IDF esp_err.h értékeivel egyeznek, hogy a logok összevethetők legyenek.
 */
#ifndef MOCK_ESP_ERR_H
#define MOCK_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                 -1
#define ESP_ERR_NO_MEM           0x101
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_STATE    0x103
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_NOT_FOUND        0x105
#define ESP_ERR_NOT_SUPPORTED    0x106
#define ESP_ERR_TIMEOUT          0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC      0x109
#define ESP_ERR_INVALID_VERSION  0x10A

#ifdef __cplusplus
extern "C" {
#endif

const char *esp_err_to_name(esp_err_t err);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_ESP_ERR_H */

#ifndef RING_HOST_H
#define RING_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flash_sim.h"

typedef int esp_err_t;
typedef int nvs_handle_t;
typedef void *SemaphoreHandle_t;

#define ESP_OK                    0
#define ESP_FAIL                 -1
#define ESP_ERR_INVALID_ARG      0x102
#define ESP_ERR_INVALID_SIZE     0x104
#define ESP_ERR_INVALID_CRC      0x109
#define ESP_ERR_NVS_NOT_FOUND    0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c

#define NVS_READWRITE 1
#define ESP_PARTITION_TYPE_DATA 1
#define portMAX_DELAY 0xffffffffu

#define BACKUP_SLOT_SIZE      256u
#define BACKUP_SECTOR_SIZE    4096u
#define BACKUP_MAGIC_V1       0x42534D31UL
#define BACKUP_MAGIC_V2       0x42534D32UL
#define BACKUP_MAGIC_V3       0x42534D33UL
#define BACKUP_FORMAT_VERSION 3U
#define BACKUP_INDEX_MAGIC    0x42495832UL
#define BACKUP_INDEX_VERSION  3U
#define REC_TYPE_NODE         1
#define REC_TYPE_MASTER       2
#define RAIN_MM_PER_TIP       0.2794f

typedef struct {
    uint32_t size;
} esp_partition_t;

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *part, size_t off, void *dst, size_t len);
esp_err_t esp_partition_write(const esp_partition_t *part, size_t off, const void *src, size_t len);
esp_err_t esp_partition_erase_range(const esp_partition_t *part, size_t off, size_t len);

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t value);
esp_err_t nvs_commit(nvs_handle_t h);

static inline int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks)
{ (void)sem; (void)ticks; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t sem)
{ (void)sem; return 1; }

const char *esp_err_to_name(esp_err_t err);

#define ESP_LOGE(...) do { if (g_mock_log_enabled) fsim_log(__VA_ARGS__); } while (0)
#define ESP_LOGW(...) do { if (g_mock_log_enabled) fsim_log(__VA_ARGS__); } while (0)
#define ESP_LOGI(...) do { if (g_mock_log_enabled) fsim_log(__VA_ARGS__); } while (0)
void fsim_log(const char *tag, const char *fmt, ...);

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t unit_id;
    uint8_t valid_mask;
    uint32_t sensor_error_mask;
    int8_t rssi;
    uint8_t src_mac[6];
    uint32_t seq;
    uint32_t boot_id;
    uint32_t rx_ms;
    uint32_t ts;
    uint32_t vbat_mv;
    uint32_t gap;
    uint32_t rain_tips_total;
    uint32_t rain_tips_delta;
    int32_t weight_raw;
    int32_t hx711_raw;
    float temp_c;
    float ds0_hive_c;
    float ds1_scale_c;
    float aht20_t_c;
    float aht20_rh;
    float bmp280_t_c;
    float bmp280_pa;
    float rain_mm_total;
    float rain_mm_delta;
} host_master_record_t;

void prod_reset_and_init(void);
bool prod_enqueue(host_master_record_t *rec);
bool prod_peek(host_master_record_t *rec);
bool prod_pop(void);
uint32_t prod_count(void);
uint32_t prod_head(void);
uint32_t prod_tail(void);
uint32_t prod_write_seq(void);
uint32_t prod_slot_count(void);
uint32_t prod_master_seq(void);
bool prod_enabled(void);
void prod_set_rain(uint32_t tips, uint32_t reported);
int prod_master_record_key(char *out, size_t cap, const host_master_record_t *rec);

#endif

/*
 * bee_scale MASTER — final v1 firmware.
 *
 * Always-on ESP32-S3-Zero master:
 *   - WiFi STA + ESP-NOW RX in parallel
 *   - node payload receive, CRC, seq dedup/gap accounting
 *   - local master sensors: HX711 raw, 2x DS18B20, AHT20, BMP280, rain tipping switch
 *   - Firebase RTDB REST upload in cloud builds
 *   - flash-backed offline backlog after failed cloud delivery
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_now.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "protocol.h"

#ifndef MASTER_CLOUD_MODE
#define MASTER_CLOUD_MODE 0
#endif
#ifndef MASTER_WEB_STATUS
#define MASTER_WEB_STATUS 0
#endif

#if MASTER_CLOUD_MODE
#if __has_include("cloud_config.h")
#include "cloud_config.h"
#else
#error "MASTER_CLOUD_MODE=1 mellett kell main/cloud_config.h (minta: cloud_config.example.h)"
#endif
#elif MASTER_WEB_STATUS
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#error "MASTER_WEB_STATUS=1 mellett kell main/wifi_config.h (minta: wifi_config.example.h)"
#endif
#endif

#define MASTER_WIFI_ENABLED (MASTER_CLOUD_MODE || MASTER_WEB_STATUS)
#ifndef MASTER_FIREBASE_HIVE_PATH_SUFFIX
#define MASTER_FIREBASE_HIVE_PATH_SUFFIX ""
#endif
#ifndef MASTER_FIREBASE_ROOT_PATH
#define MASTER_FIREBASE_ROOT_PATH ""
#endif
#ifndef MASTER_CLOUD_UPLOAD_MASTER
#define MASTER_CLOUD_UPLOAD_MASTER 1
#endif
#ifndef MASTER_FIREBASE_TARGET_HIVE_ID
#define MASTER_FIREBASE_TARGET_HIVE_ID MASTER_FIREBASE_HIVE_ID
#endif
#ifndef MASTER_CLOUD_HIVE2_SCHEMA
#define MASTER_CLOUD_HIVE2_SCHEMA 0
#endif

#define MASTER_UNIT_ID                 0
#define MASTER_SAMPLE_PERIOD_SEC       1200
#define MASTER_VALID_EPOCH_MIN         1700000000UL

#ifndef MASTER_ESPNOW_CHANNEL
#define MASTER_ESPNOW_CHANNEL          6
#endif
#ifndef MASTER_TEST_MODE
#define MASTER_TEST_MODE               0
#endif
#ifndef MASTER_WIFI_SCAN_DIAG
#define MASTER_WIFI_SCAN_DIAG          0
#endif
#ifndef MASTER_SCAN_MAX_APS
#define MASTER_SCAN_MAX_APS            32
#endif
#ifndef MASTER_TEST_SSID
#define MASTER_TEST_SSID               ""
#endif
#ifndef MASTER_WEB_REFRESH_SEC
#define MASTER_WEB_REFRESH_SEC         5
#endif

#define HX711_SCK_GPIO                 GPIO_NUM_2
#define HX711_DOUT_GPIO                GPIO_NUM_4
#define DS18B20_GPIO                   GPIO_NUM_5
#define RAIN_GPIO                      GPIO_NUM_7
#define I2C_SDA_GPIO                   GPIO_NUM_8
#define I2C_SCL_GPIO                   GPIO_NUM_9

#define I2C_PORT                       I2C_NUM_0
#define I2C_FREQ_HZ                    100000
#define I2C_TIMEOUT_MS                 100
#define AHT20_ADDR                     0x38
#define BMP280_ADDR_0                  0x76
#define BMP280_ADDR_1                  0x77
#define BMP280_CHIP_ID                 0x58

#define HX711_SAMPLES                  8
#define HX711_READY_TIMEOUT_MS         700
#define DS18B20_MAX_SENSORS            4
#define DS18B20_EXPECTED_COUNT         2
/* Explicit ROM -> szerep mapping. Élesítéskor a "DS<n> ROM=..." logból azonosított
 * 16 NAGYBETŰS hex karakter kerüljön ide (vagy -D build flaggel felülírható).
 * Üres string = fallback a rendezett ROM-sorrendre (index 0 -> hive_c,
 * index 1 -> scale_c) — szondacserénél a szerepek felcserélődhetnek! */
#ifndef DS18B20_ROM_HIVE
#define DS18B20_ROM_HIVE               ""
#endif
#ifndef DS18B20_ROM_SCALE
#define DS18B20_ROM_SCALE              ""
#endif

#define RAIN_SAMPLE_PERIOD_MS          10
#define RAIN_DEBOUNCE_MS               20
#define RAIN_LOCKOUT_MS                250
#define RAIN_MM_PER_TIP                0.2794f

#define MASTER_RX_QUEUE_LEN            64
#define MASTER_MAX_NODES               16
#define MASTER_SEND_TIMEOUT_MS         50
#define MASTER_NO_MEM_RETRY_MS         2
#define MASTER_STAT_PERIOD_MS          10000

#ifndef MASTER_NODE_PERIOD_SEC
#define MASTER_NODE_PERIOD_SEC         1200
#endif
#ifndef MASTER_NODE_SLOT_SPACING_SEC
#define MASTER_NODE_SLOT_SPACING_SEC   20
#endif
#ifndef MASTER_NODE_SLOT_GUARD_SEC
#define MASTER_NODE_SLOT_GUARD_SEC     3
#endif

#define MASTER_RECONN_BACKOFF_MIN_MS   1000
#define MASTER_RECONN_BACKOFF_MAX_MS   32000
#define MASTER_RECONN_JITTER_MS        500
#define MASTER_HTTP_TIMEOUT_MS         5000
#define MASTER_FIREBASE_TOKEN_MARGIN_S 120
#define MASTER_CLOUD_MAX_ATTEMPTS      5
#define MASTER_CLOUD_RETRY_BASE_MS     2000
#define MASTER_CLOUD_RETRY_MAX_MS      60000

#define BACKUP_SLOT_SIZE               256
#define BACKUP_SECTOR_SIZE             4096
#define BACKUP_MAGIC_V1                0x42534D31UL
#define BACKUP_MAGIC_V2                0x42534D32UL /* 86-byte, pre sensor_error_mask */
#define BACKUP_MAGIC_V3                0x42534D33UL
#define BACKUP_FORMAT_VERSION          3U
#define BACKUP_INDEX_MAGIC             0x42495832UL
#define BACKUP_INDEX_VERSION           3U

#define REC_TYPE_NODE                  1
#define REC_TYPE_MASTER                2

#define REC_VALID_TEMP                 (1U << 0)
#define REC_VALID_DS0                  (1U << 1)
#define REC_VALID_DS1                  (1U << 2)
#define REC_VALID_AHT                  (1U << 3)
#define REC_VALID_BMP                  (1U << 4)
#define REC_VALID_HX711                (1U << 5)

static const char *TAG = "MASTER";

typedef struct {
    uint8_t rom[8];
} ds18b20_sensor_t;

typedef struct {
    uint16_t dig_t1;
    int16_t dig_t2;
    int16_t dig_t3;
    uint16_t dig_p1;
    int16_t dig_p2;
    int16_t dig_p3;
    int16_t dig_p4;
    int16_t dig_p5;
    int16_t dig_p6;
    int16_t dig_p7;
    int16_t dig_p8;
    int16_t dig_p9;
    uint8_t addr;
    bool present;
} bmp280_cal_t;

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
} master_record_t;

/* A korábbi, pontosan 86 bájtos rekord: az új mező miatt ezt nem szabad az
 * aktuális sizeof()-fal dekódolni. */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t unit_id;
    uint8_t valid_mask;
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
} master_record_v2_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t unit_id;
    uint8_t valid_mask;
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
} master_record_v1_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_seq;
    uint32_t crc32;
    uint32_t format_version;
    master_record_t rec;
    uint8_t pad[BACKUP_SLOT_SIZE - 16 - sizeof(master_record_t)];
} backup_slot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_seq;
    uint32_t crc32;
    uint32_t format_version;
    uint8_t payload[BACKUP_SLOT_SIZE - 16];
} backup_raw_slot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t head;
    uint32_t tail;
    uint32_t write_seq;
    uint32_t master_seq;
    uint32_t rain_reported;
    uint32_t crc32;
} backup_index_t;

typedef struct __attribute__((packed)) {
    uint32_t head;
    uint32_t tail;
    uint32_t write_seq;
    uint32_t crc32;
} backup_index_legacy_t;

typedef enum {
    CLOUD_RESULT_OK,
    CLOUD_RESULT_SKIP,
    CLOUD_RESULT_DROP,
    CLOUD_RESULT_RETRY,
} cloud_result_t;

typedef enum {
    RECORD_STORE_DURABLE,
    RECORD_STORE_SKIPPED,
    RECORD_STORE_FAILED,
} record_store_result_t;

_Static_assert(sizeof(master_record_t) == sizeof(master_record_v2_t) + sizeof(uint32_t),
               "master_record V2 migration layout changed");
_Static_assert(sizeof(master_record_v1_t) == 82, "unexpected pre-d99 record size");
_Static_assert(sizeof(master_record_v2_t) == 86, "unexpected legacy record size");
_Static_assert(sizeof(master_record_t) == 90, "unexpected current record size");
_Static_assert(sizeof(backup_slot_t) == BACKUP_SLOT_SIZE, "backup slot size changed");
_Static_assert(sizeof(backup_raw_slot_t) == BACKUP_SLOT_SIZE, "raw backup slot size changed");
_Static_assert(sizeof(backup_index_t) == 32, "backup index size changed");
_Static_assert(sizeof(backup_index_legacy_t) == 16, "legacy backup index size changed");

typedef struct {
    sz2_payload_t payload;
    sz2_payload_v1_t payload_v1;
    sz2_hello_t hello;
    uint8_t src_mac[6];
    int8_t rssi;
    uint16_t len;
} rx_item_t;

typedef struct {
    bool in_use;
    uint8_t node_id;
    uint8_t mac[6];
    uint32_t boot_id;
    bool have_boot_id;
    bool have_seq;
    uint32_t last_seq;
    uint32_t rx_valid;
    uint32_t rx_new;
    uint32_t rx_dup;
    uint32_t miss_cum;
    uint32_t crc_err;
    uint32_t ack_sent;
    uint32_t ack_txfail;
    uint32_t resets;
    int32_t rssi_min;
    int32_t rssi_max;
    int64_t rssi_sum;
    uint32_t rssi_n;
    int64_t last_rx_us;
    bool have_measurement;
    bool last_weight_valid;
    int32_t last_weight_raw;
    bool last_temp_valid;
    float last_temp_c;
    uint16_t last_vbat_mv;
    uint32_t last_sensor_error_mask;
    int8_t last_rssi;
} node_state_t;

static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_tx_done;
static SemaphoreHandle_t s_rain_mutex;
static SemaphoreHandle_t s_backup_mutex;
#if MASTER_WIFI_ENABLED
static SemaphoreHandle_t s_reconn_sem;
#endif

static node_state_t s_nodes[MASTER_MAX_NODES];
static ds18b20_sensor_t s_ds18b20[DS18B20_MAX_SENSORS];
static int s_ds18b20_count;
static bool s_ds18_role_present[DS18B20_EXPECTED_COUNT];
static bmp280_cal_t s_bmp280;
static bool s_aht20_present;

static nvs_handle_t s_master_nvs;
static nvs_handle_t s_rain_nvs;
static nvs_handle_t s_backup_nvs;
static uint32_t s_master_seq;
/* Indulásonként változó névtér a master felhőrekordjaihoz. Az NVS elvesztésekor a
 * sorszám újraindulhat, ezért a seq önmagában nem lehet globálisan egyedi kulcs. */
static uint32_t s_master_boot_id;
static uint32_t s_rain_tip_count;
static uint32_t s_rain_last_reported;

static const esp_partition_t *s_backup_part;
static uint32_t s_backup_slot_count;
static uint32_t s_backup_head;
static uint32_t s_backup_tail;
static uint32_t s_backup_write_seq;

static volatile uint32_t s_q_dropped;
static volatile uint32_t s_diag_recv_cb;
static volatile uint32_t s_diag_len_bad;
static volatile uint32_t s_diag_ver_bad;
static volatile uint32_t s_ack_status_fail;
static volatile uint32_t s_cloud_ok;
static volatile uint32_t s_cloud_fail;
static volatile uint32_t s_cloud_skip;
static volatile uint32_t s_cloud_drop;
static volatile uint32_t s_backup_enqueued;
static volatile uint32_t s_backup_uploaded;
static volatile uint32_t s_backup_bad;
static volatile uint32_t s_backup_full;
static volatile uint32_t s_registry_full_noack;

#if MASTER_WIFI_ENABLED
static volatile bool s_wifi_assoc;
static volatile bool s_wifi_ip;
static volatile uint32_t s_disconn_cnt;
static volatile uint32_t s_got_ip_cnt;
static volatile uint32_t s_backoff_ms = MASTER_RECONN_BACKOFF_MIN_MS;
#endif
#if MASTER_CLOUD_MODE
static char s_firebase_token[2048];
static int64_t s_firebase_token_expire_us;
static int64_t s_auth_probe_not_before_us;
static bool s_auth_401_refresh_used;
#endif

static uint32_t __attribute__((unused)) crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static uint32_t current_epoch_s(void)
{
    time_t now = 0;
    time(&now);
    if ((uint32_t)now < MASTER_VALID_EPOCH_MIN) {
        return 0;
    }
    return (uint32_t)now;
}

static uint32_t millis_u32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t millis_i64(void)
{
    return esp_timer_get_time() / 1000;
}

static void nvs_init_all(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(nvs_open("master", NVS_READWRITE, &s_master_nvs));
    ESP_ERROR_CHECK(nvs_open("rain", NVS_READWRITE, &s_rain_nvs));
    ESP_ERROR_CHECK(nvs_open("backup", NVS_READWRITE, &s_backup_nvs));

    err = nvs_get_u32(s_master_nvs, "seq", &s_master_seq);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_master_seq = 0;
    } else {
        ESP_ERROR_CHECK(err);
    }
    err = nvs_get_u32(s_rain_nvs, "tips", &s_rain_tip_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_rain_tip_count = 0;
    } else {
        ESP_ERROR_CHECK(err);
    }
    err = nvs_get_u32(s_rain_nvs, "reported", &s_rain_last_reported);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_rain_last_reported = s_rain_tip_count;
        ESP_ERROR_CHECK(nvs_set_u32(s_rain_nvs, "reported", s_rain_last_reported));
        ESP_ERROR_CHECK(nvs_commit(s_rain_nvs));
    } else {
        ESP_ERROR_CHECK(err);
        if (s_rain_last_reported > s_rain_tip_count) {
            ESP_LOGW(TAG, "rain cursor ahead of counter; restoring tips reported=%u tips=%u",
                     (unsigned)s_rain_last_reported, (unsigned)s_rain_tip_count);
            s_rain_tip_count = s_rain_last_reported;
            ESP_ERROR_CHECK(nvs_set_u32(s_rain_nvs, "tips", s_rain_tip_count));
            ESP_ERROR_CHECK(nvs_commit(s_rain_nvs));
        }
    }
}

static bool rain_store_count(void)
{
    esp_err_t err = nvs_set_u32(s_rain_nvs, "tips", s_rain_tip_count);
    if (err == ESP_OK) err = nvs_commit(s_rain_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rain tip persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void rain_apply_durable_cursor(uint32_t reported)
{
    xSemaphoreTake(s_rain_mutex, portMAX_DELAY);
    if (reported > s_rain_tip_count) {
        s_rain_tip_count = reported;
        (void)rain_store_count();
    }
    s_rain_last_reported = reported;
    xSemaphoreGive(s_rain_mutex);
}

static uint32_t __attribute__((unused)) rain_get_durable_cursor(void)
{
    xSemaphoreTake(s_rain_mutex, portMAX_DELAY);
    uint32_t reported = s_rain_last_reported;
    xSemaphoreGive(s_rain_mutex);
    return reported;
}

static bool backup_store_index_values(uint32_t head, uint32_t tail, uint32_t write_seq,
                                      uint32_t master_seq, uint32_t rain_reported)
{
    backup_index_t index = {
        .magic = BACKUP_INDEX_MAGIC,
        .version = BACKUP_INDEX_VERSION,
        .head = head,
        .tail = tail,
        .write_seq = write_seq,
        .master_seq = master_seq,
        .rain_reported = rain_reported,
        .crc32 = 0,
    };
    index.crc32 = crc32_update(0, (const uint8_t *)&index,
                               sizeof(index) - sizeof(index.crc32));
    esp_err_t err = nvs_set_blob(s_backup_nvs, "state2", &index, sizeof(index));
    if (err == ESP_OK) err = nvs_commit(s_backup_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backup index persist failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static uint32_t backup_count_unlocked(void)
{
    if (s_backup_head >= s_backup_tail) {
        return s_backup_head - s_backup_tail;
    }
    return s_backup_slot_count - s_backup_tail + s_backup_head;
}

static uint32_t master_next_seq(void)
{
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    uint32_t seq = s_master_seq + 1;
    xSemaphoreGive(s_backup_mutex);
    return seq;
}

static void backup_init(void)
{
    s_backup_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "backup");
    if (s_backup_part == NULL) {
        ESP_LOGW(TAG, "backup partition not found; flash backlog disabled");
        return;
    }
    s_backup_slot_count = s_backup_part->size / BACKUP_SLOT_SIZE;

    backup_index_t index = {0};
    size_t index_len = sizeof(index);
    esp_err_t err = nvs_get_blob(s_backup_nvs, "state2", &index, &index_len);
    bool valid_index = err == ESP_OK && index_len == sizeof(index) &&
                       index.magic == BACKUP_INDEX_MAGIC &&
                       (index.version == BACKUP_INDEX_VERSION || index.version == 2U) &&
                       index.crc32 == crc32_update(0, (const uint8_t *)&index,
                                                  sizeof(index) - sizeof(index.crc32));
    bool migrate_index = false;
    if (valid_index) {
        s_backup_head = index.head;
        s_backup_tail = index.tail;
        s_backup_write_seq = index.write_seq;
        s_master_seq = index.master_seq;
        s_rain_last_reported = index.rain_reported;
        migrate_index = index.version != BACKUP_INDEX_VERSION;
    } else if (err == ESP_OK && index_len == sizeof(backup_index_legacy_t)) {
        const backup_index_legacy_t *legacy = (const backup_index_legacy_t *)&index;
        valid_index = legacy->crc32 ==
                      crc32_update(0, (const uint8_t *)legacy,
                                   sizeof(*legacy) - sizeof(legacy->crc32));
        if (valid_index) {
            s_backup_head = legacy->head;
            s_backup_tail = legacy->tail;
            s_backup_write_seq = legacy->write_seq;
            migrate_index = true;
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        esp_err_t head_err = nvs_get_u32(s_backup_nvs, "head", &s_backup_head);
        esp_err_t tail_err = nvs_get_u32(s_backup_nvs, "tail", &s_backup_tail);
        esp_err_t seq_err = nvs_get_u32(s_backup_nvs, "wseq", &s_backup_write_seq);
        if (head_err == ESP_ERR_NVS_NOT_FOUND) s_backup_head = 0;
        else if (head_err != ESP_OK) valid_index = false;
        if (tail_err == ESP_ERR_NVS_NOT_FOUND) s_backup_tail = 0;
        else if (tail_err != ESP_OK) valid_index = false;
        if (seq_err == ESP_ERR_NVS_NOT_FOUND) s_backup_write_seq = 0;
        else if (seq_err != ESP_OK) valid_index = false;
        valid_index = head_err == ESP_OK || head_err == ESP_ERR_NVS_NOT_FOUND;
        valid_index = valid_index && (tail_err == ESP_OK || tail_err == ESP_ERR_NVS_NOT_FOUND);
        valid_index = valid_index && (seq_err == ESP_OK || seq_err == ESP_ERR_NVS_NOT_FOUND);
        migrate_index = valid_index;
    }
    if (!valid_index || s_backup_head >= s_backup_slot_count ||
        s_backup_tail >= s_backup_slot_count) {
        ESP_LOGE(TAG, "backup index invalid; durable backlog disabled");
        s_backup_part = NULL;
        s_backup_slot_count = 0;
        s_backup_head = 0;
        s_backup_tail = 0;
        return;
    }
    if (migrate_index &&
        !backup_store_index_values(s_backup_head, s_backup_tail, s_backup_write_seq,
                                   s_master_seq, s_rain_last_reported)) {
        ESP_LOGE(TAG, "backup index migration failed; durable backlog disabled");
        s_backup_part = NULL;
        s_backup_slot_count = 0;
        return;
    }
    rain_apply_durable_cursor(s_rain_last_reported);
    ESP_LOGI(TAG, "backup partition size=%u slots=%u pending=%u",
             (unsigned)s_backup_part->size, (unsigned)s_backup_slot_count,
             (unsigned)backup_count_unlocked());
}

#if MASTER_CLOUD_MODE
static bool backup_slot_decode(const backup_raw_slot_t *slot, master_record_t *rec,
                               bool log_migration)
{
    if (slot->magic == BACKUP_MAGIC_V3 &&
        slot->format_version == BACKUP_FORMAT_VERSION &&
        slot->crc32 == crc32_update(0, slot->payload, sizeof(master_record_t))) {
        memcpy(rec, slot->payload, sizeof(*rec));
        return true;
    }
    if (slot->magic == BACKUP_MAGIC_V2 &&
        slot->format_version == 2U &&
        slot->crc32 == crc32_update(0, slot->payload, sizeof(master_record_v2_t))) {
        master_record_v2_t old;
        memcpy(&old, slot->payload, sizeof(old));
        memset(rec, 0, sizeof(*rec));
        rec->type=old.type; rec->unit_id=old.unit_id; rec->valid_mask=old.valid_mask;
        rec->rssi=old.rssi; memcpy(rec->src_mac, old.src_mac, sizeof(rec->src_mac));
        rec->seq=old.seq; rec->boot_id=old.boot_id; rec->rx_ms=old.rx_ms; rec->ts=old.ts;
        rec->vbat_mv=old.vbat_mv; rec->gap=old.gap; rec->rain_tips_total=old.rain_tips_total;
        rec->rain_tips_delta=old.rain_tips_delta; rec->weight_raw=old.weight_raw;
        rec->hx711_raw=old.hx711_raw; rec->temp_c=old.temp_c; rec->ds0_hive_c=old.ds0_hive_c;
        rec->ds1_scale_c=old.ds1_scale_c; rec->aht20_t_c=old.aht20_t_c;
        rec->aht20_rh=old.aht20_rh; rec->bmp280_t_c=old.bmp280_t_c;
        rec->bmp280_pa=old.bmp280_pa; rec->rain_mm_total=old.rain_mm_total;
        rec->rain_mm_delta=old.rain_mm_delta;
        if (log_migration) ESP_LOGI(TAG, "backup V2 86-byte record migrated write_seq=%u",
                                   (unsigned)slot->write_seq);
        return true;
    }
    if (slot->magic != BACKUP_MAGIC_V1) return false;
    if (slot->crc32 == crc32_update(0, slot->payload, sizeof(master_record_v1_t))) {
        master_record_v1_t old;
        memcpy(&old, slot->payload, sizeof(old));
        memset(rec, 0, sizeof(*rec));
        rec->type=old.type; rec->unit_id=old.unit_id; rec->valid_mask=old.valid_mask;
        rec->rssi=old.rssi; memcpy(rec->src_mac, old.src_mac, sizeof(rec->src_mac));
        rec->seq=old.seq; rec->boot_id=old.boot_id; rec->rx_ms=old.rx_ms; rec->ts=old.ts;
        rec->vbat_mv=old.vbat_mv; rec->gap=old.gap; rec->rain_tips_total=old.rain_tips_total;
        rec->rain_tips_delta=old.rain_tips_delta; rec->weight_raw=old.weight_raw;
        rec->hx711_raw=old.hx711_raw; rec->temp_c=old.temp_c; rec->ds0_hive_c=old.ds0_hive_c;
        rec->ds1_scale_c=old.ds1_scale_c; rec->aht20_t_c=old.aht20_t_c;
        rec->aht20_rh=old.aht20_rh; rec->bmp280_t_c=old.bmp280_t_c;
        rec->bmp280_pa=old.bmp280_pa; rec->rain_mm_total=old.rain_mm_total;
        rec->rain_mm_delta = (float)rec->rain_tips_delta * RAIN_MM_PER_TIP;
        if (log_migration) {
            ESP_LOGI(TAG, "backup V1 record migrated write_seq=%u",
                     (unsigned)slot->write_seq);
        }
        return true;
    }
    return false;
}

static bool backup_slot_erased(const backup_raw_slot_t *slot)
{
    const uint8_t *bytes = (const uint8_t *)slot;
    for (size_t i = 0; i < sizeof(*slot); i++) {
        if (bytes[i] != 0xFF) return false;
    }
    return true;
}

static bool backup_prepare_head_unlocked(const master_record_t *incoming,
                                         bool *incoming_already_durable)
{
    *incoming_already_durable = false;
    const uint32_t slots_per_sector = BACKUP_SECTOR_SIZE / BACKUP_SLOT_SIZE;
    for (;;) {
        uint32_t next = (s_backup_head + 1) % s_backup_slot_count;
        if (next == s_backup_tail) {
            s_backup_full++;
            ESP_LOGE(TAG, "backup ring full; refusing type=%u unit=%u seq=%u",
                     incoming->type, incoming->unit_id, (unsigned)incoming->seq);
            return false;
        }

        backup_raw_slot_t existing;
        esp_err_t err = esp_partition_read(s_backup_part,
                                           s_backup_head * BACKUP_SLOT_SIZE,
                                           &existing, sizeof(existing));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "backup head read failed: %s", esp_err_to_name(err));
            return false;
        }
        if (backup_slot_erased(&existing)) {
            if ((s_backup_head % slots_per_sector) == 0) {
                uint32_t sector = s_backup_head / slots_per_sector;
                uint32_t tail_sector = s_backup_tail / slots_per_sector;
                if (s_backup_tail != s_backup_head && tail_sector == sector) {
                    s_backup_full++;
                    ESP_LOGE(TAG, "backup erase boundary reached unread sector; refusing type=%u unit=%u seq=%u",
                             incoming->type, incoming->unit_id, (unsigned)incoming->seq);
                    return false;
                }
                err = esp_partition_erase_range(s_backup_part,
                                                sector * BACKUP_SECTOR_SIZE,
                                                BACKUP_SECTOR_SIZE);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "backup erase failed: %s", esp_err_to_name(err));
                    return false;
                }
            }
            return true;
        }

        master_record_t recovered;
        bool valid = backup_slot_decode(&existing, &recovered, false);
        bool recovered_expected = valid && existing.write_seq == s_backup_write_seq + 1;
        if (!recovered_expected) {
            uint32_t sector = s_backup_head / slots_per_sector;
            if (s_backup_tail != s_backup_head) {
                /* Szektorhatáron az előző ring-kör konzumált slotjai maradnak a headnél.
                 * A szektor minden slotja a headtől előre esik, a pending [tail, head)
                 * tartományon kívül, ezért ha a tail nincs ebben a szektorban, a törlés
                 * nem érinthet olvasatlan rekordot. Az orphan-adopt ág fentebb már
                 * lefutott, így indexeletlen, de teljes slot ide nem juthat. */
                uint32_t tail_sector = s_backup_tail / slots_per_sector;
                bool head_on_boundary = (s_backup_head % slots_per_sector) == 0;
                if (!head_on_boundary || tail_sector == sector) {
                    s_backup_full++;
                    ESP_LOGE(TAG, "occupied unexpected backup head=%u while queue pending; refusing write",
                             (unsigned)s_backup_head);
                    return false;
                }
                err = esp_partition_erase_range(s_backup_part,
                                                sector * BACKUP_SECTOR_SIZE,
                                                BACKUP_SECTOR_SIZE);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "backup stale-sector erase failed: %s",
                             esp_err_to_name(err));
                    return false;
                }
                ESP_LOGI(TAG, "backup erased stale sector=%u at head=%u (previous ring lap)",
                         (unsigned)sector, (unsigned)s_backup_head);
                return true;
            }
            err = esp_partition_erase_range(s_backup_part,
                                            sector * BACKUP_SECTOR_SIZE,
                                            BACKUP_SECTOR_SIZE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "backup damaged-sector erase failed: %s",
                         esp_err_to_name(err));
                return false;
            }
            ESP_LOGW(TAG, "erased unexpected/partial backup head=%u after queue drained",
                     (unsigned)s_backup_head);
            return true;
        }

        uint32_t recovered_write_seq = existing.write_seq;
        uint32_t recovered_master_seq = s_master_seq;
        uint32_t recovered_rain_reported = rain_get_durable_cursor();
        ESP_LOGW(TAG, "recovered unindexed backup slot=%u write_seq=%u",
                 (unsigned)s_backup_head, (unsigned)existing.write_seq);
        if (recovered.type == REC_TYPE_MASTER) {
            if (recovered.seq > recovered_master_seq) {
                recovered_master_seq = recovered.seq;
            }
            if (recovered.rain_tips_total > recovered_rain_reported) {
                recovered_rain_reported = recovered.rain_tips_total;
            }
        }

        uint32_t recovered_head = next;
        bool stored = backup_store_index_values(recovered_head, s_backup_tail,
                                                recovered_write_seq,
                                                recovered_master_seq,
                                                recovered_rain_reported);
        if (!stored) return false;
        s_backup_head = recovered_head;
        s_backup_write_seq = recovered_write_seq;
        s_master_seq = recovered_master_seq;
        rain_apply_durable_cursor(recovered_rain_reported);
        s_backup_enqueued++;
        if (incoming->type == REC_TYPE_NODE &&
            recovered.type == incoming->type &&
            recovered.unit_id == incoming->unit_id && recovered.seq == incoming->seq &&
            recovered.boot_id == incoming->boot_id) {
            *incoming_already_durable = true;
            return true;
        }
    }
}

static bool backup_enqueue(master_record_t *rec)
{
    if (s_backup_part == NULL || s_backup_slot_count == 0) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    bool already_durable = false;
    if (!backup_prepare_head_unlocked(rec, &already_durable)) {
        xSemaphoreGive(s_backup_mutex);
        return false;
    }
    if (already_durable) {
        xSemaphoreGive(s_backup_mutex);
        return true;
    }

    if (rec->type == REC_TYPE_MASTER) {
        uint32_t rain_reported = rain_get_durable_cursor();
        if (rec->rain_tips_total < rain_reported) {
            rec->rain_tips_total = rain_reported;
            rec->rain_mm_total = (float)rain_reported * RAIN_MM_PER_TIP;
        }
        rec->seq = s_master_seq + 1;
        rec->rain_tips_delta = rec->rain_tips_total - rain_reported;
        rec->rain_mm_delta = (float)rec->rain_tips_delta * RAIN_MM_PER_TIP;
    }

    uint32_t next = (s_backup_head + 1) % s_backup_slot_count;
    backup_slot_t slot;
    memset(&slot, 0xFF, sizeof(slot));
    uint32_t next_write_seq = s_backup_write_seq + 1;
    slot.magic = BACKUP_MAGIC_V3;
    slot.write_seq = next_write_seq;
    slot.format_version = BACKUP_FORMAT_VERSION;
    memcpy(&slot.rec, rec, sizeof(slot.rec));
    slot.crc32 = crc32_update(0, (const uint8_t *)&slot.rec, sizeof(slot.rec));

    esp_err_t err = esp_partition_write(s_backup_part, s_backup_head * BACKUP_SLOT_SIZE,
                                        &slot, sizeof(slot));
    if (err == ESP_OK) {
        backup_slot_t verify;
        err = esp_partition_read(s_backup_part, s_backup_head * BACKUP_SLOT_SIZE,
                                 &verify, sizeof(verify));
        if (err == ESP_OK && memcmp(&verify, &slot, sizeof(slot)) != 0) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK) {
        uint32_t next_master_seq = s_master_seq;
        uint32_t next_rain_reported = rain_get_durable_cursor();
        if (rec->type == REC_TYPE_MASTER) {
            next_master_seq = rec->seq;
            next_rain_reported = rec->rain_tips_total;
        }
        if (backup_store_index_values(next, s_backup_tail, next_write_seq,
                                      next_master_seq, next_rain_reported)) {
            s_backup_head = next;
            s_backup_write_seq = next_write_seq;
            s_master_seq = next_master_seq;
            rain_apply_durable_cursor(next_rain_reported);
            s_backup_enqueued++;
        } else {
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "backup write/readback failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_backup_mutex);
    return err == ESP_OK;
}

static bool backup_peek(master_record_t *rec)
{
    if (s_backup_part == NULL || s_backup_head == s_backup_tail) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);

    backup_raw_slot_t slot;
    esp_err_t err = esp_partition_read(s_backup_part, s_backup_tail * BACKUP_SLOT_SIZE,
                                       &slot, sizeof(slot));
    if (err != ESP_OK) {
        /* Átmeneti olvasási hiba: a tail marad, a cloud task később újrapróbálja. */
        ESP_LOGW(TAG, "backup tail read failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_backup_mutex);
        return false;
    }
    bool valid = backup_slot_decode(&slot, rec, true);
    if (!valid) {
        s_backup_bad++;
        uint32_t next_tail = (s_backup_tail + 1) % s_backup_slot_count;
        uint32_t rain_reported = rain_get_durable_cursor();
        if (backup_store_index_values(s_backup_head, next_tail, s_backup_write_seq,
                                      s_master_seq, rain_reported)) {
            s_backup_tail = next_tail;
        }
        xSemaphoreGive(s_backup_mutex);
        return false;
    }

    xSemaphoreGive(s_backup_mutex);
    return true;
}

static bool backup_pop(void)
{
    if (s_backup_part == NULL || s_backup_head == s_backup_tail) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    uint32_t next_tail = (s_backup_tail + 1) % s_backup_slot_count;
    uint32_t rain_reported = rain_get_durable_cursor();
    bool stored = backup_store_index_values(s_backup_head, next_tail, s_backup_write_seq,
                                            s_master_seq, rain_reported);
    if (stored) {
        s_backup_tail = next_tail;
        s_backup_uploaded++;
    }
    xSemaphoreGive(s_backup_mutex);
    return stored;
}
#endif

static void hx711_init(void)
{
    gpio_config_t sck = {
        .pin_bit_mask = 1ULL << HX711_SCK_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sck));

    gpio_config_t dout = {
        .pin_bit_mask = 1ULL << HX711_DOUT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout));
    gpio_set_level(HX711_SCK_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static bool hx711_read_raw_once(int32_t *raw)
{
    int64_t start_us = esp_timer_get_time();
    while (gpio_get_level(HX711_DOUT_GPIO) == 1) {
        if ((esp_timer_get_time() - start_us) > (int64_t)HX711_READY_TIMEOUT_MS * 1000) {
            return false;
        }
        esp_rom_delay_us(100);
    }

    /* A 25 órajel alatt a PD_SCK nem maradhat >60 µs-ig magas (a HX711 power-downba
     * resetel és korrupt értéket ad), ezért a bit-bang preempció ellen védett. */
    static portMUX_TYPE hx711_mux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t value = 0;
    portENTER_CRITICAL(&hx711_mux);
    for (int i = 0; i < 24; i++) {
        gpio_set_level(HX711_SCK_GPIO, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (uint32_t)gpio_get_level(HX711_DOUT_GPIO);
        gpio_set_level(HX711_SCK_GPIO, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level(HX711_SCK_GPIO, 1);
    esp_rom_delay_us(1);
    gpio_set_level(HX711_SCK_GPIO, 0);
    esp_rom_delay_us(1);
    portEXIT_CRITICAL(&hx711_mux);

    if (value & 0x800000U) {
        value |= 0xFF000000U;
    }
    *raw = (int32_t)value;
    return true;
}

static bool hx711_read_avg(int32_t *avg)
{
    int64_t all_acc = 0, good_acc = 0;
    int ok = 0, good = 0;
    int zero_count = 0;
    int minus_one_count = 0;
    int32_t first = 0;
    bool constant = true;
    for (int i = 0; i < HX711_SAMPLES; i++) {
        int32_t raw = 0;
        if (hx711_read_raw_once(&raw)) {
            if (ok > 0 && raw != first) constant = false;
            if (ok == 0) first = raw;
            all_acc += raw;
            ok++;
            if (raw == 0) zero_count++;
            if (raw == -1) minus_one_count++;
            if (raw != 0 && raw != -1) { good_acc += raw; good++; }
        }
    }
    *avg = good >= HX711_SAMPLES / 2 ? (int32_t)(good_acc / good) :
                                        (ok ? (int32_t)(all_acc / ok) : 0);
    bool error = ok != HX711_SAMPLES || zero_count != 0 || minus_one_count != 0 ||
                 good < HX711_SAMPLES / 2 || constant;
    ESP_LOGI(TAG, "HX711 samples total=%u successful=%d zero=%d minus1=%d unique=%d used=%d avg=%ld",
             HX711_SAMPLES, ok, zero_count, minus_one_count, constant ? 1 : 2,
             good >= HX711_SAMPLES / 2 ? good : 0, (long)*avg);
    return !error;
}

static uint8_t crc8_maxim(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t in = data[i];
        for (int b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ in) & 1U;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            in >>= 1;
        }
    }
    return crc;
}

static uint8_t crc8_aht20(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void ow_drive_low(void)
{
    gpio_set_level(DS18B20_GPIO, 0);
}

static void ow_release(void)
{
    gpio_set_level(DS18B20_GPIO, 1);
}

static bool ow_reset(void)
{
    ow_drive_low();
    esp_rom_delay_us(480);
    ow_release();
    esp_rom_delay_us(70);
    bool present = gpio_get_level(DS18B20_GPIO) == 0;
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    if (bit) {
        ow_drive_low();
        esp_rom_delay_us(6);
        ow_release();
        esp_rom_delay_us(64);
    } else {
        ow_drive_low();
        esp_rom_delay_us(60);
        ow_release();
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    ow_drive_low();
    esp_rom_delay_us(6);
    ow_release();
    esp_rom_delay_us(9);
    int bit = gpio_get_level(DS18B20_GPIO);
    esp_rom_delay_us(55);
    return bit;
}

static void ow_write_byte(uint8_t value)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit((value >> i) & 1);
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) value |= (uint8_t)(1U << i);
    }
    return value;
}

static void ds18b20_init_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DS18B20_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ow_release();
}

static void rom_to_hex(const uint8_t rom[8], char out[17])
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        out[i * 2] = hex[(rom[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[rom[i] & 0x0F];
    }
    out[16] = '\0';
}

static int ds_rom_cmp(const void *a, const void *b)
{
    return memcmp(((const ds18b20_sensor_t *)a)->rom, ((const ds18b20_sensor_t *)b)->rom, 8);
}

static int ds18b20_search(ds18b20_sensor_t *out, int max_count)
{
    uint8_t rom[8] = {0};
    int last_discrepancy = 0;
    bool last_device = false;
    int found = 0;

    while (!last_device && found < max_count) {
        if (!ow_reset()) break;
        ow_write_byte(0xF0);
        int discrepancy_marker = 0;
        int rom_byte = 0;
        uint8_t rom_mask = 1;

        for (int bit_number = 1; bit_number <= 64; bit_number++) {
            int id_bit = ow_read_bit();
            int cmp_id_bit = ow_read_bit();
            int search_direction;
            if (id_bit == 1 && cmp_id_bit == 1) return found;
            if (id_bit != cmp_id_bit) {
                search_direction = id_bit;
            } else {
                if (bit_number < last_discrepancy) {
                    search_direction = (rom[rom_byte] & rom_mask) ? 1 : 0;
                } else {
                    search_direction = bit_number == last_discrepancy;
                }
                if (search_direction == 0) discrepancy_marker = bit_number;
            }
            if (search_direction) rom[rom_byte] |= rom_mask;
            else rom[rom_byte] &= (uint8_t)~rom_mask;
            ow_write_bit(search_direction);
            rom_mask <<= 1;
            if (rom_mask == 0) {
                rom_byte++;
                rom_mask = 1;
            }
        }
        last_discrepancy = discrepancy_marker;
        if (last_discrepancy == 0) last_device = true;
        if (rom[0] == 0x28 && crc8_maxim(rom, 7) == rom[7]) {
            memcpy(out[found++].rom, rom, sizeof(rom));
        }
    }
    qsort(out, found, sizeof(out[0]), ds_rom_cmp);
    return found;
}

static void ds18b20_apply_role_mapping(void)
{
    static const char *const role_rom[DS18B20_EXPECTED_COUNT] = {
        DS18B20_ROM_HIVE,  /* index 0 -> hive_c */
        DS18B20_ROM_SCALE, /* index 1 -> scale_c */
    };
    bool explicit_roles = role_rom[0][0] != '\0' || role_rom[1][0] != '\0';
    if (!explicit_roles) {
        for (int r = 0; r < DS18B20_EXPECTED_COUNT; r++) s_ds18_role_present[r] = r < s_ds18b20_count;
        return; /* csak teljesen üres konfigurációban megengedett az index-fallback */
    }
    ds18b20_sensor_t discovered[DS18B20_MAX_SENSORS];
    int discovered_count = s_ds18b20_count;
    memcpy(discovered, s_ds18b20, sizeof(discovered));
    memset(s_ds18b20, 0, sizeof(s_ds18b20));
    for (int r = 0; r < DS18B20_EXPECTED_COUNT; r++) {
        s_ds18_role_present[r] = false;
        if (role_rom[r][0] == '\0') continue;
        int found = -1;
        for (int i = 0; i < discovered_count; i++) {
            char hex[17];
            rom_to_hex(discovered[i].rom, hex);
            if (strcmp(hex, role_rom[r]) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            ESP_LOGW(TAG, "DS18B20 role=%d ROM=%s nincs a buszon; nincs index-fallback",
                     r, role_rom[r]);
            continue;
        }
        s_ds18b20[r] = discovered[found];
        s_ds18_role_present[r] = true;
    }
    s_ds18b20_count = DS18B20_EXPECTED_COUNT;
}

static void ds18b20_refresh_search(void)
{
    s_ds18b20_count = ds18b20_search(s_ds18b20, DS18B20_MAX_SENSORS);
    ds18b20_apply_role_mapping();
    ESP_LOGI(TAG, "DS18B20 found=%d expected=%d", s_ds18b20_count, DS18B20_EXPECTED_COUNT);
    for (int i = 0; i < s_ds18b20_count; i++) {
        char rom[17];
        rom_to_hex(s_ds18b20[i].rom, rom);
        ESP_LOGI(TAG, "DS%d ROM=%s role=%s", i, rom,
                 i == 0 ? "hive_c" : (i == 1 ? "scale_c" : "extra"));
    }
}

static void ds18b20_match_rom(const uint8_t rom[8])
{
    ow_write_byte(0x55);
    for (int i = 0; i < 8; i++) ow_write_byte(rom[i]);
}

static bool ds18b20_read_all(float temps[DS18B20_MAX_SENSORS], uint32_t *error_mask)
{
    for (int i = 0; i < DS18B20_MAX_SENSORS; i++) temps[i] = NAN;
    if (error_mask) *error_mask = 0;
    if (s_ds18b20_count <= 0) {
        if (error_mask) *error_mask = SZ2_SENSOR_ERR_HIVE_DS18B20 | SZ2_SENSOR_ERR_SCALE_DS18B20;
        return false;
    }

    if (!ow_reset()) {
        if (error_mask) *error_mask = SZ2_SENSOR_ERR_HIVE_DS18B20 | SZ2_SENSOR_ERR_SCALE_DS18B20;
        return false;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0x44);
    vTaskDelay(pdMS_TO_TICKS(750));

    bool all_ok = true;
    for (int i = 0; i < s_ds18b20_count; i++) {
        if (i < DS18B20_EXPECTED_COUNT && !s_ds18_role_present[i]) {
            all_ok = false;
            if (error_mask) *error_mask |= i == 0 ? SZ2_SENSOR_ERR_HIVE_DS18B20 : SZ2_SENSOR_ERR_SCALE_DS18B20;
            continue;
        }
        if (!ow_reset()) {
            all_ok = false;
            if (error_mask && i < 2) *error_mask |= i == 0 ? SZ2_SENSOR_ERR_HIVE_DS18B20 : SZ2_SENSOR_ERR_SCALE_DS18B20;
            continue;
        }
        ds18b20_match_rom(s_ds18b20[i].rom);
        ow_write_byte(0xBE);
        uint8_t sp[9];
        for (int b = 0; b < 9; b++) sp[b] = ow_read_byte();
        bool all_zero = true;
        bool all_ff = true;
        for (int b = 0; b < 9; b++) {
            all_zero = all_zero && sp[b] == 0x00;
            all_ff = all_ff && sp[b] == 0xFF;
        }
        int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
        float temp_c = (float)raw / 16.0f;
        if (all_zero || all_ff) {
            all_ok = false;
            if (error_mask && i < 2) *error_mask |= i == 0 ? SZ2_SENSOR_ERR_HIVE_DS18B20 : SZ2_SENSOR_ERR_SCALE_DS18B20;
            continue;
        }
        temps[i] = temp_c; /* CRC/range/85 után is őrizzük meg a tényleges számot. */
        if (crc8_maxim(sp, 8) != sp[8] || temp_c < -55.0f || temp_c > 125.0f || temp_c == 85.0f) {
            all_ok = false;
            if (error_mask && i < 2) *error_mask |= i == 0 ? SZ2_SENSOR_ERR_HIVE_DS18B20 : SZ2_SENSOR_ERR_SCALE_DS18B20;
        }
    }
    return all_ok;
}

static esp_err_t i2c_write_bytes(uint8_t addr, const uint8_t *data, size_t len)
{
    return i2c_master_write_to_device(I2C_PORT, addr, data, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t i2c_read_bytes(uint8_t addr, uint8_t *data, size_t len)
{
    return i2c_master_read_from_device(I2C_PORT, addr, data, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len,
                                uint8_t *rd, size_t rd_len)
{
    return i2c_master_write_read_device(I2C_PORT, addr, wr, wr_len, rd, rd_len,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static void i2c_init_bus(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0));
}

static bool aht20_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(40));
    uint8_t status_cmd = 0x71;
    uint8_t status = 0;
    if (i2c_write_read(AHT20_ADDR, &status_cmd, 1, &status, 1) != ESP_OK) {
        return false;
    }
    if ((status & 0x08) == 0) {
        uint8_t init_cmd[] = {0xBE, 0x08, 0x00};
        if (i2c_write_bytes(AHT20_ADDR, init_cmd, sizeof(init_cmd)) != ESP_OK) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static bool aht20_read(float *temp_c, float *rh)
{
    uint8_t cmd[] = {0xAC, 0x33, 0x00};
    if (i2c_write_bytes(AHT20_ADDR, cmd, sizeof(cmd)) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(90));
    uint8_t data[7] = {0};
    if (i2c_read_bytes(AHT20_ADDR, data, sizeof(data)) != ESP_OK) return false;
    if (data[0] & 0x80) return false;
    if (crc8_aht20(data, 6) != data[6]) return false;

    uint32_t raw_h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | ((uint32_t)data[3] >> 4);
    uint32_t raw_t = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];
    *rh = (float)raw_h * 100.0f / 1048576.0f;
    *temp_c = (float)raw_t * 200.0f / 1048576.0f - 50.0f;
    return true;
}

static uint16_t u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t s16_le(const uint8_t *p)
{
    return (int16_t)u16_le(p);
}

static bool bmp280_try_init_addr(uint8_t addr)
{
    uint8_t reg = 0xD0;
    uint8_t id = 0;
    if (i2c_write_read(addr, &reg, 1, &id, 1) != ESP_OK || id != BMP280_CHIP_ID) {
        return false;
    }
    reg = 0x88;
    uint8_t cal[24] = {0};
    if (i2c_write_read(addr, &reg, 1, cal, sizeof(cal)) != ESP_OK) {
        return false;
    }

    s_bmp280.dig_t1 = u16_le(&cal[0]);
    s_bmp280.dig_t2 = s16_le(&cal[2]);
    s_bmp280.dig_t3 = s16_le(&cal[4]);
    s_bmp280.dig_p1 = u16_le(&cal[6]);
    s_bmp280.dig_p2 = s16_le(&cal[8]);
    s_bmp280.dig_p3 = s16_le(&cal[10]);
    s_bmp280.dig_p4 = s16_le(&cal[12]);
    s_bmp280.dig_p5 = s16_le(&cal[14]);
    s_bmp280.dig_p6 = s16_le(&cal[16]);
    s_bmp280.dig_p7 = s16_le(&cal[18]);
    s_bmp280.dig_p8 = s16_le(&cal[20]);
    s_bmp280.dig_p9 = s16_le(&cal[22]);
    s_bmp280.addr = addr;
    s_bmp280.present = true;
    uint8_t cfg[] = {0xF5, 0x00};
    i2c_write_bytes(addr, cfg, sizeof(cfg));
    return true;
}

static bool bmp280_init(void)
{
    memset(&s_bmp280, 0, sizeof(s_bmp280));
    return bmp280_try_init_addr(BMP280_ADDR_0) || bmp280_try_init_addr(BMP280_ADDR_1);
}

static bool bmp280_read(float *temp_c, float *pressure_pa)
{
    if (!s_bmp280.present) return false;
    uint8_t ctrl[] = {0xF4, 0x25};
    if (i2c_write_bytes(s_bmp280.addr, ctrl, sizeof(ctrl)) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t reg = 0xF7;
    uint8_t data[6] = {0};
    if (i2c_write_read(s_bmp280.addr, &reg, 1, data, sizeof(data)) != ESP_OK) return false;

    int32_t adc_p = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | ((int32_t)data[2] >> 4);
    int32_t adc_t = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | ((int32_t)data[5] >> 4);
    int32_t var1 = ((((adc_t >> 3) - ((int32_t)s_bmp280.dig_t1 << 1))) *
                    ((int32_t)s_bmp280.dig_t2)) >> 11;
    int32_t var2 = (((((adc_t >> 4) - ((int32_t)s_bmp280.dig_t1)) *
                      ((adc_t >> 4) - ((int32_t)s_bmp280.dig_t1))) >> 12) *
                    ((int32_t)s_bmp280.dig_t3)) >> 14;
    int32_t t_fine = var1 + var2;
    int32_t t = (t_fine * 5 + 128) >> 8;

    int64_t p_var1 = (int64_t)t_fine - 128000;
    int64_t p_var2 = p_var1 * p_var1 * (int64_t)s_bmp280.dig_p6;
    p_var2 += (p_var1 * (int64_t)s_bmp280.dig_p5) << 17;
    p_var2 += ((int64_t)s_bmp280.dig_p4) << 35;
    p_var1 = ((p_var1 * p_var1 * (int64_t)s_bmp280.dig_p3) >> 8) +
             ((p_var1 * (int64_t)s_bmp280.dig_p2) << 12);
    p_var1 = (((((int64_t)1) << 47) + p_var1)) * ((int64_t)s_bmp280.dig_p1) >> 33;
    if (p_var1 == 0) return false;

    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - p_var2) * 3125) / p_var1;
    p_var1 = (((int64_t)s_bmp280.dig_p9) * (p >> 13) * (p >> 13)) >> 25;
    p_var2 = (((int64_t)s_bmp280.dig_p8) * p) >> 19;
    p = ((p + p_var1 + p_var2) >> 8) + (((int64_t)s_bmp280.dig_p7) << 4);

    *temp_c = (float)t / 100.0f;
    *pressure_pa = (float)p / 256.0f;
    return true;
}

static void rain_gpio_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RAIN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void rain_task(void *arg)
{
    (void)arg;
    int raw_level = gpio_get_level(RAIN_GPIO);
    int stable_level = raw_level;
    int candidate_level = raw_level;
    int64_t candidate_since_ms = millis_i64();
    int64_t last_tip_ms = -RAIN_LOCKOUT_MS;

    for (;;) {
        int64_t ms = millis_i64();
        raw_level = gpio_get_level(RAIN_GPIO);
        if (raw_level != candidate_level) {
            candidate_level = raw_level;
            candidate_since_ms = ms;
        }
        if (candidate_level != stable_level && (ms - candidate_since_ms) >= RAIN_DEBOUNCE_MS) {
            int old_stable = stable_level;
            stable_level = candidate_level;
            if (old_stable == 1 && stable_level == 0 && (ms - last_tip_ms) >= RAIN_LOCKOUT_MS) {
                xSemaphoreTake(s_rain_mutex, portMAX_DELAY);
                s_rain_tip_count++;
                (void)rain_store_count();
                uint32_t tips = s_rain_tip_count;
                xSemaphoreGive(s_rain_mutex);
                last_tip_ms = ms;
                ESP_LOGI(TAG, "RAIN TIP total=%" PRIu32 " mm=%.4f",
                         tips, (double)tips * RAIN_MM_PER_TIP);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RAIN_SAMPLE_PERIOD_MS));
    }
}

static void rain_snapshot(uint32_t *total, uint32_t *delta, float *mm_total)
{
    xSemaphoreTake(s_rain_mutex, portMAX_DELAY);
    *total = s_rain_tip_count;
    *delta = s_rain_tip_count - s_rain_last_reported;
    *mm_total = (float)s_rain_tip_count * RAIN_MM_PER_TIP;
    xSemaphoreGive(s_rain_mutex);
}

static void ensure_peer(const uint8_t *mac)
{
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
        peer.channel = 0;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = false;
        esp_err_t err = esp_now_add_peer(&peer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "add_peer failed %02X:%02X:%02X:%02X:%02X:%02X: %s",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     esp_err_to_name(err));
            return;
        }
        esp_now_rate_config_t rc = {
            .phymode = WIFI_PHY_MODE_11B,
            .rate = WIFI_PHY_RATE_1M_L,
            .ersu = false,
            .dcm = false,
        };
        err = esp_now_set_peer_rate_config(mac, &rc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "peer rate config failed: %s", esp_err_to_name(err));
        }
    }
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) s_ack_status_fail++;
    xSemaphoreGive(s_tx_done);
}
#else
static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    if (status != ESP_NOW_SEND_SUCCESS) s_ack_status_fail++;
    xSemaphoreGive(s_tx_done);
}
#endif

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    rx_item_t item;
    s_diag_recv_cb++;
    item.len = (uint16_t)len;
    if (len == (int)sizeof(sz2_payload_t)) {
        memcpy(&item.payload, data, sizeof(sz2_payload_t));
    } else if (len == (int)sizeof(sz2_payload_v1_t)) {
        memcpy(&item.payload_v1, data, sizeof(sz2_payload_v1_t));
    } else if (len == (int)sizeof(sz2_hello_t)) {
        memcpy(&item.hello, data, sizeof(sz2_hello_t));
    } else {
        memset(&item.payload, 0, sizeof(item.payload));
        memset(&item.hello, 0, sizeof(item.hello));
    }
    memcpy(item.src_mac, info->src_addr, 6);
    item.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_q_dropped++;
    }
}

static uint32_t next_node_slot_offset_s(uint8_t node_id)
{
    uint32_t period = MASTER_NODE_PERIOD_SEC;
    uint32_t spacing = MASTER_NODE_SLOT_SPACING_SEC;
    if (period == 0) period = 1200;
    if (spacing == 0) spacing = 1;

    uint32_t target = ((uint32_t)node_id * spacing) % period;
    if (target == 0) target = spacing < period ? spacing : 0;

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t phase = now_s % period;
    uint32_t target_abs = target;
    if (target_abs <= phase + MASTER_NODE_SLOT_GUARD_SEC) {
        target_abs += period;
    }
    return target_abs - phase;
}

static bool send_ack(const uint8_t *dest, uint8_t node_id, uint32_t boot_id, uint32_t seq)
{
    sz2_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.protocol_version = SZ2_PROTO_VERSION;
    ack.packet_type = SZ2_PKT_ACK;
    ack.node_id = node_id;
    ack.boot_id = boot_id;
    ack.ack_last_seq = seq;
    ack.clear_buffer_until_seq = seq;
    ack.clock_correction_s = (int32_t)current_epoch_s();
    ack.next_slot_offset_s = next_node_slot_offset_s(node_id);

    ensure_peer(dest);
    for (;;) {
        esp_err_t err = esp_now_send(dest, (const uint8_t *)&ack, sizeof(ack));
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(MASTER_SEND_TIMEOUT_MS));
            ESP_LOGI(TAG, "ACK node=%u seq=%u next_slot_s=%u",
                     node_id, (unsigned)seq, (unsigned)ack.next_slot_offset_s);
            return true;
        }
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(MASTER_NO_MEM_RETRY_MS));
            continue;
        }
        ESP_LOGW(TAG, "ACK send failed node=%u seq=%u: %s",
                 node_id, (unsigned)seq, esp_err_to_name(err));
        return false;
    }
}

static bool send_sync_ack(const uint8_t *dest, uint8_t node_id, uint32_t boot_id, uint32_t hello_seq)
{
    sz2_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.protocol_version = SZ2_PROTO_VERSION;
    ack.packet_type = SZ2_PKT_ACK;
    ack.node_id = node_id;
    ack.boot_id = boot_id;
    ack.ack_last_seq = hello_seq;
    ack.clear_buffer_until_seq = 0;
    ack.clock_correction_s = (int32_t)current_epoch_s();
    ack.next_slot_offset_s = next_node_slot_offset_s(node_id);

    ensure_peer(dest);
    for (;;) {
        esp_err_t err = esp_now_send(dest, (const uint8_t *)&ack, sizeof(ack));
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(MASTER_SEND_TIMEOUT_MS));
            ESP_LOGI(TAG, "SYNC_ACK node=%u hello_seq=%u next_slot_s=%u",
                     node_id, (unsigned)hello_seq, (unsigned)ack.next_slot_offset_s);
            return true;
        }
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(MASTER_NO_MEM_RETRY_MS));
            continue;
        }
        ESP_LOGW(TAG, "SYNC_ACK send failed node=%u hello_seq=%u: %s",
                 node_id, (unsigned)hello_seq, esp_err_to_name(err));
        return false;
    }
}

static node_state_t *node_get(uint8_t node_id, const uint8_t *mac)
{
    for (int i = 0; i < MASTER_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].node_id == node_id) {
            if (memcmp(s_nodes[i].mac, mac, 6) != 0) {
                ESP_LOGW(TAG, "NODE_TAKEOVER node=%u old=%02X:%02X:%02X:%02X:%02X:%02X new=%02X:%02X:%02X:%02X:%02X:%02X; state reset",
                         node_id, s_nodes[i].mac[0], s_nodes[i].mac[1], s_nodes[i].mac[2],
                         s_nodes[i].mac[3], s_nodes[i].mac[4], s_nodes[i].mac[5],
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                memset(&s_nodes[i], 0, sizeof(s_nodes[i]));
                s_nodes[i].in_use = true;
                s_nodes[i].node_id = node_id;
                memcpy(s_nodes[i].mac, mac, 6);
                s_nodes[i].rssi_min = 127;
                s_nodes[i].rssi_max = -128;
            }
            return &s_nodes[i];
        }
    }
    for (int i = 0; i < MASTER_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) {
            node_state_t *n = &s_nodes[i];
            memset(n, 0, sizeof(*n));
            n->in_use = true;
            n->node_id = node_id;
            memcpy(n->mac, mac, 6);
            n->rssi_min = 127;
            n->rssi_max = -128;
            return n;
        }
    }
    s_registry_full_noack++;
    ESP_LOGE(TAG, "NODE_REGISTRY_FULL no-ACK node=%u count=%u", node_id,
             (unsigned)s_registry_full_noack);
    return NULL;
}

static bool __attribute__((unused)) cloud_record_enabled(const master_record_t *rec)
{
#if MASTER_CLOUD_MODE && !MASTER_CLOUD_UPLOAD_MASTER
    return rec->type != REC_TYPE_MASTER;
#else
    (void)rec;
    return true;
#endif
}

static record_store_result_t cloud_enqueue_record(master_record_t *rec)
{
#if MASTER_CLOUD_MODE
    if (!cloud_record_enabled(rec)) {
        ESP_LOGI(TAG, "CLOUD_SKIP type=%u unit=%u seq=%u", rec->type, rec->unit_id, (unsigned)rec->seq);
        return RECORD_STORE_SKIPPED;
    }
    if (!backup_enqueue(rec)) {
        s_cloud_drop++;
        ESP_LOGE(TAG, "DURABLE_ENQUEUE_FAILED type=%u unit=%u seq=%u",
                 rec->type, rec->unit_id, (unsigned)rec->seq);
        return RECORD_STORE_FAILED;
    }
    return RECORD_STORE_DURABLE;
#else
    ESP_LOGI(TAG, "CLOUD_OFF type=%u unit=%u seq=%u", rec->type, rec->unit_id, (unsigned)rec->seq);
    return RECORD_STORE_SKIPPED;
#endif
}

static void node_record_from_payload(master_record_t *rec, const sz2_payload_t *p,
                                     const rx_item_t *item, uint32_t gap)
{
    memset(rec, 0, sizeof(*rec));
    rec->type = REC_TYPE_NODE;
    rec->unit_id = p->node_id;
    rec->valid_mask = 0;
    rec->sensor_error_mask = p->sensor_error_mask;
    if (isfinite(p->weight)) rec->valid_mask |= REC_VALID_HX711;
    if (isfinite(p->temp)) rec->valid_mask |= REC_VALID_TEMP;
    rec->rssi = item->rssi;
    memcpy(rec->src_mac, item->src_mac, sizeof(rec->src_mac));
    rec->seq = p->seq;
    rec->boot_id = p->boot_id;
    rec->rx_ms = millis_u32();
    rec->ts = p->send_timestamp_ms >= MASTER_VALID_EPOCH_MIN ?
              p->send_timestamp_ms : current_epoch_s();
    rec->vbat_mv = p->vbat;
    rec->gap = gap;
    if (rec->valid_mask & REC_VALID_HX711) {
        rec->weight_raw = (int32_t)lrintf(p->weight);
    }
    rec->temp_c = p->temp;
}

static const char *hx711_payload_status(float weight, uint32_t error_mask)
{
    if (!isfinite(weight)) {
        return "HX711_ERR";
    }
    if (error_mask & SZ2_SENSOR_ERR_HX711) {
        return "HX711_FAULT";
    }
    if (fabsf(weight) <= 0.5f) {
        return "HX711_ZERO_RAW_SUSPECT";
    }
    return "HX711_OK";
}

static const char *ds18b20_payload_status(float temp, uint32_t error_mask)
{
    return isfinite(temp) && !(error_mask & SZ2_SENSOR_ERR_NODE_DS18B20) ?
           "DS18B20_OK" : "DS18B20_FAULT";
}

static void print_node_stat(const node_state_t *n, int64_t now_us)
{
    int rssi_avg = n->rssi_n ? (int)(n->rssi_sum / (int64_t)n->rssi_n) : 0;
    int age_s = n->last_rx_us ? (int)((now_us - n->last_rx_us) / 1000000) : -1;
    ESP_LOGI(TAG, "NSTAT node=%u last_seq=%u new=%u dup=%u miss=%u crc=%u "
                  "ack=%u ackfail=%u resets=%u rssi_avg=%d rssi_min=%ld rssi_max=%ld age_s=%d",
             n->node_id, (unsigned)n->last_seq, (unsigned)n->rx_new, (unsigned)n->rx_dup,
             (unsigned)n->miss_cum, (unsigned)n->crc_err, (unsigned)n->ack_sent,
             (unsigned)n->ack_txfail, (unsigned)n->resets, rssi_avg,
             (long)n->rssi_min, (long)n->rssi_max, age_s);
}

static void rx_consumer_task(void *arg)
{
    (void)arg;
    rx_item_t item;
    int64_t last_stat_us = esp_timer_get_time();

    for (;;) {
        bool got = xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE;
        if (got) {
            if (item.len == (uint16_t)sizeof(sz2_hello_t)) {
                sz2_hello_t *h = &item.hello;
                if (h->protocol_version != SZ2_PROTO_VERSION || h->packet_type != SZ2_PKT_HELLO) {
                    s_diag_ver_bad++;
                    continue;
                }
                node_state_t *n = node_get(h->node_id, item.src_mac);
                if (n == NULL) continue;
                if (sz2_hello_crc(h) != h->crc) {
                    n->crc_err++;
                    continue;
                }

                if (!n->have_boot_id) {
                    n->boot_id = h->boot_id;
                    n->have_boot_id = true;
                } else if (h->boot_id != n->boot_id) {
                    n->boot_id = h->boot_id;
                    n->have_seq = false;
                    n->resets++;
                }
                if (memcmp(n->mac, item.src_mac, 6) != 0) {
                    memcpy(n->mac, item.src_mac, 6);
                }
                n->last_rx_us = esp_timer_get_time();
                if (item.rssi < n->rssi_min) n->rssi_min = item.rssi;
                if (item.rssi > n->rssi_max) n->rssi_max = item.rssi;
                n->rssi_sum += item.rssi;
                n->rssi_n++;

                uint32_t fail_before = s_ack_status_fail;
                if (send_sync_ack(item.src_mac, h->node_id, h->boot_id, h->hello_seq)) {
                    n->ack_sent++;
                    if (s_ack_status_fail != fail_before) n->ack_txfail++;
                }
                ESP_LOGI(TAG, "HELLO node=%u hello_seq=%u boot=0x%08X pending=%u next_seq=%u wake=%u flags=0x%X rssi=%d",
                         h->node_id, (unsigned)h->hello_seq, (unsigned)h->boot_id,
                         (unsigned)h->pending_count, (unsigned)h->next_seq,
                         (unsigned)h->wake_period_s, (unsigned)h->flags, (int)item.rssi);
                continue;
            }
            bool legacy_v1 = item.len == (uint16_t)sizeof(sz2_payload_v1_t);
            if (item.len != (uint16_t)sizeof(sz2_payload_t) && !legacy_v1) {
                s_diag_len_bad++;
                continue;
            }
            sz2_payload_t legacy_normalized;
            sz2_payload_t *p = &item.payload;
            if (legacy_v1) {
                const sz2_payload_v1_t *old = &item.payload_v1;
                if (old->protocol_version != SZ2_PROTO_VERSION_V1 || old->packet_type != SZ2_PKT_DATA ||
                    sz2_payload_v1_crc(old) != old->crc) {
                    s_diag_ver_bad++;
                    continue;
                }
                memset(&legacy_normalized, 0, sizeof(legacy_normalized));
                legacy_normalized.protocol_version = SZ2_PROTO_VERSION;
                legacy_normalized.packet_type = old->packet_type;
                legacy_normalized.node_id = old->node_id;
                memcpy(legacy_normalized.node_mac, old->node_mac, sizeof(old->node_mac));
                legacy_normalized.boot_id=old->boot_id; legacy_normalized.seq=old->seq;
                legacy_normalized.packet_idx=old->packet_idx; legacy_normalized.total_packets=old->total_packets;
                legacy_normalized.send_timestamp_ms=old->send_timestamp_ms;
                legacy_normalized.weight=old->weight; legacy_normalized.temp=old->temp;
                legacy_normalized.vbat=old->vbat; /* error mask defaults to zero */
                p = &legacy_normalized;
                ESP_LOGI(TAG, "RX legacy v1 DATA accepted node=%u seq=%u", p->node_id, (unsigned)p->seq);
            } else if (p->protocol_version != SZ2_PROTO_VERSION || p->packet_type != SZ2_PKT_DATA) {
                s_diag_ver_bad++;
                continue;
            }
            node_state_t *n = node_get(p->node_id, item.src_mac);
            if (n == NULL) continue;
            if (!legacy_v1 && sz2_payload_crc(p) != p->crc) {
                n->crc_err++;
                continue;
            }

            if (memcmp(n->mac, item.src_mac, 6) != 0) {
                memcpy(n->mac, item.src_mac, 6);
            }

            n->rx_valid++;
            n->last_rx_us = esp_timer_get_time();
            if (item.rssi < n->rssi_min) n->rssi_min = item.rssi;
            if (item.rssi > n->rssi_max) n->rssi_max = item.rssi;
            n->rssi_sum += item.rssi;
            n->rssi_n++;
            n->have_measurement = true;
            n->last_weight_valid = isfinite(p->weight);
            if (n->last_weight_valid) n->last_weight_raw = (int32_t)lrintf(p->weight);
            n->last_temp_valid = isfinite(p->temp);
            if (n->last_temp_valid) n->last_temp_c = p->temp;
            n->last_vbat_mv = p->vbat;
            n->last_sensor_error_mask = p->sensor_error_mask;
            n->last_rssi = item.rssi;
            uint32_t gap = 0;
            bool boot_changed = n->have_boot_id && p->boot_id != n->boot_id;
            bool is_new = boot_changed || !n->have_seq || p->seq > n->last_seq;
            if (is_new && !boot_changed && n->have_seq) {
                gap = p->seq - n->last_seq - 1;
            }

            bool accepted = true;
            if (is_new) {
                master_record_t rec;
                node_record_from_payload(&rec, p, &item, gap);
                record_store_result_t store_result = cloud_enqueue_record(&rec);
                accepted = store_result != RECORD_STORE_FAILED;
                if (accepted) {
                    if (!n->have_boot_id) {
                        n->have_boot_id = true;
                    } else if (boot_changed) {
                        n->resets++;
                    }
                    n->boot_id = p->boot_id;
                    n->have_seq = true;
                    n->last_seq = p->seq;
                    n->miss_cum += gap;
                    n->rx_new++;
                } else {
                    ESP_LOGE(TAG, "RX_NOT_ACKED node=%u seq=%u reason=durable_persist_failed",
                             p->node_id, (unsigned)p->seq);
                }
            } else {
                n->rx_dup++;
            }

            if (!is_new || accepted) {
                uint32_t fail_before = s_ack_status_fail;
                if (send_ack(item.src_mac, p->node_id, p->boot_id, p->seq)) {
                    n->ack_sent++;
                    if (s_ack_status_fail != fail_before) n->ack_txfail++;
                }
            }

            ESP_LOGI(TAG, "RX node=%u seq=%u kind=%s gap=%u rssi=%d "
                          "weight=%.0f(%s) temp=%.2f(%s) vbat=%u",
                     p->node_id, (unsigned)p->seq, is_new ? "NEW" : "DUP",
                     (unsigned)gap, (int)item.rssi, (double)p->weight,
                     hx711_payload_status(p->weight, p->sensor_error_mask),
                     (double)p->temp, ds18b20_payload_status(p->temp, p->sensor_error_mask),
                     (unsigned)p->vbat);
            if (p->sensor_error_mask & SZ2_SENSOR_ERR_HX711) {
                ESP_LOGW(TAG, "SENSOR_WARN node=%u seq=%u sensor=HX711 status=TIMEOUT kind=%s rssi=%d",
                         p->node_id, (unsigned)p->seq, is_new ? "NEW" : "DUP",
                         (int)item.rssi);
            } else if (fabsf(p->weight) <= 0.5f) {
                ESP_LOGW(TAG, "SENSOR_WARN node=%u seq=%u sensor=HX711 status=ZERO_RAW_SUSPECT "
                              "weight=%.0f kind=%s rssi=%d",
                         p->node_id, (unsigned)p->seq, (double)p->weight,
                         is_new ? "NEW" : "DUP", (int)item.rssi);
            }
            if (p->sensor_error_mask & SZ2_SENSOR_ERR_NODE_DS18B20 || !isfinite(p->temp)) {
                ESP_LOGW(TAG, "SENSOR_WARN node=%u seq=%u sensor=DS18B20 status=INVALID kind=%s rssi=%d",
                         p->node_id, (unsigned)p->seq, is_new ? "NEW" : "DUP",
                         (int)item.rssi);
            }

        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_stat_us >= (int64_t)MASTER_STAT_PERIOD_MS * 1000) {
            last_stat_us = now_us;
            uint8_t ch = 0;
            wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&ch, &sec);
            for (int i = 0; i < MASTER_MAX_NODES; i++) {
                if (s_nodes[i].in_use) {
                    print_node_stat(&s_nodes[i], now_us);
                }
            }
#if MASTER_WIFI_ENABLED
            const char *wifi_state = s_wifi_ip ? "IP" : (s_wifi_assoc ? "ASSOC" : "DOWN");
#else
            const char *wifi_state = "OFF";
#endif
            ESP_LOGI(TAG, "MSTAT uptime_s=%u rxcb=%u q_drop=%u len_bad=%u ver_bad=%u "
                          "wifi=%s ch=%u expected_ch=%u cloud_ok=%u cloud_fail=%u cloud_skip=%u cloud_drop=%u "
                          "backup_pending=%u backup_enq=%u backup_up=%u backup_bad=%u backup_full=%u",
                     (unsigned)(now_us / 1000000), (unsigned)s_diag_recv_cb,
                     (unsigned)s_q_dropped, (unsigned)s_diag_len_bad,
                     (unsigned)s_diag_ver_bad, wifi_state, (unsigned)ch,
                     (unsigned)MASTER_ESPNOW_CHANNEL,
                     (unsigned)s_cloud_ok, (unsigned)s_cloud_fail, (unsigned)s_cloud_skip,
                     (unsigned)s_cloud_drop,
                     (unsigned)backup_count_unlocked(), (unsigned)s_backup_enqueued,
                     (unsigned)s_backup_uploaded, (unsigned)s_backup_bad,
                     (unsigned)s_backup_full);
        }
    }
}

static void check_wifi_channel(const char *where)
{
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_get_channel(&ch, &sec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CHANNEL_CHECK where=%s failed=%s expected=%u",
                 where, esp_err_to_name(err), (unsigned)MASTER_ESPNOW_CHANNEL);
        return;
    }
    if (ch != MASTER_ESPNOW_CHANNEL) {
        ESP_LOGE(TAG, "CHANNEL_MISMATCH where=%s actual=%u expected=%u cloud=%d",
                 where, (unsigned)ch, (unsigned)MASTER_ESPNOW_CHANNEL, MASTER_CLOUD_MODE);
    } else {
        ESP_LOGI(TAG, "CHANNEL_OK where=%s actual=%u expected=%u",
                 where, (unsigned)ch, (unsigned)MASTER_ESPNOW_CHANNEL);
    }
}

#if MASTER_WIFI_SCAN_DIAG
static void wifi_scan_diag_once(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    const char *target = MASTER_TEST_SSID;
    ESP_LOGI(TAG, "TEST_SCAN start target_ssid=%s max_aps=%u",
             target[0] ? target : "*", (unsigned)MASTER_SCAN_MAX_APS);

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TEST_SCAN failed start err=%s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_total = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&ap_total));

    uint16_t ap_count = ap_total;
    if (ap_count > MASTER_SCAN_MAX_APS) {
        ap_count = MASTER_SCAN_MAX_APS;
    }

    wifi_ap_record_t *aps = NULL;
    if (ap_count > 0) {
        aps = calloc(ap_count, sizeof(*aps));
        if (aps == NULL) {
            ESP_LOGE(TAG, "TEST_SCAN no_mem count=%u bytes=%u",
                     (unsigned)ap_count, (unsigned)(ap_count * sizeof(*aps)));
            return;
        }
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, aps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TEST_SCAN failed records err=%s total=%u",
                 esp_err_to_name(err), (unsigned)ap_total);
        free(aps);
        return;
    }

    bool target_found = false;
    ESP_LOGI(TAG, "TEST_SCAN result total=%u printed=%u expected_esnow_ch=%u",
             (unsigned)ap_total, (unsigned)ap_count, (unsigned)MASTER_ESPNOW_CHANNEL);
    for (uint16_t i = 0; i < ap_count; i++) {
        const bool match = target[0] && strcmp((const char *)aps[i].ssid, target) == 0;
        target_found = target_found || match;
        ESP_LOGI(TAG, "TEST_AP %02u ssid=\"%s\" bssid=%02X:%02X:%02X:%02X:%02X:%02X "
                      "ch=%u rssi=%d auth=%u%s",
                 (unsigned)i, (const char *)aps[i].ssid,
                 aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                 aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                 (unsigned)aps[i].primary, (int)aps[i].rssi,
                 (unsigned)aps[i].authmode, match ? " MATCH" : "");
    }
    free(aps);
    if (target[0] && !target_found) {
        ESP_LOGW(TAG, "TEST_SCAN target_not_found ssid=\"%s\"", target);
    }
}
#endif

#if MASTER_CLOUD_MODE
typedef struct {
    char *buf;
    int cap;
    int len;
} http_resp_buf_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != NULL) {
        http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;
        int room = rb->cap - rb->len - 1;
        if (room <= 0) return ESP_OK;
        int copy = evt->data_len < room ? evt->data_len : room;
        memcpy(rb->buf + rb->len, evt->data, copy);
        rb->len += copy;
        rb->buf[rb->len] = '\0';
    }
    return ESP_OK;
}

static bool http_json(const char *url, const char *body, esp_http_client_method_t method,
                      const char *bearer_token, char *resp, int resp_cap, int *status_out)
{
    http_resp_buf_t rb = {.buf = resp, .cap = resp_cap, .len = 0};
    if (resp_cap > 0) resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = MASTER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_cb,
        .user_data = &rb,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return false;
    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        char auth_header[2200];
        int n = snprintf(auth_header, sizeof(auth_header), "Bearer %s", bearer_token);
        if (n <= 0 || n >= (int)sizeof(auth_header)) {
            esp_http_client_cleanup(client);
            return false;
        }
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 0 && err != ESP_OK) status = -1;
    if (status_out) *status_out = status;
    esp_http_client_cleanup(client);
    return err == ESP_OK && status >= 200 && status < 300;
}

static bool firebase_token_valid(void)
{
    return s_firebase_token[0] != '\0' &&
           esp_timer_get_time() < s_firebase_token_expire_us;
}

static bool firebase_auth_refresh(void)
{
    char url[256];
    int u = snprintf(url, sizeof(url),
                     "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=%s",
                     MASTER_FIREBASE_API_KEY);
    if (u <= 0 || u >= (int)sizeof(url)) return false;

    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"email\":\"%s\",\"password\":\"%s\",\"returnSecureToken\":true}",
                     MASTER_FIREBASE_EMAIL, MASTER_FIREBASE_PASSWORD);
    if (n <= 0 || n >= (int)sizeof(body)) return false;

    char resp[4096];
    int status = 0;
    if (!http_json(url, body, HTTP_METHOD_POST, NULL, resp, sizeof(resp), &status)) {
        ESP_LOGW(TAG, "Firebase auth failed status=%d", status);
        return false;
    }
    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) return false;
    const cJSON *id_token = cJSON_GetObjectItemCaseSensitive(root, "idToken");
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expiresIn");
    if (!cJSON_IsString(id_token) || id_token->valuestring == NULL) {
        cJSON_Delete(root);
        return false;
    }
    snprintf(s_firebase_token, sizeof(s_firebase_token), "%s", id_token->valuestring);
    int expires_s = 3600;
    if (cJSON_IsString(expires) && expires->valuestring != NULL) {
        expires_s = atoi(expires->valuestring);
    }
    if (expires_s > MASTER_FIREBASE_TOKEN_MARGIN_S) {
        expires_s -= MASTER_FIREBASE_TOKEN_MARGIN_S;
    }
    s_firebase_token_expire_us = esp_timer_get_time() + (int64_t)expires_s * 1000000;
    cJSON_Delete(root);
    return true;
}

static int json_add(char *buf, int cap, int off, const char *fmt, ...)
{
    if (off < 0 || off >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0 || off + n >= cap) return -1;
    return off + n;
}

static bool build_record_json(const master_record_t *rec, char *body, int body_cap)
{
    uint32_t ts = rec->ts ? rec->ts : current_epoch_s();
    if (ts == 0) return false;

    int n = json_add(body, body_cap, 0, "{\"ts\":%u,\"rx_ms\":%u,\"unit_id\":%u,\"seq\":%u,\"sensor_error_mask\":%u",
                     (unsigned)ts, (unsigned)rec->rx_ms, rec->unit_id, (unsigned)rec->seq,
                     (unsigned)rec->sensor_error_mask);
    if (rec->type == REC_TYPE_NODE) {
        n = json_add(body, body_cap, n,
                     ",\"boot_id\":\"0x%08X\",\"vbat_mv\":%u,"
                     "\"rssi\":%d,\"gap\":%u,"
                     "\"src_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
                     (unsigned)rec->boot_id, (unsigned)rec->vbat_mv,
                     (int)rec->rssi, (unsigned)rec->gap,
                     rec->src_mac[0], rec->src_mac[1], rec->src_mac[2],
                     rec->src_mac[3], rec->src_mac[4], rec->src_mac[5]);
        if (rec->valid_mask & REC_VALID_HX711) {
            n = json_add(body, body_cap, n, ",\"weight_raw\":%ld,\"hx711_ok\":true",
                         (long)rec->weight_raw);
        } else {
            n = json_add(body, body_cap, n, ",\"hx711_ok\":false");
        }
        if (rec->valid_mask & REC_VALID_TEMP) {
            n = json_add(body, body_cap, n, ",\"temp_c\":%.3f", (double)rec->temp_c);
        }
    } else {
        n = json_add(body, body_cap, n,
                     ",\"rain_tips_total\":%u,\"rain_mm_total\":%.4f,"
                     "\"rain_tips_delta\":%u,\"rain_mm_delta\":%.4f",
                     (unsigned)rec->rain_tips_total, (double)rec->rain_mm_total,
                     (unsigned)rec->rain_tips_delta, (double)rec->rain_mm_delta);
        if (rec->valid_mask & REC_VALID_HX711) {
            n = json_add(body, body_cap, n, ",\"hx711_raw\":%ld", (long)rec->hx711_raw);
        }
        if (rec->valid_mask & REC_VALID_DS0) {
            n = json_add(body, body_cap, n, ",\"ds0_hive_c\":%.3f", (double)rec->ds0_hive_c);
        }
        if (rec->valid_mask & REC_VALID_DS1) {
            n = json_add(body, body_cap, n, ",\"ds1_scale_c\":%.3f", (double)rec->ds1_scale_c);
        }
        if (rec->valid_mask & REC_VALID_AHT) {
            n = json_add(body, body_cap, n, ",\"aht20_t_c\":%.3f,\"aht20_rh\":%.3f",
                         (double)rec->aht20_t_c, (double)rec->aht20_rh);
        }
        if (rec->valid_mask & REC_VALID_BMP) {
            n = json_add(body, body_cap, n, ",\"bmp280_t_c\":%.3f,\"bmp280_pa\":%.1f",
                         (double)rec->bmp280_t_c, (double)rec->bmp280_pa);
        }
    }
    n = json_add(body, body_cap, n, "}");
    return n > 0;
}

static bool build_experiment_node_json(const master_record_t *rec, char *body, int body_cap)
{
    uint32_t ts = rec->ts ? rec->ts : current_epoch_s();
    if (ts == 0 || rec->type != REC_TYPE_NODE) return false;

    int n = json_add(body, body_cap, 0,
                     "{\"ts\":%u,\"rx_ms\":%u,\"node_id\":%u,\"seq\":%u,"
                     "\"boot_id\":\"0x%08X\",\"packet_idx\":0,\"vbat_mv\":%u,"
                     "\"weight\":%ld,\"temp_c\":%.3f,\"rssi\":%d,"
                     "\"kind\":\"NEW\",\"gap\":%u,\"sensor_error_mask\":%u,"
                     "\"src_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                     (unsigned)ts, (unsigned)rec->rx_ms, rec->unit_id,
                     (unsigned)rec->seq, (unsigned)rec->boot_id,
                     (unsigned)rec->vbat_mv, (long)rec->weight_raw,
                     (double)rec->temp_c, (int)rec->rssi, (unsigned)rec->gap,
                     (unsigned)rec->sensor_error_mask,
                     rec->src_mac[0], rec->src_mac[1], rec->src_mac[2],
                     rec->src_mac[3], rec->src_mac[4], rec->src_mac[5]);
    return n > 0;
}

static bool build_hive2_node_json(const master_record_t *rec, char *body, int body_cap)
{
    uint32_t ts = rec->ts ? rec->ts : current_epoch_s();
    if (ts == 0 || rec->type != REC_TYPE_NODE) return false;

    int n = json_add(body, body_cap, 0, "{\"ts\":%u,\"node_id\":%u,\"weight_raw\":%ld,\"sensor_error_mask\":%u",
                     (unsigned)ts, rec->unit_id, (long)rec->weight_raw,
                     (unsigned)rec->sensor_error_mask);
    if (rec->valid_mask & REC_VALID_TEMP) {
        n = json_add(body, body_cap, n, ",\"temp_c\":%.3f", (double)rec->temp_c);
    }
    if (rec->vbat_mv > 0) {
        n = json_add(body, body_cap, n, ",\"vbat_mv\":%u", (unsigned)rec->vbat_mv);
    }
    n = json_add(body, body_cap, n, "}");
    return n > 0;
}

static bool build_hive2_master_rod_json(const master_record_t *rec, char *body, int body_cap)
{
    uint32_t ts = rec->ts ? rec->ts : current_epoch_s();
    if (ts == 0) return false;
    int n = json_add(body, body_cap, 0, "{\"ts\":%u,\"weight_raw\":%ld,\"sensor_error_mask\":%u}",
                     (unsigned)ts, (long)rec->hx711_raw, (unsigned)rec->sensor_error_mask);
    return n > 0;
}

static bool build_hive2_master_env_json(const master_record_t *rec, char *body, int body_cap)
{
    uint32_t ts = rec->ts ? rec->ts : current_epoch_s();
    if (ts == 0) return false;

    int n = json_add(body, body_cap, 0, "{\"ts\":%u,\"rain_mm\":%.4f,\"sensor_error_mask\":%u",
                     (unsigned)ts, (double)rec->rain_mm_delta, (unsigned)rec->sensor_error_mask);
    if (rec->valid_mask & REC_VALID_AHT) {
        n = json_add(body, body_cap, n, ",\"air_t_c\":%.3f,\"air_rh\":%.3f",
                     (double)rec->aht20_t_c, (double)rec->aht20_rh);
    }
    if (rec->valid_mask & REC_VALID_BMP) {
        n = json_add(body, body_cap, n, ",\"bmp_t_c\":%.3f,\"pressure_pa\":%.1f",
                     (double)rec->bmp280_t_c, (double)rec->bmp280_pa);
    }
    n = json_add(body, body_cap, n, "}");
    return n > 0;
}

static bool build_hive2_master_temp_json(const master_record_t *rec, char *body, int body_cap)
{
    uint32_t ts = rec->ts ? rec->ts : current_epoch_s();
    if (ts == 0) return false;
    if (!(rec->valid_mask & REC_VALID_DS0) && !(rec->valid_mask & REC_VALID_DS1)) return false;

    int n = json_add(body, body_cap, 0, "{\"ts\":%u,\"sensor_error_mask\":%u",
                     (unsigned)ts, (unsigned)rec->sensor_error_mask);
    if (rec->valid_mask & REC_VALID_DS0) {
        n = json_add(body, body_cap, n, ",\"hive_c\":%.3f", (double)rec->ds0_hive_c);
    }
    if (rec->valid_mask & REC_VALID_DS1) {
        n = json_add(body, body_cap, n, ",\"scale_c\":%.3f", (double)rec->ds1_scale_c);
    }
    n = json_add(body, body_cap, n, "}");
    return n > 0;
}

static bool cloud_put_json_path(const char *path, const char *body, int *status_out)
{
    char url[2700];
    int u = snprintf(url, sizeof(url), "%s/%s.json?auth=%s",
                     MASTER_FIREBASE_DATABASE_URL, path, s_firebase_token);
    if (u <= 0 || u >= (int)sizeof(url)) return false;

    char resp[256];
    return http_json(url, body, HTTP_METHOD_PUT, NULL, resp, sizeof(resp), status_out);
}

static int master_record_key(char *out, size_t cap, const master_record_t *rec)
{
    /* A már flashben levő, régi firmware-rel készült master rekordok boot_id=0
     * értékűek. Ezeknél tartsuk meg a régi kulcsot, így a backlog újraküldése továbbra
     * idempotens; minden új rekord boot-scoped kulcsot kap. */
    if (rec->boot_id == 0) {
        return snprintf(out, cap, "master_%u", (unsigned)rec->seq);
    }
    return snprintf(out, cap, "master_%08X_%u",
                    (unsigned)rec->boot_id, (unsigned)rec->seq);
}

static bool cloud_record_should_skip(const master_record_t *rec)
{
    if (!cloud_record_enabled(rec)) return true;
    if (MASTER_FIREBASE_ROOT_PATH[0] != '\0') {
        return rec->type != REC_TYPE_NODE;
    }
    return rec->type != REC_TYPE_NODE && rec->type != REC_TYPE_MASTER;
}

static cloud_result_t __attribute__((unused)) cloud_post_hive2_record_once(const master_record_t *rec)
{
    char path[256];
    char body[1024];
    int status = 0;
    bool attempted = false;
    bool ok_all = true;

    if (rec->type == REC_TYPE_NODE) {
        if (!build_hive2_node_json(rec, body, sizeof(body))) return CLOUD_RESULT_RETRY;
        int p = snprintf(path, sizeof(path),
                         "hives/%s/rods/node_%u/readings/%08X_%u",
                         MASTER_FIREBASE_TARGET_HIVE_ID, rec->unit_id,
                         (unsigned)rec->boot_id, (unsigned)rec->seq);
        if (p <= 0 || p >= (int)sizeof(path)) return CLOUD_RESULT_RETRY;
        attempted = true;
        ok_all = cloud_put_json_path(path, body, &status);
    } else {
        char record_key[40];
        int key_len = master_record_key(record_key, sizeof(record_key), rec);
        if (key_len <= 0 || key_len >= (int)sizeof(record_key)) return CLOUD_RESULT_RETRY;
        if (build_hive2_master_rod_json(rec, body, sizeof(body))) {
            int p = snprintf(path, sizeof(path), "hives/%s/rods/master/readings/%s",
                             MASTER_FIREBASE_TARGET_HIVE_ID, record_key);
            if (p <= 0 || p >= (int)sizeof(path)) return CLOUD_RESULT_RETRY;
            attempted = true;
            ok_all = cloud_put_json_path(path, body, &status) && ok_all;
        }
        if (build_hive2_master_env_json(rec, body, sizeof(body))) {
            int p = snprintf(path, sizeof(path), "hives/%s/env/readings/%s",
                             MASTER_FIREBASE_TARGET_HIVE_ID, record_key);
            if (p <= 0 || p >= (int)sizeof(path)) return CLOUD_RESULT_RETRY;
            attempted = true;
            ok_all = cloud_put_json_path(path, body, &status) && ok_all;
        }
        if (build_hive2_master_temp_json(rec, body, sizeof(body))) {
            int p = snprintf(path, sizeof(path), "hives/%s/temp/readings/%s",
                             MASTER_FIREBASE_TARGET_HIVE_ID, record_key);
            if (p <= 0 || p >= (int)sizeof(path)) return CLOUD_RESULT_RETRY;
            attempted = true;
            ok_all = cloud_put_json_path(path, body, &status) && ok_all;
        }
    }

    if (!ok_all) ESP_LOGW(TAG, "cloud PUT failed status=%d type=%u unit=%u seq=%u",
                          status, rec->type, rec->unit_id, (unsigned)rec->seq);
    return attempted && ok_all ? CLOUD_RESULT_OK : CLOUD_RESULT_RETRY;
}

static cloud_result_t cloud_post_record_once(const master_record_t *rec)
{
    if (cloud_record_should_skip(rec)) return CLOUD_RESULT_SKIP;
    if (!s_wifi_ip) return CLOUD_RESULT_RETRY;
    if (esp_timer_get_time() < s_auth_probe_not_before_us) return CLOUD_RESULT_RETRY;
    if (!firebase_token_valid() && !firebase_auth_refresh()) return CLOUD_RESULT_RETRY;

#if MASTER_CLOUD_HIVE2_SCHEMA
    if (MASTER_FIREBASE_ROOT_PATH[0] == '\0') {
        return cloud_post_hive2_record_once(rec);
    }
#endif

    char url[2700];
    int u;
    bool experiment_path = MASTER_FIREBASE_ROOT_PATH[0] != '\0';
    if (experiment_path) {
        if (rec->type != REC_TYPE_NODE) return CLOUD_RESULT_SKIP;
        u = snprintf(url, sizeof(url),
                     "%s/%s/nodes/node_%u/readings/%08X_%u.json?auth=%s",
                     MASTER_FIREBASE_DATABASE_URL, MASTER_FIREBASE_ROOT_PATH,
                     rec->unit_id, (unsigned)rec->boot_id, (unsigned)rec->seq,
                     s_firebase_token);
    } else if (rec->type == REC_TYPE_NODE) {
        u = snprintf(url, sizeof(url),
                     "%s/hives/%s%s/nodes/node_%u/raw/%08X_%u.json?auth=%s",
                     MASTER_FIREBASE_DATABASE_URL, MASTER_FIREBASE_HIVE_ID,
                     MASTER_FIREBASE_HIVE_PATH_SUFFIX, rec->unit_id,
                     (unsigned)rec->boot_id, (unsigned)rec->seq,
                     s_firebase_token);
    } else {
        char record_key[40];
        int key_len = master_record_key(record_key, sizeof(record_key), rec);
        if (key_len <= 0 || key_len >= (int)sizeof(record_key)) return CLOUD_RESULT_RETRY;
        u = snprintf(url, sizeof(url),
                     "%s/hives/%s%s/master/raw/%s.json?auth=%s",
                     MASTER_FIREBASE_DATABASE_URL, MASTER_FIREBASE_HIVE_ID,
                     MASTER_FIREBASE_HIVE_PATH_SUFFIX, record_key,
                     s_firebase_token);
    }
    if (u <= 0 || u >= (int)sizeof(url)) return CLOUD_RESULT_RETRY;

    char body[1024];
    bool body_ok = experiment_path ?
                   build_experiment_node_json(rec, body, sizeof(body)) :
                   build_record_json(rec, body, sizeof(body));
    if (!body_ok) return CLOUD_RESULT_RETRY;
    char resp[256];
    int status = 0;
    bool ok = http_json(url, body, HTTP_METHOD_PUT, NULL, resp, sizeof(resp), &status);
    if (ok) {
        s_auth_401_refresh_used = false;
        return CLOUD_RESULT_OK;
    }
    ESP_LOGW(TAG, "cloud PUT failed status=%d type=%u unit=%u seq=%u",
             status, rec->type, rec->unit_id, (unsigned)rec->seq);
    if (status == 401) {
        if (!s_auth_401_refresh_used) {
            s_auth_401_refresh_used = true;
            s_firebase_token_expire_us = 0; /* a következő kísérlet egyszer frissít */
        } else {
            s_auth_probe_not_before_us = esp_timer_get_time() + 15LL * 60LL * 1000000LL;
            ESP_LOGW(TAG, "Firebase repeated 401; auth probe paused for 15 minutes");
        }
        return CLOUD_RESULT_RETRY;
    }
    return status >= 400 && status < 500 ? CLOUD_RESULT_DROP : CLOUD_RESULT_RETRY;
}

static cloud_result_t cloud_post_record_with_retries(const master_record_t *rec)
{
    if (cloud_record_should_skip(rec)) {
        s_cloud_skip++;
        ESP_LOGW(TAG, "CLOUD_SKIP_INVALID type=%u unit=%u seq=%u valid=0x%02X",
                 rec->type, rec->unit_id, (unsigned)rec->seq, rec->valid_mask);
        return CLOUD_RESULT_SKIP;
    }
    if (!s_wifi_ip) return CLOUD_RESULT_RETRY;

    uint32_t backoff = MASTER_CLOUD_RETRY_BASE_MS;
    for (int attempt = 1; attempt <= MASTER_CLOUD_MAX_ATTEMPTS; attempt++) {
        cloud_result_t result = cloud_post_record_once(rec);
        if (result == CLOUD_RESULT_OK) {
            s_cloud_ok++;
            return CLOUD_RESULT_OK;
        }
        if (result == CLOUD_RESULT_SKIP) {
            s_cloud_skip++;
            return CLOUD_RESULT_SKIP;
        }
        if (result == CLOUD_RESULT_DROP) {
            s_cloud_drop++;
            ESP_LOGW(TAG, "CLOUD_DROP_PERMANENT type=%u unit=%u seq=%u",
                     rec->type, rec->unit_id, (unsigned)rec->seq);
            return CLOUD_RESULT_DROP;
        }
        s_cloud_fail++;
        if (attempt < MASTER_CLOUD_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = backoff * 2U;
            if (backoff > MASTER_CLOUD_RETRY_MAX_MS) backoff = MASTER_CLOUD_RETRY_MAX_MS;
        }
    }
    return CLOUD_RESULT_RETRY;
}

static void sntp_start_once(void)
{
    static bool started = false;
    if (started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    started = true;
}

static void cloud_task(void *arg)
{
    (void)arg;
    master_record_t rec;
    for (;;) {
        if (!backup_peek(&rec)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        cloud_result_t result = cloud_post_record_with_retries(&rec);
        if (result == CLOUD_RESULT_OK || result == CLOUD_RESULT_SKIP || result == CLOUD_RESULT_DROP) {
            if (!backup_pop()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
#endif

#if MASTER_WIFI_ENABLED
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = (const wifi_event_sta_connected_t *)data;
        s_wifi_assoc = true;
        ESP_LOGI(TAG, "WIFI CONNECTED ssid=%s ch=%u", (const char *)ev->ssid, (unsigned)ev->channel);
        if (ev->channel != MASTER_ESPNOW_CHANNEL) {
            ESP_LOGE(TAG, "CHANNEL_MISMATCH where=STA_CONNECTED actual=%u expected=%u",
                     (unsigned)ev->channel, (unsigned)MASTER_ESPNOW_CHANNEL);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_assoc = false;
        s_wifi_ip = false;
        s_disconn_cnt++;
        xSemaphoreGive(s_reconn_sem);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_wifi_ip = true;
        s_got_ip_cnt++;
        s_backoff_ms = MASTER_RECONN_BACKOFF_MIN_MS;
#if MASTER_CLOUD_MODE
        sntp_start_once();
#endif
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);
        ESP_LOGI(TAG, "WIFI GOT_IP ip=" IPSTR " gw=" IPSTR " ch=%u",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), (unsigned)ch);
#if MASTER_WEB_STATUS
        ESP_LOGI(TAG, "LOCAL_STATUS open http://" IPSTR "/",
                 IP2STR(&ev->ip_info.ip));
#endif
        check_wifi_channel("GOT_IP");
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_reconn_sem, portMAX_DELAY);
        if (s_wifi_assoc) continue;
        uint32_t wait_ms = s_backoff_ms + (esp_random() % MASTER_RECONN_JITTER_MS);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        if (!s_wifi_assoc) {
            uint32_t next = s_backoff_ms * 2U;
            s_backoff_ms = next > MASTER_RECONN_BACKOFF_MAX_MS ? MASTER_RECONN_BACKOFF_MAX_MS : next;
            esp_wifi_connect();
        }
    }
}
#endif

#if MASTER_WEB_STATUS
static esp_err_t web_sendf(httpd_req_t *req, const char *fmt, ...)
{
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(line)) return ESP_FAIL;
    return httpd_resp_sendstr_chunk(req, line);
}

static esp_err_t web_status_handler(httpd_req_t *req)
{
    /* This is a best-effort monitoring view. RX is the only writer; a slightly
     * older field is preferable to blocking an ESP-NOW ACK while rendering HTML. */
    node_state_t nodes[MASTER_MAX_NODES];
    memcpy(nodes, s_nodes, sizeof(nodes));
    const int64_t now_us = esp_timer_get_time();
    const int fresh_limit_s = MASTER_NODE_PERIOD_SEC + MASTER_NODE_SLOT_SPACING_SEC +
                              MASTER_NODE_SLOT_GUARD_SEC + 30;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (web_sendf(req,
        "<!doctype html><html><head><meta charset=utf-8><meta http-equiv=refresh content=%u>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Bee Scale master</title><style>body{font:16px system-ui;margin:18px;color:#18221b}"
        "table{border-collapse:collapse;width:100%%;max-width:1000px}th,td{border:1px solid #ccd5ce;"
        "padding:8px;text-align:left}th{background:#edf4ee}.ok{color:#087a35;font-weight:700}"
        ".stale{color:#a24b00;font-weight:700}.wait{color:#5d6670}small{color:#5d6670}</style></head><body>"
        "<h1>Bee Scale master</h1><p>WiFi: <b>%s</b> &middot; uptime: %us &middot; refresh: %us</p>"
        "<table><tr><th>Node</th><th>Allapot</th><th>Utolso vetel</th><th>RSSI</th>"
        "<th>Seq</th><th>Merleg (raw)</th><th>Homerseklet</th><th>VBAT</th>"
        "<th>Hibak (crc/miss/ackfail)</th></tr>",
        (unsigned)MASTER_WEB_REFRESH_SEC, s_wifi_ip ? "csatlakozva" : "nincs IP",
        (unsigned)(now_us / 1000000), (unsigned)MASTER_WEB_REFRESH_SEC) != ESP_OK) {
        return ESP_FAIL;
    }

    int node_count = 0;
    for (int i = 0; i < MASTER_MAX_NODES; i++) {
        const node_state_t *n = &nodes[i];
        if (!n->in_use) continue;
        node_count++;
        int age_s = n->last_rx_us ? (int)((now_us - n->last_rx_us) / 1000000) : -1;
        bool online = age_s >= 0 && age_s <= fresh_limit_s;
        int rssi_avg = n->rssi_n ? (int)(n->rssi_sum / (int64_t)n->rssi_n) : 0;
        const char *state = online ? "ONLINE" : "ELOREGED";
        const char *state_class = online ? "ok" : "stale";
        char age[32];
        char weight[32];
        char temp[32];
        if (age_s < 0) snprintf(age, sizeof(age), "nincs adat");
        else snprintf(age, sizeof(age), "%d s", age_s);
        if (n->have_measurement && n->last_weight_valid) {
            snprintf(weight, sizeof(weight), "%ld%s", (long)n->last_weight_raw,
                     (n->last_sensor_error_mask & SZ2_SENSOR_ERR_HX711) ? " FAULT" : "");
        } else {
            snprintf(weight, sizeof(weight), "--");
        }
        if (n->have_measurement && n->last_temp_valid) {
            snprintf(temp, sizeof(temp), "%.2f C%s", (double)n->last_temp_c,
                     (n->last_sensor_error_mask & SZ2_SENSOR_ERR_NODE_DS18B20) ? " FAULT" : "");
        } else {
            snprintf(temp, sizeof(temp), "--");
        }
        if (web_sendf(req,
            "<tr><td>%u</td><td class=%s>%s</td><td>%s</td><td>%d dBm <small>(atlag)</small></td>"
            "<td>" PRIu32 "</td><td>%s</td><td>%s</td><td>%u mV</td>"
            "<td>" PRIu32 "/" PRIu32 "/" PRIu32 "</td></tr>",
            n->node_id, state_class, state, age, rssi_avg, n->last_seq, weight, temp,
            n->last_vbat_mv, n->crc_err, n->miss_cum, n->ack_txfail) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (node_count == 0) {
        if (web_sendf(req,
            "<tr><td colspan=9 class=wait>Meg nincs bejelentkezett node. Inditsd el a node-ot, "
            "vagy varj a kovetkezo kuldesere.</td></tr>") != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (web_sendf(req,
        "</table><p><small>A lap csak a master helyi memoriaban levo utolso veteli allapotot mutatja; "
        "internet es felho nem szukseges hozza.</small></p></body></html>") != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

static void web_status_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 7168;
    config.max_open_sockets = 2;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LOCAL_STATUS server start failed: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t status_uri = {
        .uri = "/", .method = HTTP_GET, .handler = web_status_handler, .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LOCAL_STATUS URI register failed: %s", esp_err_to_name(err));
        httpd_stop(server);
        return;
    }
    ESP_LOGI(TAG, "LOCAL_STATUS server started (waiting for WiFi IP)");
}
#endif

static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if MASTER_WIFI_ENABLED
    esp_netif_create_default_wifi_sta();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

#if MASTER_WIFI_ENABLED
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", MASTER_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", MASTER_WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
#if MASTER_WIFI_SCAN_DIAG
    wifi_scan_diag_once();
#endif
#if !MASTER_WIFI_ENABLED
    ESP_ERROR_CHECK(esp_wifi_set_channel(MASTER_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    check_wifi_channel("wifi_espnow_init");
#else
    ESP_LOGI(TAG, "WiFi STA mode: AP/router csatorna diktal, expected_ch=%u",
             (unsigned)MASTER_ESPNOW_CHANNEL);
#endif

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    static const uint8_t bc[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ensure_peer(bc);

    uint8_t mac[6] = {0};
    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "master MAC=%02X:%02X:%02X:%02X:%02X:%02X cloud=%d web=%d ch=%u expected_ch=%u",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], MASTER_CLOUD_MODE, MASTER_WEB_STATUS,
             (unsigned)ch, (unsigned)MASTER_ESPNOW_CHANNEL);
}

static void master_measure_task(void *arg)
{
    (void)arg;
    for (;;) {
        master_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.type = REC_TYPE_MASTER;
        rec.unit_id = MASTER_UNIT_ID;
        rec.boot_id = s_master_boot_id;
        rec.seq = master_next_seq();
        rec.rx_ms = millis_u32();
        rec.ts = current_epoch_s();

        /* Egy korábbi hibából csak új mintavételi ciklus elején próbálunk reinitet. */
        if (!s_aht20_present) s_aht20_present = aht20_init();
        if (!s_bmp280.present) (void)bmp280_init();

        int32_t hx_raw = 0;
        bool hx_ok = hx711_read_avg(&hx_raw);
        rec.valid_mask |= REC_VALID_HX711;
        rec.hx711_raw = hx_raw;
        if (!hx_ok) rec.sensor_error_mask |= SZ2_SENSOR_ERR_HX711;

        float ds_t[DS18B20_MAX_SENSORS];
        uint32_t ds_error = 0;
        if (s_ds18b20_count != DS18B20_EXPECTED_COUNT) {
            ds18b20_refresh_search();
        }
        (void)ds18b20_read_all(ds_t, &ds_error);
        rec.sensor_error_mask |= ds_error;
        if (s_ds18b20_count > 0 && isfinite(ds_t[0])) {
            rec.valid_mask |= REC_VALID_DS0;
            rec.ds0_hive_c = ds_t[0];
        }
        if (s_ds18b20_count > 1 && isfinite(ds_t[1])) {
            rec.valid_mask |= REC_VALID_DS1;
            rec.ds1_scale_c = ds_t[1];
        }

        float aht_t = NAN;
        float aht_rh = NAN;
        if (s_aht20_present && aht20_read(&aht_t, &aht_rh)) {
            rec.valid_mask |= REC_VALID_AHT;
            rec.aht20_t_c = aht_t;
            rec.aht20_rh = aht_rh;
        } else {
            rec.sensor_error_mask |= SZ2_SENSOR_ERR_AHT20;
        }

        float bmp_t = NAN;
        float bmp_pa = NAN;
        if (s_bmp280.present && bmp280_read(&bmp_t, &bmp_pa)) {
            rec.valid_mask |= REC_VALID_BMP;
            rec.bmp280_t_c = bmp_t;
            rec.bmp280_pa = bmp_pa;
        } else {
            rec.sensor_error_mask |= SZ2_SENSOR_ERR_BMP280;
        }

        uint32_t rain_total = 0;
        uint32_t rain_delta = 0;
        float rain_mm_total = 0.0f;
        rain_snapshot(&rain_total, &rain_delta, &rain_mm_total);
        rec.rain_tips_total = rain_total;
        rec.rain_tips_delta = rain_delta;
        rec.rain_mm_total = rain_mm_total;
        rec.rain_mm_delta = (float)rain_delta * RAIN_MM_PER_TIP;

        record_store_result_t store_result = cloud_enqueue_record(&rec);

        ESP_LOGI(TAG, "MASTER_SAMPLE seq=%u store=%u valid=0x%02X error=0x%08X hx711_raw=%ld ds0=%.3f ds1=%.3f "
                      "aht_t=%.3f aht_rh=%.3f bmp_t=%.3f bmp_pa=%.1f "
                      "rain_tips=%u delta=%u rain_mm_delta=%.4f",
                 (unsigned)rec.seq, (unsigned)store_result, rec.valid_mask,
                 (unsigned)rec.sensor_error_mask,
                 (long)rec.hx711_raw,
                 (double)rec.ds0_hive_c, (double)rec.ds1_scale_c,
                 (double)rec.aht20_t_c, (double)rec.aht20_rh,
                 (double)rec.bmp280_t_c, (double)rec.bmp280_pa,
                 (unsigned)rec.rain_tips_total, (unsigned)rec.rain_tips_delta,
                 (double)rec.rain_mm_delta);

        if (store_result == RECORD_STORE_FAILED) {
            ESP_LOGE(TAG, "master sample not durably queued seq=%u; rain cursor retained",
                     (unsigned)rec.seq);
        }
        vTaskDelay(pdMS_TO_TICKS(MASTER_SAMPLE_PERIOD_SEC * 1000UL));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== bee_scale MASTER v1 === mode=%s cloud=%d expected_ch=%u",
             MASTER_TEST_MODE ? "TEST" : "PROD", MASTER_CLOUD_MODE,
             (unsigned)MASTER_ESPNOW_CHANNEL);
#if MASTER_CLOUD_MODE
    if (MASTER_FIREBASE_ROOT_PATH[0] != '\0') {
        ESP_LOGI(TAG, "cloud target root=%s master_upload=%d",
                 MASTER_FIREBASE_ROOT_PATH, MASTER_CLOUD_UPLOAD_MASTER);
    } else {
        ESP_LOGI(TAG, "cloud target hive=%s path_suffix=%s master_upload=%d hive2_schema=%d",
                 MASTER_FIREBASE_TARGET_HIVE_ID, MASTER_FIREBASE_HIVE_PATH_SUFFIX,
                 MASTER_CLOUD_UPLOAD_MASTER, MASTER_CLOUD_HIVE2_SCHEMA);
    }
#endif
    ESP_LOGI(TAG, "pins HX711 SCK=%d DOUT=%d DS=%d RAIN=%d I2C SDA=%d SCL=%d",
             HX711_SCK_GPIO, HX711_DOUT_GPIO, DS18B20_GPIO, RAIN_GPIO,
             I2C_SDA_GPIO, I2C_SCL_GPIO);

    nvs_init_all();
    s_master_boot_id = esp_random();
    if (s_master_boot_id == 0) s_master_boot_id = 1;
    ESP_LOGI(TAG, "master boot_id=0x%08X", (unsigned)s_master_boot_id);
    s_rain_mutex = xSemaphoreCreateMutex();
    s_backup_mutex = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(MASTER_RX_QUEUE_LEN, sizeof(rx_item_t));
    s_tx_done = xSemaphoreCreateBinary();
#if MASTER_CLOUD_MODE
    s_reconn_sem = xSemaphoreCreateCounting(8, 0);
#endif
    configASSERT(s_rain_mutex && s_backup_mutex && s_rx_queue && s_tx_done);

    backup_init();
    hx711_init();
    ds18b20_init_gpio();
    rain_gpio_init();
    i2c_init_bus();
    ds18b20_refresh_search();
    s_aht20_present = aht20_init();
    s_bmp280.present = bmp280_init();

    wifi_espnow_init();
#if MASTER_WEB_STATUS
    web_status_start();
#endif

    xTaskCreatePinnedToCore(rx_consumer_task, "master_rx", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(rain_task, "rain", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(master_measure_task, "master_sample", 8192, NULL, 3, NULL, 1);
#if MASTER_WIFI_ENABLED
    xTaskCreatePinnedToCore(reconnect_task, "wifi_reconn", 4096, NULL, 4, NULL, 0);
#endif
#if MASTER_CLOUD_MODE
    xTaskCreatePinnedToCore(cloud_task, "cloud", 24576, NULL, 3, NULL, 0);
#endif

    ESP_LOGI(TAG, "master ready");
}

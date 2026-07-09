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

#if defined(MASTER_CLOUD_MODE) && MASTER_CLOUD_MODE
#if __has_include("cloud_config.h")
#include "cloud_config.h"
#else
#error "MASTER_CLOUD_MODE=1 mellett kell main/cloud_config.h (minta: cloud_config.example.h)"
#endif
#endif

#ifndef MASTER_CLOUD_MODE
#define MASTER_CLOUD_MODE 0
#endif

#define MASTER_UNIT_ID                 0
#define MASTER_SAMPLE_PERIOD_SEC       1200
#define MASTER_VALID_EPOCH_MIN         1700000000UL

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

#define RAIN_SAMPLE_PERIOD_MS          10
#define RAIN_DEBOUNCE_MS               20
#define RAIN_LOCKOUT_MS                250
#define RAIN_MM_PER_TIP                0.2794f

#define MASTER_RX_QUEUE_LEN            64
#define MASTER_CLOUD_QUEUE_LEN         64
#define MASTER_MAX_NODES               16
#define MASTER_SEND_TIMEOUT_MS         50
#define MASTER_NO_MEM_RETRY_MS         2
#define MASTER_ADVERT_SLEEP_S          12
#define MASTER_STAT_PERIOD_MS          10000

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
#define BACKUP_MAGIC                   0x42534D31UL

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
} master_record_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t write_seq;
    uint32_t crc32;
    uint32_t reserved;
    master_record_t rec;
    uint8_t pad[BACKUP_SLOT_SIZE - 16 - sizeof(master_record_t)];
} backup_slot_t;

typedef struct {
    sz2_payload_t payload;
    uint8_t src_mac[6];
    int8_t rssi;
    uint16_t len;
} rx_item_t;

typedef struct {
    bool in_use;
    uint8_t node_id;
    uint8_t mac[6];
    uint32_t boot_id;
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
    int64_t last_rx_us;
} node_state_t;

static QueueHandle_t s_rx_queue;
static QueueHandle_t s_cloud_queue;
static SemaphoreHandle_t s_tx_done;
static SemaphoreHandle_t s_rain_mutex;
static SemaphoreHandle_t s_backup_mutex;
#if MASTER_CLOUD_MODE
static SemaphoreHandle_t s_reconn_sem;
#endif

static node_state_t s_nodes[MASTER_MAX_NODES];
static ds18b20_sensor_t s_ds18b20[DS18B20_MAX_SENSORS];
static int s_ds18b20_count;
static bmp280_cal_t s_bmp280;
static bool s_aht20_present;

static nvs_handle_t s_master_nvs;
static nvs_handle_t s_rain_nvs;
static nvs_handle_t s_backup_nvs;
static uint32_t s_master_seq;
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
static volatile uint32_t s_cloud_drop;
static volatile uint32_t s_backup_enqueued;
static volatile uint32_t s_backup_uploaded;
static volatile uint32_t s_backup_bad;

#if MASTER_CLOUD_MODE
static volatile bool s_wifi_assoc;
static volatile bool s_wifi_ip;
static volatile uint32_t s_disconn_cnt;
static volatile uint32_t s_got_ip_cnt;
static volatile uint32_t s_backoff_ms = MASTER_RECONN_BACKOFF_MIN_MS;
static char s_firebase_token[2048];
static int64_t s_firebase_token_expire_us;
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

    if (nvs_get_u32(s_master_nvs, "seq", &s_master_seq) == ESP_ERR_NVS_NOT_FOUND) {
        s_master_seq = 0;
    }
    if (nvs_get_u32(s_rain_nvs, "tips", &s_rain_tip_count) == ESP_ERR_NVS_NOT_FOUND) {
        s_rain_tip_count = 0;
    }
}

static void rain_store_count(void)
{
    if (nvs_set_u32(s_rain_nvs, "tips", s_rain_tip_count) == ESP_OK) {
        nvs_commit(s_rain_nvs);
    }
}

static void backup_store_index(void)
{
    nvs_set_u32(s_backup_nvs, "head", s_backup_head);
    nvs_set_u32(s_backup_nvs, "tail", s_backup_tail);
    nvs_set_u32(s_backup_nvs, "wseq", s_backup_write_seq);
    nvs_commit(s_backup_nvs);
}

static uint32_t backup_count_unlocked(void)
{
    if (s_backup_head >= s_backup_tail) {
        return s_backup_head - s_backup_tail;
    }
    return s_backup_slot_count - s_backup_tail + s_backup_head;
}

static void backup_init(void)
{
    s_backup_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "backup");
    if (s_backup_part == NULL) {
        ESP_LOGW(TAG, "backup partition not found; flash backlog disabled");
        return;
    }
    s_backup_slot_count = s_backup_part->size / BACKUP_SLOT_SIZE;

    if (nvs_get_u32(s_backup_nvs, "head", &s_backup_head) == ESP_ERR_NVS_NOT_FOUND) {
        s_backup_head = 0;
    }
    if (nvs_get_u32(s_backup_nvs, "tail", &s_backup_tail) == ESP_ERR_NVS_NOT_FOUND) {
        s_backup_tail = 0;
    }
    if (nvs_get_u32(s_backup_nvs, "wseq", &s_backup_write_seq) == ESP_ERR_NVS_NOT_FOUND) {
        s_backup_write_seq = 0;
    }
    if (s_backup_head >= s_backup_slot_count || s_backup_tail >= s_backup_slot_count) {
        s_backup_head = 0;
        s_backup_tail = 0;
        s_backup_write_seq = 0;
        backup_store_index();
    }
    ESP_LOGI(TAG, "backup partition size=%u slots=%u pending=%u",
             (unsigned)s_backup_part->size, (unsigned)s_backup_slot_count,
             (unsigned)backup_count_unlocked());
}

#if MASTER_CLOUD_MODE
static bool backup_enqueue(const master_record_t *rec)
{
    if (s_backup_part == NULL || s_backup_slot_count == 0) {
        return false;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);

    const uint32_t slots_per_sector = BACKUP_SECTOR_SIZE / BACKUP_SLOT_SIZE;
    uint32_t next = (s_backup_head + 1) % s_backup_slot_count;
    if (next == s_backup_tail) {
        s_backup_tail = (s_backup_tail + 1) % s_backup_slot_count;
    }

    if ((s_backup_head % slots_per_sector) == 0) {
        uint32_t sector = s_backup_head / slots_per_sector;
        uint32_t tail_sector = s_backup_tail / slots_per_sector;
        if (s_backup_tail != s_backup_head && tail_sector == sector) {
            s_backup_tail = ((sector + 1) * slots_per_sector) % s_backup_slot_count;
        }
        esp_err_t err = esp_partition_erase_range(s_backup_part, sector * BACKUP_SECTOR_SIZE,
                                                  BACKUP_SECTOR_SIZE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "backup erase failed: %s", esp_err_to_name(err));
            xSemaphoreGive(s_backup_mutex);
            return false;
        }
    }

    backup_slot_t slot;
    memset(&slot, 0xFF, sizeof(slot));
    slot.magic = BACKUP_MAGIC;
    slot.write_seq = ++s_backup_write_seq;
    slot.reserved = 0;
    memcpy(&slot.rec, rec, sizeof(slot.rec));
    slot.crc32 = crc32_update(0, (const uint8_t *)&slot.rec, sizeof(slot.rec));

    esp_err_t err = esp_partition_write(s_backup_part, s_backup_head * BACKUP_SLOT_SIZE,
                                        &slot, sizeof(slot));
    if (err == ESP_OK) {
        s_backup_head = next;
        backup_store_index();
        s_backup_enqueued++;
    } else {
        ESP_LOGW(TAG, "backup write failed: %s", esp_err_to_name(err));
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

    backup_slot_t slot;
    esp_err_t err = esp_partition_read(s_backup_part, s_backup_tail * BACKUP_SLOT_SIZE,
                                       &slot, sizeof(slot));
    if (err != ESP_OK || slot.magic != BACKUP_MAGIC ||
        slot.crc32 != crc32_update(0, (const uint8_t *)&slot.rec, sizeof(slot.rec))) {
        s_backup_bad++;
        s_backup_tail = (s_backup_tail + 1) % s_backup_slot_count;
        backup_store_index();
        xSemaphoreGive(s_backup_mutex);
        return false;
    }

    memcpy(rec, &slot.rec, sizeof(*rec));
    xSemaphoreGive(s_backup_mutex);
    return true;
}

static void backup_pop(void)
{
    if (s_backup_part == NULL || s_backup_head == s_backup_tail) {
        return;
    }
    xSemaphoreTake(s_backup_mutex, portMAX_DELAY);
    s_backup_tail = (s_backup_tail + 1) % s_backup_slot_count;
    backup_store_index();
    s_backup_uploaded++;
    xSemaphoreGive(s_backup_mutex);
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

    uint32_t value = 0;
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

    if (value & 0x800000U) {
        value |= 0xFF000000U;
    }
    *raw = (int32_t)value;
    return true;
}

static bool hx711_read_avg(int32_t *avg)
{
    int64_t acc = 0;
    int ok = 0;
    int zero_count = 0;
    for (int i = 0; i < HX711_SAMPLES; i++) {
        int32_t raw = 0;
        if (hx711_read_raw_once(&raw)) {
            acc += raw;
            ok++;
            if (raw == 0) zero_count++;
        }
    }
    if (ok == 0 || zero_count == ok) {
        return false;
    }
    *avg = (int32_t)(acc / ok);
    return true;
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

static void ds18b20_refresh_search(void)
{
    s_ds18b20_count = ds18b20_search(s_ds18b20, DS18B20_MAX_SENSORS);
    ESP_LOGI(TAG, "DS18B20 found=%d expected=%d", s_ds18b20_count, DS18B20_EXPECTED_COUNT);
    for (int i = 0; i < s_ds18b20_count; i++) {
        char rom[17];
        rom_to_hex(s_ds18b20[i].rom, rom);
        ESP_LOGI(TAG, "DS%d ROM=%s", i, rom);
    }
}

static void ds18b20_match_rom(const uint8_t rom[8])
{
    ow_write_byte(0x55);
    for (int i = 0; i < 8; i++) ow_write_byte(rom[i]);
}

static bool ds18b20_read_all(float temps[DS18B20_MAX_SENSORS])
{
    for (int i = 0; i < DS18B20_MAX_SENSORS; i++) temps[i] = NAN;
    if (s_ds18b20_count <= 0) return false;

    if (!ow_reset()) return false;
    ow_write_byte(0xCC);
    ow_write_byte(0x44);
    vTaskDelay(pdMS_TO_TICKS(750));

    bool all_ok = true;
    for (int i = 0; i < s_ds18b20_count; i++) {
        if (!ow_reset()) {
            all_ok = false;
            continue;
        }
        ds18b20_match_rom(s_ds18b20[i].rom);
        ow_write_byte(0xBE);
        uint8_t sp[9];
        for (int b = 0; b < 9; b++) sp[b] = ow_read_byte();
        if (crc8_maxim(sp, 8) != sp[8]) {
            all_ok = false;
            continue;
        }
        int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
        temps[i] = (float)raw / 16.0f;
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
    int64_t candidate_since_ms = millis_u32();
    int64_t last_tip_ms = -RAIN_LOCKOUT_MS;

    for (;;) {
        int64_t ms = millis_u32();
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
                rain_store_count();
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
    s_rain_last_reported = s_rain_tip_count;
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
        if (esp_now_add_peer(&peer) != ESP_OK) return;
        esp_now_rate_config_t rc = {
            .phymode = WIFI_PHY_MODE_11B,
            .rate = WIFI_PHY_RATE_1M_L,
            .ersu = false,
            .dcm = false,
        };
        esp_now_set_peer_rate_config(mac, &rc);
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
    } else {
        memset(&item.payload, 0, sizeof(item.payload));
    }
    memcpy(item.src_mac, info->src_addr, 6);
    item.rssi = (info->rx_ctrl != NULL) ? (int8_t)info->rx_ctrl->rssi : 0;
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        s_q_dropped++;
    }
}

static bool send_ack(const uint8_t *dest, uint8_t node_id, uint32_t seq)
{
    sz2_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    ack.protocol_version = SZ2_PROTO_VERSION;
    ack.packet_type = SZ2_PKT_ACK;
    ack.node_id = node_id;
    ack.ack_last_seq = seq;
    ack.clear_buffer_until_seq = seq;
    ack.next_slot_offset_s = MASTER_ADVERT_SLEEP_S;

    ensure_peer(dest);
    for (;;) {
        esp_err_t err = esp_now_send(dest, (const uint8_t *)&ack, sizeof(ack));
        if (err == ESP_OK) {
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(MASTER_SEND_TIMEOUT_MS));
            return true;
        }
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            vTaskDelay(pdMS_TO_TICKS(MASTER_NO_MEM_RETRY_MS));
            continue;
        }
        return false;
    }
}

static node_state_t *node_get(uint8_t node_id, const uint8_t *mac)
{
    for (int i = 0; i < MASTER_MAX_NODES; i++) {
        if (s_nodes[i].in_use && s_nodes[i].node_id == node_id) return &s_nodes[i];
    }
    for (int i = 0; i < MASTER_MAX_NODES; i++) {
        if (!s_nodes[i].in_use) {
            node_state_t *n = &s_nodes[i];
            memset(n, 0, sizeof(*n));
            n->in_use = true;
            n->node_id = node_id;
            memcpy(n->mac, mac, 6);
            return n;
        }
    }
    return NULL;
}

static void cloud_enqueue_record(const master_record_t *rec)
{
#if MASTER_CLOUD_MODE
    if (xQueueSend(s_cloud_queue, rec, 0) != pdTRUE) {
        s_cloud_drop++;
        backup_enqueue(rec);
    }
#else
    ESP_LOGI(TAG, "CLOUD_OFF type=%u unit=%u seq=%u", rec->type, rec->unit_id, (unsigned)rec->seq);
#endif
}

static void node_record_from_payload(master_record_t *rec, const sz2_payload_t *p,
                                     const rx_item_t *item, uint32_t gap)
{
    memset(rec, 0, sizeof(*rec));
    rec->type = REC_TYPE_NODE;
    rec->unit_id = p->node_id;
    rec->valid_mask = REC_VALID_HX711;
    if (isfinite(p->temp)) rec->valid_mask |= REC_VALID_TEMP;
    rec->rssi = item->rssi;
    memcpy(rec->src_mac, item->src_mac, sizeof(rec->src_mac));
    rec->seq = p->seq;
    rec->boot_id = p->boot_id;
    rec->rx_ms = millis_u32();
    rec->ts = current_epoch_s();
    rec->vbat_mv = p->vbat;
    rec->gap = gap;
    rec->weight_raw = (int32_t)lrintf(p->weight);
    rec->temp_c = p->temp;
}

static void rx_consumer_task(void *arg)
{
    (void)arg;
    rx_item_t item;
    int64_t last_stat_us = esp_timer_get_time();

    for (;;) {
        bool got = xQueueReceive(s_rx_queue, &item, pdMS_TO_TICKS(500)) == pdTRUE;
        if (got) {
            if (item.len != (uint16_t)sizeof(sz2_payload_t)) {
                s_diag_len_bad++;
                continue;
            }
            sz2_payload_t *p = &item.payload;
            if (p->protocol_version != SZ2_PROTO_VERSION || p->packet_type != SZ2_PKT_DATA) {
                s_diag_ver_bad++;
                continue;
            }
            node_state_t *n = node_get(p->node_id, item.src_mac);
            if (n == NULL) continue;
            if (sz2_payload_crc(p) != p->crc) {
                n->crc_err++;
                continue;
            }

            if (n->rx_valid == 0) {
                n->boot_id = p->boot_id;
            } else if (p->boot_id != n->boot_id) {
                n->boot_id = p->boot_id;
                n->have_seq = false;
                n->resets++;
            }
            if (memcmp(n->mac, item.src_mac, 6) != 0) {
                memcpy(n->mac, item.src_mac, 6);
            }

            n->rx_valid++;
            n->last_rx_us = esp_timer_get_time();
            uint32_t gap = 0;
            bool is_new = false;
            if (!n->have_seq) {
                is_new = true;
                n->have_seq = true;
                n->last_seq = p->seq;
            } else if (p->seq > n->last_seq) {
                is_new = true;
                gap = p->seq - n->last_seq - 1;
                n->miss_cum += gap;
                n->last_seq = p->seq;
            }
            if (is_new) n->rx_new++;
            else n->rx_dup++;

            uint32_t fail_before = s_ack_status_fail;
            if (send_ack(item.src_mac, p->node_id, p->seq)) {
                n->ack_sent++;
                if (s_ack_status_fail != fail_before) n->ack_txfail++;
            }

            ESP_LOGI(TAG, "RX node=%u seq=%u kind=%s gap=%u rssi=%d weight_raw=%.0f temp=%.2f",
                     p->node_id, (unsigned)p->seq, is_new ? "NEW" : "DUP",
                     (unsigned)gap, (int)item.rssi, (double)p->weight, (double)p->temp);

            if (is_new) {
                master_record_t rec;
                node_record_from_payload(&rec, p, &item, gap);
                cloud_enqueue_record(&rec);
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_stat_us >= (int64_t)MASTER_STAT_PERIOD_MS * 1000) {
            last_stat_us = now_us;
            uint8_t ch = 0;
            wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
            esp_wifi_get_channel(&ch, &sec);
#if MASTER_CLOUD_MODE
            const char *wifi_state = s_wifi_ip ? "IP" : (s_wifi_assoc ? "ASSOC" : "DOWN");
#else
            const char *wifi_state = "OFF";
#endif
            ESP_LOGI(TAG, "MSTAT uptime_s=%u rxcb=%u q_drop=%u len_bad=%u ver_bad=%u "
                          "wifi=%s ch=%u cloud_ok=%u cloud_fail=%u cloud_drop=%u "
                          "backup_pending=%u backup_enq=%u backup_up=%u backup_bad=%u",
                     (unsigned)(now_us / 1000000), (unsigned)s_diag_recv_cb,
                     (unsigned)s_q_dropped, (unsigned)s_diag_len_bad,
                     (unsigned)s_diag_ver_bad, wifi_state, (unsigned)ch,
                     (unsigned)s_cloud_ok, (unsigned)s_cloud_fail, (unsigned)s_cloud_drop,
                     (unsigned)backup_count_unlocked(), (unsigned)s_backup_enqueued,
                     (unsigned)s_backup_uploaded, (unsigned)s_backup_bad);
        }
    }
}

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
                      char *resp, int resp_cap, int *status_out)
{
    http_resp_buf_t rb = {.buf = resp, .cap = resp_cap, .len = 0};
    if (resp_cap > 0) resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = MASTER_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_cb,
        .user_data = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return false;
    esp_http_client_set_method(client, method);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
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
    if (!http_json(url, body, HTTP_METHOD_POST, resp, sizeof(resp), &status)) {
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

    int n = json_add(body, body_cap, 0, "{\"ts\":%u,\"rx_ms\":%u,\"unit_id\":%u,\"seq\":%u",
                     (unsigned)ts, (unsigned)rec->rx_ms, rec->unit_id, (unsigned)rec->seq);
    if (rec->type == REC_TYPE_NODE) {
        n = json_add(body, body_cap, n,
                     ",\"boot_id\":\"0x%08X\",\"weight_raw\":%ld,\"vbat_mv\":%u,"
                     "\"rssi\":%d,\"gap\":%u,"
                     "\"src_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
                     (unsigned)rec->boot_id, (long)rec->weight_raw, (unsigned)rec->vbat_mv,
                     (int)rec->rssi, (unsigned)rec->gap,
                     rec->src_mac[0], rec->src_mac[1], rec->src_mac[2],
                     rec->src_mac[3], rec->src_mac[4], rec->src_mac[5]);
        if (rec->valid_mask & REC_VALID_TEMP) {
            n = json_add(body, body_cap, n, ",\"temp_c\":%.3f", (double)rec->temp_c);
        }
    } else {
        n = json_add(body, body_cap, n,
                     ",\"rain_tips_total\":%u,\"rain_mm_total\":%.4f,\"rain_tips_delta\":%u",
                     (unsigned)rec->rain_tips_total, (double)rec->rain_mm_total,
                     (unsigned)rec->rain_tips_delta);
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

static bool cloud_post_record_once(const master_record_t *rec)
{
    if (!s_wifi_ip) return false;
    if (!firebase_token_valid() && !firebase_auth_refresh()) return false;

    char url[2700];
    int u;
    if (rec->type == REC_TYPE_NODE) {
        u = snprintf(url, sizeof(url),
                     "%s/hives/%s/nodes/node_%u/raw/%08X_%u.json?auth=%s",
                     MASTER_FIREBASE_DATABASE_URL, MASTER_FIREBASE_HIVE_ID,
                     rec->unit_id, (unsigned)rec->boot_id, (unsigned)rec->seq,
                     s_firebase_token);
    } else {
        u = snprintf(url, sizeof(url),
                     "%s/hives/%s/master/raw/master_%u.json?auth=%s",
                     MASTER_FIREBASE_DATABASE_URL, MASTER_FIREBASE_HIVE_ID,
                     (unsigned)rec->seq, s_firebase_token);
    }
    if (u <= 0 || u >= (int)sizeof(url)) return false;

    char body[1024];
    if (!build_record_json(rec, body, sizeof(body))) return false;
    char resp[256];
    int status = 0;
    bool ok = http_json(url, body, HTTP_METHOD_PUT, resp, sizeof(resp), &status);
    if (!ok) ESP_LOGW(TAG, "cloud PUT failed status=%d type=%u unit=%u seq=%u",
                      status, rec->type, rec->unit_id, (unsigned)rec->seq);
    return ok;
}

static bool cloud_post_record_with_retries(const master_record_t *rec)
{
    uint32_t backoff = MASTER_CLOUD_RETRY_BASE_MS;
    for (int attempt = 1; attempt <= MASTER_CLOUD_MAX_ATTEMPTS; attempt++) {
        if (cloud_post_record_once(rec)) {
            s_cloud_ok++;
            return true;
        }
        s_cloud_fail++;
        if (attempt < MASTER_CLOUD_MAX_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = backoff * 2U;
            if (backoff > MASTER_CLOUD_RETRY_MAX_MS) backoff = MASTER_CLOUD_RETRY_MAX_MS;
        }
    }
    return false;
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
        while (s_wifi_ip && backup_peek(&rec)) {
            if (cloud_post_record_with_retries(&rec)) {
                backup_pop();
            } else {
                break;
            }
        }

        if (xQueueReceive(s_cloud_queue, &rec, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }
        if (!cloud_post_record_with_retries(&rec)) {
            backup_enqueue(&rec);
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *ev = (const wifi_event_sta_connected_t *)data;
        s_wifi_assoc = true;
        ESP_LOGI(TAG, "WIFI CONNECTED ssid=%s ch=%u", (const char *)ev->ssid, (unsigned)ev->channel);
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
        sntp_start_once();
        uint8_t ch = 0;
        wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
        esp_wifi_get_channel(&ch, &sec);
        ESP_LOGI(TAG, "WIFI GOT_IP ip=" IPSTR " gw=" IPSTR " ch=%u",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw), (unsigned)ch);
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

static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if MASTER_CLOUD_MODE
    esp_netif_create_default_wifi_sta();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

#if MASTER_CLOUD_MODE
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

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    static const uint8_t bc[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ensure_peer(bc);

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "master MAC=%02X:%02X:%02X:%02X:%02X:%02X cloud=%d",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], MASTER_CLOUD_MODE);
}

static void master_measure_task(void *arg)
{
    (void)arg;
    for (;;) {
        master_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.type = REC_TYPE_MASTER;
        rec.unit_id = MASTER_UNIT_ID;
        rec.seq = ++s_master_seq;
        rec.rx_ms = millis_u32();
        rec.ts = current_epoch_s();

        int32_t hx_raw = 0;
        if (hx711_read_avg(&hx_raw)) {
            rec.valid_mask |= REC_VALID_HX711;
            rec.hx711_raw = hx_raw;
        }

        float ds_t[DS18B20_MAX_SENSORS];
        ds18b20_read_all(ds_t);
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
            s_aht20_present = aht20_init();
        }

        float bmp_t = NAN;
        float bmp_pa = NAN;
        if (s_bmp280.present && bmp280_read(&bmp_t, &bmp_pa)) {
            rec.valid_mask |= REC_VALID_BMP;
            rec.bmp280_t_c = bmp_t;
            rec.bmp280_pa = bmp_pa;
        } else {
            bmp280_init();
        }

        uint32_t rain_total = 0;
        uint32_t rain_delta = 0;
        float rain_mm_total = 0.0f;
        rain_snapshot(&rain_total, &rain_delta, &rain_mm_total);
        rec.rain_tips_total = rain_total;
        rec.rain_tips_delta = rain_delta;
        rec.rain_mm_total = rain_mm_total;

        nvs_set_u32(s_master_nvs, "seq", s_master_seq);
        nvs_commit(s_master_nvs);

        ESP_LOGI(TAG, "MASTER_SAMPLE seq=%u valid=0x%02X hx711_raw=%ld ds0=%.3f ds1=%.3f "
                      "aht_t=%.3f aht_rh=%.3f bmp_t=%.3f bmp_pa=%.1f rain_tips=%u delta=%u",
                 (unsigned)rec.seq, rec.valid_mask, (long)rec.hx711_raw,
                 (double)rec.ds0_hive_c, (double)rec.ds1_scale_c,
                 (double)rec.aht20_t_c, (double)rec.aht20_rh,
                 (double)rec.bmp280_t_c, (double)rec.bmp280_pa,
                 (unsigned)rec.rain_tips_total, (unsigned)rec.rain_tips_delta);

        cloud_enqueue_record(&rec);
        vTaskDelay(pdMS_TO_TICKS(MASTER_SAMPLE_PERIOD_SEC * 1000UL));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== bee_scale MASTER v1 ===");
    ESP_LOGI(TAG, "pins HX711 SCK=%d DOUT=%d DS=%d RAIN=%d I2C SDA=%d SCL=%d",
             HX711_SCK_GPIO, HX711_DOUT_GPIO, DS18B20_GPIO, RAIN_GPIO,
             I2C_SDA_GPIO, I2C_SCL_GPIO);

    nvs_init_all();
    s_rain_mutex = xSemaphoreCreateMutex();
    s_backup_mutex = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(MASTER_RX_QUEUE_LEN, sizeof(rx_item_t));
    s_cloud_queue = xQueueCreate(MASTER_CLOUD_QUEUE_LEN, sizeof(master_record_t));
    s_tx_done = xSemaphoreCreateBinary();
#if MASTER_CLOUD_MODE
    s_reconn_sem = xSemaphoreCreateCounting(8, 0);
#endif
    configASSERT(s_rain_mutex && s_backup_mutex && s_rx_queue && s_cloud_queue && s_tx_done);

    backup_init();
    hx711_init();
    ds18b20_init_gpio();
    rain_gpio_init();
    i2c_init_bus();
    ds18b20_refresh_search();
    s_aht20_present = aht20_init();
    s_bmp280.present = bmp280_init();

    wifi_espnow_init();

    xTaskCreatePinnedToCore(rx_consumer_task, "master_rx", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(rain_task, "rain", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(master_measure_task, "master_sample", 8192, NULL, 3, NULL, 1);
#if MASTER_CLOUD_MODE
    xTaskCreatePinnedToCore(reconnect_task, "wifi_reconn", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(cloud_task, "cloud", 8192, NULL, 3, NULL, 0);
#endif

    ESP_LOGI(TAG, "master ready");
}

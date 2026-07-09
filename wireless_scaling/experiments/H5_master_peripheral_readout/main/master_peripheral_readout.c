/*
 * H5 — master peripheral readout hardware test.
 *
 * No radio/cloud/sleep. This experiment only validates the master peripheral wiring:
 *   HX711       SCK=GPIO2, DOUT=GPIO4
 *   DS18B20 bus GPIO5, expected two sensors
 *   rain switch GPIO7, external pull-up + RC filter, closed switch pulls LOW
 *   AHT20       I2C GPIO8 SDA, GPIO9 SCL, address 0x38
 *   BMP280-M    I2C GPIO8 SDA, GPIO9 SCL, address 0x76 or 0x77
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HX711_SCK_GPIO          GPIO_NUM_2
#define HX711_DOUT_GPIO         GPIO_NUM_4
#define DS18B20_GPIO            GPIO_NUM_5
#define RAIN_SWITCH_GPIO        GPIO_NUM_7
#define I2C_SDA_GPIO            GPIO_NUM_8
#define I2C_SCL_GPIO            GPIO_NUM_9

#define I2C_PORT                I2C_NUM_0
#define I2C_FREQ_HZ             100000
#define I2C_TIMEOUT_MS          100

#define AHT20_ADDR              0x38
#define BMP280_ADDR_0           0x76
#define BMP280_ADDR_1           0x77
#define BMP280_CHIP_ID          0x58

#define READ_PERIOD_MS          2000
#define HX711_SAMPLES           8
#define HX711_READY_TIMEOUT_MS  700
#define HX711_DIAG_LOG          0
#define DS18B20_EXPECTED_COUNT  2
#define DS18B20_MAX_SENSORS     4

static const char *TAG = "MASTER-READ";

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

static ds18b20_sensor_t s_ds18b20[DS18B20_MAX_SENSORS];
static int s_ds18b20_count;
static bmp280_cal_t s_bmp280;
static bool s_aht20_present;

static uint8_t crc8_maxim(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t in = data[i];
        for (int b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ in) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
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

    gpio_set_level(HX711_SCK_GPIO, 1);
    esp_rom_delay_us(100);
    ESP_LOGI(TAG, "HX711 init: SCK=GPIO%d DOUT=GPIO%d", HX711_SCK_GPIO, HX711_DOUT_GPIO);
}

static void hx711_power_up(void)
{
    gpio_set_level(HX711_SCK_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void hx711_power_down(void)
{
    gpio_set_level(HX711_SCK_GPIO, 1);
    esp_rom_delay_us(100);
}

static bool hx711_read_raw_once(int32_t *raw, int64_t *ready_wait_us, int *dout_after)
{
    int64_t start_us = esp_timer_get_time();
    while (gpio_get_level(HX711_DOUT_GPIO) == 1) {
        if ((esp_timer_get_time() - start_us) > (int64_t)HX711_READY_TIMEOUT_MS * 1000) {
            return false;
        }
        esp_rom_delay_us(100);
    }
    *ready_wait_us = esp_timer_get_time() - start_us;

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

    *dout_after = gpio_get_level(HX711_DOUT_GPIO);

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
    int dout_after_low_count = 0;

    hx711_power_up();
    for (int i = 0; i < HX711_SAMPLES; i++) {
        int32_t raw = 0;
        int64_t ready_wait_us = 0;
        int dout_after = -1;
        if (hx711_read_raw_once(&raw, &ready_wait_us, &dout_after)) {
            acc += raw;
            ok++;
            if (raw == 0) {
                zero_count++;
            }
            if (dout_after == 0) {
                dout_after_low_count++;
            }
#if HX711_DIAG_LOG
            ESP_LOGI(TAG, "HX711_DIAG sample=%d raw=%" PRId32 " raw_hex=0x%06" PRIX32
                     " ready_us=%" PRId64 " dout_after=%d",
                     i, raw, ((uint32_t)raw) & 0x00FFFFFFU, ready_wait_us, dout_after);
#endif
        } else {
            ESP_LOGW(TAG, "HX711 timeout sample=%d dout_now=%d sck_now=%d",
                     i, gpio_get_level(HX711_DOUT_GPIO), gpio_get_level(HX711_SCK_GPIO));
            break;
        }
    }
    hx711_power_down();

    if (ok == 0) {
        return false;
    }
    *avg = (int32_t)(acc / ok);
    if (zero_count == ok && dout_after_low_count == ok) {
        ESP_LOGW(TAG, "HX711 invalid: DOUT stuck LOW / all-zero frame (check DT/SCK wiring, HX711 power, GND)");
        return false;
    }
    return ok == HX711_SAMPLES;
}

static void rain_switch_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RAIN_SWITCH_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_LOGI(TAG, "Rain switch init: GPIO%d external pull-up + RC filter, closed=LOW",
             RAIN_SWITCH_GPIO);
}

static bool rain_switch_closed(int *level)
{
    int low_count = 0;
    int high_count = 0;

    for (int i = 0; i < 5; i++) {
        if (gpio_get_level(RAIN_SWITCH_GPIO) == 0) {
            low_count++;
        } else {
            high_count++;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    *level = (low_count > high_count) ? 0 : 1;
    return *level == 0;
}

static void ow_drive_low(void)
{
    gpio_set_level(DS18B20_GPIO, 0);
}

static void ow_release(void)
{
    gpio_set_level(DS18B20_GPIO, 1);
}

static int ow_read_pin(void)
{
    return gpio_get_level(DS18B20_GPIO);
}

static bool ow_reset(void)
{
    ow_drive_low();
    esp_rom_delay_us(480);
    ow_release();
    esp_rom_delay_us(70);
    bool present = ow_read_pin() == 0;
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
    int bit = ow_read_pin();
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
        if (ow_read_bit()) {
            value |= (uint8_t)(1U << i);
        }
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
    ESP_LOGI(TAG, "DS18B20 init: 1-Wire GPIO%d open-drain, external 4k7 pull-up recommended",
             DS18B20_GPIO);
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

static int ds18b20_search(ds18b20_sensor_t *out, int max_count)
{
    uint8_t rom[8] = {0};
    int last_discrepancy = 0;
    bool last_device = false;
    int found = 0;

    while (!last_device && found < max_count) {
        if (!ow_reset()) {
            break;
        }

        ow_write_byte(0xF0);
        int discrepancy_marker = 0;
        int rom_byte = 0;
        uint8_t rom_mask = 1;

        for (int bit_number = 1; bit_number <= 64; bit_number++) {
            int id_bit = ow_read_bit();
            int cmp_id_bit = ow_read_bit();
            int search_direction;

            if (id_bit == 1 && cmp_id_bit == 1) {
                return found;
            }

            if (id_bit != cmp_id_bit) {
                search_direction = id_bit;
            } else {
                if (bit_number < last_discrepancy) {
                    search_direction = (rom[rom_byte] & rom_mask) ? 1 : 0;
                } else {
                    search_direction = bit_number == last_discrepancy;
                }
                if (search_direction == 0) {
                    discrepancy_marker = bit_number;
                }
            }

            if (search_direction) {
                rom[rom_byte] |= rom_mask;
            } else {
                rom[rom_byte] &= (uint8_t)~rom_mask;
            }
            ow_write_bit(search_direction);

            rom_mask <<= 1;
            if (rom_mask == 0) {
                rom_byte++;
                rom_mask = 1;
            }
        }

        last_discrepancy = discrepancy_marker;
        if (last_discrepancy == 0) {
            last_device = true;
        }

        if (rom[0] == 0x28 && crc8_maxim(rom, 7) == rom[7]) {
            memcpy(out[found].rom, rom, sizeof(rom));
            found++;
        } else {
            char bad_rom[17];
            rom_to_hex(rom, bad_rom);
            ESP_LOGW(TAG, "DS18B20 invalid/non-DS ROM ignored: %s", bad_rom);
        }
    }

    return found;
}

static void ds18b20_refresh_search(void)
{
    s_ds18b20_count = ds18b20_search(s_ds18b20, DS18B20_MAX_SENSORS);
    ESP_LOGI(TAG, "DS18B20 search: found=%d expected=%d", s_ds18b20_count, DS18B20_EXPECTED_COUNT);
    for (int i = 0; i < s_ds18b20_count; i++) {
        char rom[17];
        rom_to_hex(s_ds18b20[i].rom, rom);
        ESP_LOGI(TAG, "DS18B20[%d] ROM=%s", i, rom);
    }
}

static void ds18b20_match_rom(const uint8_t rom[8])
{
    ow_write_byte(0x55);
    for (int i = 0; i < 8; i++) {
        ow_write_byte(rom[i]);
    }
}

static bool ds18b20_read_all(float temps[DS18B20_MAX_SENSORS])
{
    for (int i = 0; i < DS18B20_MAX_SENSORS; i++) {
        temps[i] = NAN;
    }
    if (s_ds18b20_count <= 0) {
        return false;
    }

    if (!ow_reset()) {
        ESP_LOGW(TAG, "DS18B20 bus missing before conversion");
        return false;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0x44);
    vTaskDelay(pdMS_TO_TICKS(750));

    bool all_ok = true;
    for (int i = 0; i < s_ds18b20_count; i++) {
        if (!ow_reset()) {
            ESP_LOGW(TAG, "DS18B20 bus missing before read index=%d", i);
            all_ok = false;
            continue;
        }
        ds18b20_match_rom(s_ds18b20[i].rom);
        ow_write_byte(0xBE);

        uint8_t sp[9];
        for (int b = 0; b < 9; b++) {
            sp[b] = ow_read_byte();
        }
        if (crc8_maxim(sp, 8) != sp[8]) {
            ESP_LOGW(TAG, "DS18B20 CRC error index=%d", i);
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

static esp_err_t i2c_write_read(uint8_t addr, const uint8_t *wr, size_t wr_len, uint8_t *rd, size_t rd_len)
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
    ESP_LOGI(TAG, "I2C init: SDA=GPIO%d SCL=GPIO%d freq=%d", I2C_SDA_GPIO, I2C_SCL_GPIO, I2C_FREQ_HZ);
}

static bool aht20_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(40));
    uint8_t status_cmd = 0x71;
    uint8_t status = 0;
    if (i2c_write_read(AHT20_ADDR, &status_cmd, 1, &status, 1) != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 not found at 0x%02X", AHT20_ADDR);
        return false;
    }

    if ((status & 0x08) == 0) {
        uint8_t init_cmd[] = {0xBE, 0x08, 0x00};
        if (i2c_write_bytes(AHT20_ADDR, init_cmd, sizeof(init_cmd)) != ESP_OK) {
            ESP_LOGW(TAG, "AHT20 calibration command failed");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "AHT20 init OK status=0x%02X", status);
    return true;
}

static bool aht20_read(float *temp_c, float *rh)
{
    uint8_t cmd[] = {0xAC, 0x33, 0x00};
    if (i2c_write_bytes(AHT20_ADDR, cmd, sizeof(cmd)) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(90));

    uint8_t data[7] = {0};
    if (i2c_read_bytes(AHT20_ADDR, data, sizeof(data)) != ESP_OK) {
        return false;
    }
    if (data[0] & 0x80) {
        return false;
    }
    if (crc8_aht20(data, 6) != data[6]) {
        ESP_LOGW(TAG, "AHT20 CRC error");
        return false;
    }

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

static esp_err_t bmp280_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_write_read(addr, &reg, 1, data, len);
}

static esp_err_t bmp280_write_reg(uint8_t addr, uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_write_bytes(addr, data, sizeof(data));
}

static bool bmp280_try_init_addr(uint8_t addr)
{
    uint8_t id = 0;
    if (bmp280_read_reg(addr, 0xD0, &id, 1) != ESP_OK || id != BMP280_CHIP_ID) {
        return false;
    }

    uint8_t cal[24] = {0};
    if (bmp280_read_reg(addr, 0x88, cal, sizeof(cal)) != ESP_OK) {
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

    ESP_ERROR_CHECK_WITHOUT_ABORT(bmp280_write_reg(addr, 0xF5, 0x00));
    ESP_LOGI(TAG, "BMP280 init OK addr=0x%02X id=0x%02X", addr, id);
    return true;
}

static bool bmp280_init(void)
{
    memset(&s_bmp280, 0, sizeof(s_bmp280));
    if (bmp280_try_init_addr(BMP280_ADDR_0)) {
        return true;
    }
    if (bmp280_try_init_addr(BMP280_ADDR_1)) {
        return true;
    }
    ESP_LOGW(TAG, "BMP280 not found at 0x%02X or 0x%02X", BMP280_ADDR_0, BMP280_ADDR_1);
    return false;
}

static bool bmp280_read(float *temp_c, float *pressure_hpa)
{
    if (!s_bmp280.present) {
        return false;
    }

    if (bmp280_write_reg(s_bmp280.addr, 0xF4, 0x25) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[6] = {0};
    if (bmp280_read_reg(s_bmp280.addr, 0xF7, data, sizeof(data)) != ESP_OK) {
        return false;
    }

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
    p_var2 = p_var2 + ((p_var1 * (int64_t)s_bmp280.dig_p5) << 17);
    p_var2 = p_var2 + (((int64_t)s_bmp280.dig_p4) << 35);
    p_var1 = ((p_var1 * p_var1 * (int64_t)s_bmp280.dig_p3) >> 8) +
             ((p_var1 * (int64_t)s_bmp280.dig_p2) << 12);
    p_var1 = (((((int64_t)1) << 47) + p_var1)) * ((int64_t)s_bmp280.dig_p1) >> 33;

    if (p_var1 == 0) {
        return false;
    }

    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - p_var2) * 3125) / p_var1;
    p_var1 = (((int64_t)s_bmp280.dig_p9) * (p >> 13) * (p >> 13)) >> 25;
    p_var2 = (((int64_t)s_bmp280.dig_p8) * p) >> 19;
    p = ((p + p_var1 + p_var2) >> 8) + (((int64_t)s_bmp280.dig_p7) << 4);

    *temp_c = (float)t / 100.0f;
    *pressure_hpa = (float)p / 25600.0f;
    return true;
}

static void log_reading(uint32_t seq)
{
    int32_t hx_raw = 0;
    bool hx_ok = hx711_read_avg(&hx_raw);

    float ds_t[DS18B20_MAX_SENSORS];
    bool ds_ok = ds18b20_read_all(ds_t);

    int rain_level = 1;
    bool rain_closed = rain_switch_closed(&rain_level);

    float aht_t = NAN;
    float aht_rh = NAN;
    bool aht_ok = s_aht20_present && aht20_read(&aht_t, &aht_rh);
    if (!aht_ok) {
        s_aht20_present = aht20_init();
    }

    float bmp_t = NAN;
    float bmp_hpa = NAN;
    bool bmp_ok = s_bmp280.present && bmp280_read(&bmp_t, &bmp_hpa);
    if (!bmp_ok) {
        bmp280_init();
    }

    int64_t ms = esp_timer_get_time() / 1000;
    char ds0_rom[17] = "NA";
    char ds1_rom[17] = "NA";
    if (s_ds18b20_count > 0) {
        rom_to_hex(s_ds18b20[0].rom, ds0_rom);
    }
    if (s_ds18b20_count > 1) {
        rom_to_hex(s_ds18b20[1].rom, ds1_rom);
    }

    ESP_LOGI(TAG,
             "MASTER_READ ms=%" PRId64 " seq=%" PRIu32
             " hx711_ok=%d hx711_raw=%" PRId32
             " ds_ok=%d ds_count=%d ds0_rom=%s ds0_c=%.3f ds1_rom=%s ds1_c=%.3f"
             " rain_level=%d rain_closed=%d"
             " aht_ok=%d aht_t=%.3f aht_rh=%.3f"
             " bmp_ok=%d bmp_t=%.3f bmp_hpa=%.3f",
             ms, seq, hx_ok ? 1 : 0, hx_raw,
             ds_ok ? 1 : 0, s_ds18b20_count, ds0_rom, ds_t[0], ds1_rom, ds_t[1],
             rain_level, rain_closed ? 1 : 0,
             aht_ok ? 1 : 0, aht_t, aht_rh,
             bmp_ok ? 1 : 0, bmp_t, bmp_hpa);

    printf("CSV,%" PRId64 ",%" PRIu32 ",%d,%" PRId32 ",%d,%d,%.3f,%.3f,%d,%d,%d,%.3f,%.3f,%d,%.3f,%.3f\n",
           ms, seq, hx_ok ? 1 : 0, hx_raw, ds_ok ? 1 : 0, s_ds18b20_count,
           ds_t[0], ds_t[1], rain_level, rain_closed ? 1 : 0,
           aht_ok ? 1 : 0, aht_t, aht_rh, bmp_ok ? 1 : 0, bmp_t, bmp_hpa);
}

void app_main(void)
{
    ESP_LOGI(TAG, "bee_scale master peripheral readout stage starting");
    ESP_LOGI(TAG, "Pins: HX711 SCK=%d DOUT=%d, DS18B20=%d, rain=%d, I2C SDA=%d SCL=%d",
             HX711_SCK_GPIO, HX711_DOUT_GPIO, DS18B20_GPIO, RAIN_SWITCH_GPIO,
             I2C_SDA_GPIO, I2C_SCL_GPIO);

    hx711_init();
    ds18b20_init_gpio();
    rain_switch_init();
    i2c_init_bus();

    ds18b20_refresh_search();
    s_aht20_present = aht20_init();
    bmp280_init();

    printf("CSV_HEADER,ms,seq,hx711_ok,hx711_raw,ds_ok,ds_count,ds0_c,ds1_c,rain_level,rain_closed,aht_ok,aht_t,aht_rh,bmp_ok,bmp_t,bmp_hpa\n");

    uint32_t seq = 0;
    while (true) {
        if (seq > 0 && (seq % 30) == 0 && s_ds18b20_count != DS18B20_EXPECTED_COUNT) {
            ds18b20_refresh_search();
        }
        log_reading(seq++);
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}

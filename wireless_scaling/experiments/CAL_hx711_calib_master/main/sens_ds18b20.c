/*
 * sens_ds18b20.c — DS18B20 1-Wire MULTI-szonda bit-bang driver (MASTER-variáns).
 *
 * EREDET: a prod firmware/master/main/master.c DS18B20-logikája (ow_* bit-bang, ds18b20_search
 * ROM-search, crc8_maxim, match-ROM scratchpad-olvasás) ide MÁSOLVA, a kalibrációs eszköz
 * egyszerűbb API-jára szabva. A prod kódhoz nem nyúltunk.
 *
 * A NODE-variáns egyetlen szondát olvas Skip-ROM-mal; itt EGY buszon 2 (vagy több) szonda van,
 * ezért ROM-search kell, és match-ROM-mal olvassuk ki egyenként. A szerep-mappinget (hive/scale)
 * SZÁNDÉKOSAN nem végezzük — a kalibrációhoz mindkét hőt logoljuk, a scale-t a fit választja ki.
 */
#include "sens_ds18b20.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>     /* NAN */
#include <string.h>   /* memcpy, memcmp */
#include <stdlib.h>   /* qsort */

static const char *TAG = "sens_ds18b20";

typedef struct {
    uint8_t rom[8];
} ds_sensor_t;

static ds_sensor_t s_ds[DS18B20_MAX_SENSORS];
static int s_ds_count = 0;

/* ===================== 1-Wire bit-bang (master.c-ből átemelve) ===================== */

static void ow_drive_low(void) { gpio_set_level(DS18B20_GPIO, 0); }
static void ow_release(void)   { gpio_set_level(DS18B20_GPIO, 1); }

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

static int ds_rom_cmp(const void *a, const void *b)
{
    return memcmp(((const ds_sensor_t *)a)->rom, ((const ds_sensor_t *)b)->rom, 8);
}

/* Dallas 1-Wire SEARCH ROM (0xF0) — a master.c-beli algoritmus. Csak a family-code 0x28
 * (DS18B20) + helyes CRC eszközöket veszi fel; a végén ROM szerint rendez (stabil sorrend). */
static int ds18b20_search(ds_sensor_t *out, int max_count)
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

static void ds18b20_match_rom(const uint8_t rom[8])
{
    ow_write_byte(0x55);
    for (int i = 0; i < 8; i++) ow_write_byte(rom[i]);
}

/* ===================== Publikus API ===================== */

void ds18b20_init(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(DS18B20_GPIO);

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DS18B20_GPIO,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ow_release();

    s_ds_count = ds18b20_search(s_ds, DS18B20_MAX_SENSORS);
    ESP_LOGI(TAG, "ds18b20_init: GPIO%d, found=%d expected=%d",
             DS18B20_GPIO, s_ds_count, DS18B20_EXPECTED_COUNT);
}

int ds18b20_get_count(void)
{
    return s_ds_count;
}

const uint8_t *ds18b20_get_rom(int idx)
{
    if (idx < 0 || idx >= s_ds_count) {
        return NULL;
    }
    return s_ds[idx].rom;
}

bool ds18b20_read_all(float *temps, int max)
{
    for (int i = 0; i < max; i++) temps[i] = NAN;
    if (s_ds_count <= 0) {
        return false;
    }
    if (!ow_reset()) {
        ESP_LOGW(TAG, "DS18B20 nincs jelen a buszon");
        return false;
    }
    /* Skip-ROM Convert T: minden szonda egyszerre konvertál. */
    ow_write_byte(0xCC);
    ow_write_byte(0x44);
    vTaskDelay(pdMS_TO_TICKS(DS18B20_CONVERSION_MS));

    bool all_ok = true;
    int n = s_ds_count < max ? s_ds_count : max;
    for (int i = 0; i < n; i++) {
        if (!ow_reset()) {
            all_ok = false;
            continue;
        }
        ds18b20_match_rom(s_ds[i].rom);
        ow_write_byte(0xBE);   /* Read scratchpad. */
        uint8_t sp[9];
        for (int b = 0; b < 9; b++) sp[b] = ow_read_byte();

        bool all_zero = true, all_ff = true;
        for (int b = 0; b < 9; b++) {
            all_zero = all_zero && sp[b] == 0x00;
            all_ff = all_ff && sp[b] == 0xFF;
        }
        if (all_zero || all_ff) {
            ESP_LOGW(TAG, "DS%d ervenytelen busz-allapot", i);
            all_ok = false;
            continue;
        }
        int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
        float temp_c = (float)raw / 16.0f;
        temps[i] = temp_c;   /* a tényleges számot CRC/range/85 után is megőrizzük */
        if (crc8_maxim(sp, 8) != sp[8] || temp_c < -55.0f || temp_c > 125.0f || temp_c == 85.0f) {
            ESP_LOGW(TAG, "DS%d minoseg-hiba temp=%.2f", i, (double)temp_c);
            all_ok = false;
        }
    }
    return all_ok;
}

/*
 * sens_ds18b20.c — DS18B20 1-Wire bit-bang driver.
 *
 * Nincs külső komponensfüggőség: a rövid node-kábelhez elég a klasszikus GPIO open-drain
 * bit-bang. Blocking olvasás, mert a node ciklusában ez külön mérési fázis, nem UI/task.
 */
#include "sens_ds18b20.h"
#include "node_config.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>   /* NAN */
#include <stdbool.h>
#include <stdint.h>

static const char *TAG = "sens_ds18b20";

static void ow_drive_low(void)
{
#if DS18B20_ENABLED
    gpio_set_level(DS18B20_GPIO, 0);
#endif
}

static void ow_release(void)
{
#if DS18B20_ENABLED
    gpio_set_level(DS18B20_GPIO, 1);
#endif
}

static int ow_read_pin(void)
{
#if DS18B20_ENABLED
    return gpio_get_level(DS18B20_GPIO);
#else
    return 1;
#endif
}

static bool ow_reset(void)
{
    ow_drive_low();
    esp_rom_delay_us(480);
    ow_release();
    esp_rom_delay_us(70);
    bool present = (ow_read_pin() == 0);
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

static void ow_write_byte(uint8_t v)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit((v >> i) & 1);
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (ow_read_bit()) {
            v |= (uint8_t)(1U << i);
        }
    }
    return v;
}

static uint8_t ds18b20_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t inbyte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            inbyte >>= 1;
        }
    }
    return crc;
}

void ds18b20_init(void)
{
#if !DS18B20_ENABLED
    ESP_LOGW(TAG, "ds18b20_init: DS18B20 tiltva (GPIO%d ag kikotve)", DS18B20_GPIO);
    return;
#endif

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << DS18B20_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ow_release();

    ESP_LOGI(TAG, "ds18b20_init: 1-Wire GPIO%d open-drain, presence=%s",
             DS18B20_GPIO, ow_reset() ? "igen" : "nincs");
}

float ds18b20_read_temp(void)
{
#if !DS18B20_ENABLED
    return NAN;
#endif

    if (!ow_reset()) {
        ESP_LOGW(TAG, "DS18B20 nincs jelen a buszon");
        return NAN;
    }

    ow_write_byte(0xCC);   /* Skip ROM: egy szenzor a node-on. */
    ow_write_byte(0x44);   /* Convert T. */
    vTaskDelay(pdMS_TO_TICKS(750));

    if (!ow_reset()) {
        ESP_LOGW(TAG, "DS18B20 eltunt konverzio utan");
        return NAN;
    }

    ow_write_byte(0xCC);
    ow_write_byte(0xBE);   /* Read scratchpad. */

    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }

    uint8_t crc = ds18b20_crc8(sp, 8);
    if (crc != sp[8]) {
        ESP_LOGW(TAG, "DS18B20 CRC hiba: calc=0x%02X got=0x%02X", crc, sp[8]);
        return NAN;
    }

    int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
    return (float)raw / 16.0f;
}

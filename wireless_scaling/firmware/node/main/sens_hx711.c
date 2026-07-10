/*
 * sens_hx711.c — HX711 mérőerősítő valódi bit-bang olvasás + power-path.
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * EREDET: a H2 (h2_hx711_power_path.c, DATA-MONITOR mód) és az M1b (m1b_node.c) éles-mért
 *   bit-bang olvasója, a firmware pin-tervére (node_config.h: PD_SCK=GPIO4, DOUT=GPIO3) igazítva.
 *
 * MŰKÖDÉS
 *   - power_up  : PD_SCK LOW -> a chip aktív, a load cell excitation BE; HX711_SETTLE_MS settle.
 *   - read_raw  : a DOUT LOW (adat kész) után 25 SCK-pulzus -> 24-bit 2's complement (A/gain128).
 *   - read_weight: HX711_SAMPLES nyers minta átlaga float-ként (KALIBRÁLATLAN — nyers ADC-count).
 *   - power_down: PD_SCK HIGH (>60us) -> excitation lekapcsol; a szintet deep sleepre GPIO-holddal
 *                 befagyasztjuk (H2 tanulság: enélkül a chip kiesne power-downból, ~100+ µA).
 *
 * KRITIKUS (M1b tanulság): timer-wake után a deep sleep = reboot, de a PD_SCK-n MÉG AKTÍV a
 *   deep-sleep GPIO-hold (a power_down tette rá). Ha nem oldjuk fel, a set_level nem hat, a chip
 *   power-downban ragad, a DOUT sosem megy LOW -> minden olvasás timeout/0. Ezért hx711_init()
 *   a konfig ELŐTT feloldja a holdot.
 */
#include "sens_hx711.h"
#include <math.h>
#include "node_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us() */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sens_hx711";

void hx711_init(void)
{
    /* Deep-sleep GPIO-hold feloldása (lásd a fájl-fejléc KRITIKUS pontját). */
    gpio_hold_dis(HX711_PD_SCK_GPIO);
    gpio_deep_sleep_hold_dis();

    gpio_config_t pdsck_cfg = {
        .pin_bit_mask = (1ULL << HX711_PD_SCK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pdsck_cfg));

    gpio_config_t dout_cfg = {
        .pin_bit_mask = (1ULL << HX711_DOUT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout_cfg));

    /* Alap: power-down (PD_SCK HIGH), amíg valaki explicit fel nem hozza. */
    gpio_set_level(HX711_PD_SCK_GPIO, 1);
    ESP_LOGI(TAG, "hx711_init: PD_SCK=GPIO%d (out), DOUT=GPIO%d (in)",
             HX711_PD_SCK_GPIO, HX711_DOUT_GPIO);
}

void hx711_power_up(void)
{
    /* A power_down() GPIO-holdot tett a PD_SCK-ra (befagyasztja HIGH-ban, AKTÍV módban is).
     * Enélkül a set_level(0) HATÁSTALAN -> a chip power-downban ragad -> DOUT timeout.
     * (Ez okozta a 2. fázis power_down UTÁNI olvasások — pl. 6. fázis — timeoutjait.) */
    gpio_hold_dis(HX711_PD_SCK_GPIO);
    gpio_deep_sleep_hold_dis();

    gpio_set_level(HX711_PD_SCK_GPIO, 0);          /* LOW = power-up, excitation BE */
    vTaskDelay(pdMS_TO_TICKS(HX711_SETTLE_MS));    /* első stabil konverzióra várunk */
}

void hx711_power_down(void)
{
    gpio_set_level(HX711_PD_SCK_GPIO, 1);          /* HIGH >60us = power-down */
    esp_rom_delay_us(100);
    /* A HIGH szint deep sleep alatt is maradjon meg (excitation kikapcsolva) — H2 tanulság. */
    gpio_hold_en(HX711_PD_SCK_GPIO);
    gpio_deep_sleep_hold_en();
}

bool hx711_read_raw_checked(int32_t *raw_out)
{
    /* Várjuk a DOUT LOW-t (adat kész). Timeout, hogy modul/bekötés nélkül ne ragadjon be. */
    int guard = 0;
    while (gpio_get_level(HX711_DOUT_GPIO) == 1) {
        esp_rom_delay_us(100);
        if (++guard > 5000) {   /* ~0.5 s */
            ESP_LOGW(TAG, "HX711 DOUT timeout (nem kesz / nincs modul / rossz bekotes?)");
            if (raw_out) {
                *raw_out = 0;
            }
            return false;
        }
    }

    uint32_t value = 0;
    for (int i = 0; i < 24; i++) {
        gpio_set_level(HX711_PD_SCK_GPIO, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (uint32_t)gpio_get_level(HX711_DOUT_GPIO);
        gpio_set_level(HX711_PD_SCK_GPIO, 0);
        esp_rom_delay_us(1);
    }

    /* 25. pulzus: gain = 128 (A csatorna) a következő konverzióhoz. */
    gpio_set_level(HX711_PD_SCK_GPIO, 1);
    esp_rom_delay_us(1);
    gpio_set_level(HX711_PD_SCK_GPIO, 0);
    esp_rom_delay_us(1);

    /* 24-bit 2's complement -> előjel-kiterjesztés 32-bitre. */
    if (value & 0x800000U) {
        value |= 0xFF000000U;
    }
    if (raw_out) {
        *raw_out = (int32_t)value;
    }
    return true;
}

int32_t hx711_read_raw(void)
{
    int32_t raw = 0;
    hx711_read_raw_checked(&raw);
    return raw;
}

static void sort_i32(int32_t *values, int n)
{
    for (int i = 1; i < n; i++) {
        int32_t key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
}

bool hx711_read_weight_checked(float *weight_out)
{
    int32_t samples[HX711_MEDIAN_SAMPLES];
    for (int i = 0; i < HX711_MEDIAN_SAMPLES; i++) {
        if (!hx711_read_raw_checked(&samples[i])) {
            if (weight_out) {
                *weight_out = NAN;
            }
            return false;
        }
    }

    sort_i32(samples, HX711_MEDIAN_SAMPLES);
    int32_t min = samples[0];
    int32_t max = samples[HX711_MEDIAN_SAMPLES - 1];
    int32_t span = max - min;

    float weight = 0.0f;
    if ((HX711_MEDIAN_SAMPLES % 2) == 0) {
        int hi = HX711_MEDIAN_SAMPLES / 2;
        int lo = hi - 1;
        weight = (float)(((int64_t)samples[lo] + (int64_t)samples[hi]) / 2.0);
    } else {
        weight = (float)samples[HX711_MEDIAN_SAMPLES / 2];
    }

    ESP_LOGI(TAG, "HX711 samples n=%u min=%ld median=%.0f max=%ld span=%ld",
             (unsigned)HX711_MEDIAN_SAMPLES, (long)min, (double)weight,
             (long)max, (long)span);

    if (min == 0 && max == 0) {
        ESP_LOGW(TAG, "HX711 gyanus: minden nyers minta 0 (DOUT stuck-low / rossz bekotes / nincs ertelmes ADC adat)");
        if (weight_out) {
            *weight_out = NAN;
        }
        return false;
    }
    if (min == -1 && max == -1) {
        ESP_LOGW(TAG, "HX711 gyanus: minden nyers minta -1 (DOUT stuck-high / busz hiba)");
        if (weight_out) {
            *weight_out = NAN;
        }
        return false;
    }
    if (span == 0) {
        ESP_LOGW(TAG, "HX711 gyanus: minden nyers minta azonos (%ld)", (long)min);
    }

    if (weight_out) {
        /* Kalibráció még nincs -> a nyers mediánt adjuk vissza float-ként. */
        *weight_out = weight;
    }
    return true;
}

float hx711_read_weight(void)
{
    float weight = NAN;
    hx711_read_weight_checked(&weight);
    return weight;
}

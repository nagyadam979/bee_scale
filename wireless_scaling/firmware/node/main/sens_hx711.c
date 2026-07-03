/*
 * sens_hx711.c — lásd sens_hx711.h.
 * STUB: az interfész él, a valós bit-bang olvasás + power-path a H2 h2_hx711_power_path.c-ből
 * kerül be a modul-feltöltéskor. Egyelőre logol + dummy értéket ad.
 */
#include "sens_hx711.h"
#include "node_config.h"
#include "esp_log.h"

static const char *TAG = "sens_hx711";

void hx711_init(void)
{
    ESP_LOGW(TAG, "[stub] hx711_init (PD_SCK=GPIO%d, DOUT=GPIO%d) — TODO: GPIO config (H2-bol)",
             HX711_PD_SCK_GPIO, HX711_DOUT_GPIO);
}

void hx711_power_up(void)
{
    ESP_LOGW(TAG, "[stub] hx711_power_up (PD_SCK LOW) — TODO");
}

void hx711_power_down(void)
{
    ESP_LOGW(TAG, "[stub] hx711_power_down (PD_SCK HIGH) — TODO");
}

int32_t hx711_read_raw(void)
{
    return 0;   /* TODO: H2 bit-bang (25 SCK, A/gain128, 2's complement) */
}

float hx711_read_weight(void)
{
    ESP_LOGW(TAG, "[stub] hx711_read_weight (avg %d) — TODO", HX711_SAMPLES);
    return 0.0f;
}

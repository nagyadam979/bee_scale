/*
 * sens_vbat.c — lásd sens_vbat.h.
 * STUB: az interfész él, a valós NPN-kapcsolás + adc_oneshot + kalibráció + osztó-számítás a
 * modul-feltöltéskor kerül be. Egyelőre logol + dummy mV-t ad.
 */
#include "sens_vbat.h"
#include "node_config.h"
#include "esp_log.h"

static const char *TAG = "sens_vbat";

void vbat_init(void)
{
    ESP_LOGW(TAG, "[stub] vbat_init (SW=GPIO%d, ADC1 ch%d) — TODO: GPIO + adc_oneshot + cali",
             VBAT_SW_GPIO, VBAT_ADC_CHANNEL);
}

void vbat_enable(void)
{
    ESP_LOGW(TAG, "[stub] vbat_enable (NPN ON) — TODO");
}

void vbat_disable(void)
{
    ESP_LOGW(TAG, "[stub] vbat_disable (NPN OFF) — TODO");
}

uint16_t vbat_sample_mv(void)
{
    ESP_LOGW(TAG, "[stub] vbat_sample_mv (avg %d, ratio %d) — TODO: adc olvasas",
             VBAT_ADC_SAMPLES, VBAT_DIVIDER_RATIO);
    return 0;
}

uint16_t vbat_read_mv(void)
{
    ESP_LOGW(TAG, "[stub] vbat_read_mv (enable+settle %d ms+sample+disable) — TODO", VBAT_SETTLE_MS);
    return 0;
}

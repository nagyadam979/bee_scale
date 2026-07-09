/*
 * sens_vbat.c — akkufeszültség mérés: NPN-kapcsolt feszültségosztó + ADC1 oneshot + kalibráció.
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * A pinek/paraméterek a node_config.h-ból (a bekötés-tweak commitok után):
 *   - osztó: 10k/10k (VBAT_DIVIDER_RATIO=2), a TP4056 OUT+ (akkusín) felezve az ADC-re.
 *   - ADC:   ADC1 (VBAT_ADC_UNIT), csatorna VBAT_ADC_CHANNEL (GPIO0), atten VBAT_ADC_ATTEN (12 dB).
 *            A C3-on CSAK az ADC1 megbízható WiFi mellett (az ADC2 oneshot HW-korlátos) — ezért ADC1.
 *   - NPN low-side kapcsoló bázisa: VBAT_SW_GPIO (GPIO10). HIGH = osztó él, LOW = ~0 szivárgás.
 *
 * MÉRÉS: enable -> VBAT_SETTLE_MS beállás -> VBAT_ADC_SAMPLES nyers minta átlaga -> kalibrált mV
 *   -> * VBAT_DIVIDER_RATIO (vissza az akku mV-jára) -> disable. Kalibráció: a C3 curve-fitting
 *   sémát támogatja; ha nem áll rendelkezésre, durva lineáris nyers->mV közelítésre esünk vissza.
 */
#include "sens_vbat.h"
#include "node_config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sens_vbat";

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_ok = false;

void vbat_init(void)
{
    gpio_hold_dis(VBAT_SW_GPIO);

    /* NPN-kapcsoló GPIO: output, alap LOW (osztó lekapcsolva). Akkor is így hagyjuk,
     * ha a VBAT mérés tiltott, mert ez a fogyasztásbarát állapot. */
    gpio_config_t sw_cfg = {
        .pin_bit_mask = (1ULL << VBAT_SW_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&sw_cfg));
    gpio_set_level(VBAT_SW_GPIO, 0);

#if !VBAT_MEASURE_ENABLED
    ESP_LOGW(TAG, "vbat_init: VBAT meres tiltva (oszto le van kotve GPIO0-rol)");
    return;
#endif

    /* ADC1 oneshot unit + csatorna. */
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = VBAT_ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = VBAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chan_cfg));

    /* Kalibráció (curve fitting a C3-on). Hiba esetén durva közelítésre esünk vissza. */
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = VBAT_ADC_UNIT,
        .chan     = VBAT_ADC_CHANNEL,
        .atten    = VBAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    } else {
        ESP_LOGW(TAG, "ADC kalibracio nem elerheto -> nyers->mV kozelites");
    }

    ESP_LOGI(TAG, "vbat_init: SW=GPIO%d, ADC1 ch%d, atten=%d, ratio=%d, cali=%s",
             VBAT_SW_GPIO, VBAT_ADC_CHANNEL, VBAT_ADC_ATTEN, VBAT_DIVIDER_RATIO,
             s_cali_ok ? "igen" : "nem");
}

void vbat_enable(void)
{
#if !VBAT_MEASURE_ENABLED
    return;
#endif
    gpio_set_level(VBAT_SW_GPIO, 1);   /* NPN ON -> osztó él */
}

void vbat_disable(void)
{
    gpio_set_level(VBAT_SW_GPIO, 0);   /* NPN OFF -> ~0 szivárgás */
}

uint16_t vbat_sample_mv(void)
{
#if !VBAT_MEASURE_ENABLED
    return 0;
#endif

    int64_t acc = 0;
    int n = 0;
    for (int i = 0; i < VBAT_ADC_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw) == ESP_OK) {
            acc += raw;
            n++;
        }
    }
    if (n == 0) {
        ESP_LOGW(TAG, "ADC olvasas hiba (minden minta sikertelen)");
        return 0;
    }
    int raw_avg = (int)(acc / n);

    int mv = 0;
    if (s_cali_ok) {
        adc_cali_raw_to_voltage(s_cali, raw_avg, &mv);
    } else {
        /* Durva közelítés 12 dB atten / 12-bit: ~0..2500 mV a 0..4095 raw-ra. */
        mv = (int)((int64_t)raw_avg * 2500 / 4095);
    }
    return (uint16_t)(mv * VBAT_DIVIDER_RATIO);   /* vissza az akku mV-jára */
}

uint16_t vbat_read_mv(void)
{
    vbat_enable();
    vTaskDelay(pdMS_TO_TICKS(VBAT_SETTLE_MS));   /* RC-beállás a bekapcsolás után */
    uint16_t mv = vbat_sample_mv();
    vbat_disable();
    return mv;
}

void vbat_prepare_sleep(void)
{
    vbat_disable();

#if VBAT_MEASURE_ENABLED
    if (s_cali_ok && s_cali) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
        s_cali_ok = false;
    }

    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }

    gpio_config_t adc_cfg = {
        .pin_bit_mask = (1ULL << VBAT_ADC_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&adc_cfg));
#endif

#if VBAT_MEASURE_ENABLED
    ESP_LOGI(TAG, "vbat sleep: ADC unit torolve, SW(GPIO%d)=LOW, ADC(GPIO%d)=input/no-pull",
             VBAT_SW_GPIO, VBAT_ADC_GPIO);
#else
    ESP_LOGI(TAG, "vbat sleep: VBAT meres tiltva, SW(GPIO%d)=LOW", VBAT_SW_GPIO);
#endif
}

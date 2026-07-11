/*
 * esp_adc/adc_oneshot.h — host-oldali stub (node_queue_sim tesztpad).
 * Csak a node_config.h ADC makróihoz kellő enum értékek.
 */
#ifndef MOCK_ESP_ADC_ONESHOT_H
#define MOCK_ESP_ADC_ONESHOT_H

typedef enum {
    ADC_UNIT_1 = 0,
    ADC_UNIT_2 = 1,
} adc_unit_t;

typedef enum {
    ADC_CHANNEL_0 = 0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3, ADC_CHANNEL_4,
    ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_7, ADC_CHANNEL_8, ADC_CHANNEL_9,
} adc_channel_t;

typedef enum {
    ADC_ATTEN_DB_0 = 0,
    ADC_ATTEN_DB_2_5 = 1,
    ADC_ATTEN_DB_6 = 2,
    ADC_ATTEN_DB_12 = 3,
} adc_atten_t;

#endif /* MOCK_ESP_ADC_ONESHOT_H */

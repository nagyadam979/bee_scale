/*
 * sens_ds18b20.c — lásd sens_ds18b20.h.
 * STUB: az interfész él, a valós 1-Wire implementáció a modul-feltöltéskor kerül be
 * (bit-bang vagy RMT-alapú — a döntés a feltöltéskor). Egyelőre logol + NAN-t ad.
 */
#include <math.h>   /* NAN */
#include "sens_ds18b20.h"
#include "node_config.h"
#include "esp_log.h"

static const char *TAG = "sens_ds18b20";

void ds18b20_init(void)
{
    ESP_LOGW(TAG, "[stub] ds18b20_init (1-Wire GPIO%d) — TODO: 1-Wire reset/presence", DS18B20_GPIO);
}

float ds18b20_read_temp(void)
{
    ESP_LOGW(TAG, "[stub] ds18b20_read_temp — TODO: convert T + read scratchpad");
    return NAN;
}

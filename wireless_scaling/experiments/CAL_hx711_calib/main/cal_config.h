/*
 * cal_config.h — a HX711 kalibrációs firmware (NODE-variáns) paraméterei egy helyen.
 * bee_scale / wireless_scaling · 2. fázis · experiments/CAL_hx711_calib
 *
 * ÖNÁLLÓ eszköz: a prod firmware/node olvasói (sens_hx711.c, sens_ds18b20.c) ide MÁSOLVA
 * lettek, és INNEN (nem a prod node_config.h-ból) veszik a pineket/paramétereket. A prod
 * kódhoz nem nyúlunk. A define-nevek szándékosan azonosak a prod-éval, hogy a másolt .c-k
 * változtatás nélkül forduljanak.
 *
 * BEKÖTÉS = NODE (ESP32-C3 SuperMini): HX711 PD_SCK=GPIO4, DOUT=GPIO3; DS18B20=GPIO9.
 * A master (S3-Zero) bekötése ELTÉR (SCK=GPIO2, DOUT=GPIO4, DS18B20=GPIO5) — az külön variáns.
 */
#ifndef CAL_CONFIG_H
#define CAL_CONFIG_H

#include "driver/gpio.h"

/* ===================== HX711 (NODE bekötés) ===================== */
#define HX711_PD_SCK_GPIO    GPIO_NUM_4   /* PD_SCK: HIGH>60us = power-down; olvasáskor SCK */
#define HX711_DOUT_GPIO      GPIO_NUM_3   /* DOUT: adat (input), LOW = konverzió kész */

/* Hány nyers mintából képezzünk mediánt egy leolvasáshoz (zaj-csillapítás). */
#define HX711_MEDIAN_SAMPLES 12
/* Régi BENCH API-kompatibilitás (a másolt sens_hx711.c ezt is használja). */
#define HX711_SAMPLES        HX711_MEDIAN_SAMPLES
/* Power-up után az első stabil konverzióra várt idő (ms). */
#define HX711_SETTLE_MS      50

/* ===================== DS18B20 (mérleg-hőm, scale_c) ===================== */
#define DS18B20_ENABLED       1
#define DS18B20_GPIO          GPIO_NUM_9
#define DS18B20_CONVERSION_MS 750

/* ===================== Kék user LED (életjel a hő-logban) ===================== */
#define USER_LED_GPIO         GPIO_NUM_8   /* tipikusan aktív-alacsony */

/* ===================== Kalibrációs firmware paraméterei ===================== */
/* Boot utáni figyelő-ablak: ha ezalatt jön parancs-karakter -> INTERAKTÍV; különben AUTOSTART
 * hő-logolás (off-grid eset, nincs host). */
#define CAL_BOOT_LISTEN_MS        10000
/* Hő-logoló mintavételi periódus (ms). 15 s @ 256 KiB log ~ 68 óra. */
#define CAL_TEMP_LOG_INTERVAL_MS  15000
/* Súly-stream sor-ütem (ms) — a DS18B20 750 ms konverziója miatt a tényleges ütem ~1-3 Hz. */
#define CAL_WSTREAM_INTERVAL_MS   300
/* A súly-streamben a scale_c-t ennyi ms-enként frissítjük (a raw gyorsabban jön). */
#define CAL_WSTREAM_TEMP_REFRESH_MS 2000

#endif /* CAL_CONFIG_H */

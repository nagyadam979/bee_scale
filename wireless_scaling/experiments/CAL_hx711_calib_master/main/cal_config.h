/*
 * cal_config.h — a HX711 kalibrációs firmware (MASTER-variáns) paraméterei egy helyen.
 * bee_scale / wireless_scaling · 2. fázis · experiments/CAL_hx711_calib_master
 *
 * ÖNÁLLÓ eszköz: a prod firmware/master DS18B20 + HX711 olvasói ide MÁSOLVA/ÁTEMELVE lettek,
 * és INNEN (nem a prod master.c define-jaiból) veszik a pineket/paramétereket. A prod kódhoz
 * nem nyúlunk.
 *
 * BEKÖTÉS = MASTER (ESP32-S3-Zero): HX711 PD_SCK=GPIO2, DOUT=GPIO4; DS18B20 busz=GPIO5.
 * A node (C3 SuperMini) bekötése ELTÉR (SCK=4, DOUT=3, DS=9) — az a CAL_hx711_calib variáns.
 *
 * KÜLÖNBSÉG A NODE-HOZ KÉPEST: a masteren 2 DS18B20 van EGY 1-Wire buszon (kaptár-hő + mérleg-
 * cella-hő), ROM-cím szerint. A kalibrációs firmware MINDKÉT szondát megkeresi (ROM-search) és
 * MINDKETTŐ hőjét logolja — a "scale" szerepű szondát a felhő-oldali fit választja ki a bootkor
 * kiírt "# DS ROLES" ROM-lista alapján.
 */
#ifndef CAL_CONFIG_H
#define CAL_CONFIG_H

#include "driver/gpio.h"

/* ===================== HX711 (MASTER bekötés) ===================== */
#define HX711_PD_SCK_GPIO    GPIO_NUM_2   /* PD_SCK: HIGH>60us = power-down; olvasáskor SCK */
#define HX711_DOUT_GPIO      GPIO_NUM_4   /* DOUT: adat (input), LOW = konverzió kész */

/* Hány nyers mintából képezzünk mediánt egy leolvasáshoz (zaj-csillapítás). */
#define HX711_MEDIAN_SAMPLES 12
/* Régi BENCH API-kompatibilitás (a másolt sens_hx711.c ezt is használja). */
#define HX711_SAMPLES        HX711_MEDIAN_SAMPLES
/* Power-up után az első stabil konverzióra várt idő (ms). */
#define HX711_SETTLE_MS      50

/* ===================== DS18B20 (2 szonda egy buszon) ===================== */
#define DS18B20_ENABLED       1
#define DS18B20_GPIO          GPIO_NUM_5
#define DS18B20_CONVERSION_MS 750
/* A ROM-search legfeljebb ennyi szondát tárol; a masteren 2 várható (hive + scale). */
#define DS18B20_MAX_SENSORS   4
#define DS18B20_EXPECTED_COUNT 2

/* ===================== LED (életjel a hő-logban) ===================== */
/* Az S3-Zero-n a felhasználói LED egy WS2812 RGB (GPIO21), NEM sima GPIO-LED. Ezért a sima
 * GPIO-alapú led_pulse-ot kikapcsoljuk (no-op); az off-grid életjel maga a soros TLG sor. */
#define CAL_HAS_SIMPLE_LED    0

/* ===================== Kalibrációs firmware paraméterei ===================== */
/* Boot utáni figyelő-ablak: ha ezalatt jön parancs-karakter -> INTERAKTÍV; különben AUTOSTART
 * hő-logolás (off-grid eset, nincs host). */
#define CAL_BOOT_LISTEN_MS        10000
/* Hő-logoló mintavételi periódus (ms). 15 s @ 256 KiB / 20 B ~ 13100 rekord ~ 54 óra. */
#define CAL_TEMP_LOG_INTERVAL_MS  15000
/* Súly-stream sor-ütem (ms) — a DS18B20 750 ms konverziója miatt a tényleges ütem ~1-3 Hz. */
#define CAL_WSTREAM_INTERVAL_MS   300
/* A súly-streamben a hőt ennyi ms-enként frissítjük (a raw gyorsabban jön). */
#define CAL_WSTREAM_TEMP_REFRESH_MS 2000

#endif /* CAL_CONFIG_H */

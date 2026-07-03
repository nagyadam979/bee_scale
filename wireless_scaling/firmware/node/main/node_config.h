/*
 * node_config.h — a node-firmware ÖSSZES hangolható paramétere egy helyen
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * CLAUDE.md elv: a hangolható paraméterek a fájl tetején/egy helyen, kommentálva. A modulok
 * (sens_*, espnow_tx, node_sleep, main) INNEN veszik a pineket, időzítést, profilt.
 *
 * PROFIL — a firmware fő kapcsolója (mint a H2 H2_MEASURE_MODE):
 *   NODE_PROFILE_BENCH : 1. tesztfázis. ÉBRENLÉT fázis-szekvencerrel — minden alrendszer
 *                        KÜLÖN, hosszú, egyenletes szakaszban aktív, hogy a DMM-en
 *                        komponensenként lépcső látszódjon (melyik mennyit húz), közben a
 *                        kommunikáció is terhelhető. 2 perc ébren / 2 perc alvás (szimmetrikus).
 *   NODE_PROFILE_FIELD : éles üzem. Egyszeri gyors mérés+küldés, majd hosszú deep sleep (~20 perc).
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* ===================== PROFIL ===================== */
#define NODE_PROFILE_BENCH   0
#define NODE_PROFILE_FIELD   1

/* Itt váltasz profilt (vagy -D NODE_PROFILE=... build-flaggel). Default: BENCH. */
#ifndef NODE_PROFILE
#define NODE_PROFILE         NODE_PROFILE_BENCH
#endif

/* ===================== NODE azonosító / rádió ===================== */

/* A node azonosítója (docs: unit_id). Több node esetén EGYEDI. */
#define NODE_ID              1

/* WiFi/ESP-NOW csatorna. EGYEZZEN a master csatornájával! (1..13) */
#define NODE_WIFI_CHANNEL    1

/* Egy payloadot ennyiszer küld el back-to-back — olcsó broadcast-redundancia (Sz2/M1 minta). */
#define NODE_TX_REPEAT       3

/* ===================== IDŐZÍTÉS — profilfüggő ===================== */
#if NODE_PROFILE == NODE_PROFILE_BENCH
  /* Alvási idő két ébrenlét közt (s). Szimmetrikus a 2 perc ébrenléttel. */
  #define NODE_WAKE_PERIOD_SEC   120
  /* Egy BENCH-fázis hossza (s) — ennyi ideig aktív EGY alrendszer, hogy a DMM tisztán mérje.
   * A H2-ben 10 s már tiszta volt; 20 s bőven stabil átlagot ad. 6 fázis × 20 s = 120 s. */
  #define BENCH_PHASE_SEC        20
  /* Az ESPNOW_TX fázisban ilyen gyakran küld egy payloadot (ms) — folyamatos link-terhelés. */
  #define BENCH_TX_INTERVAL_MS   2000
  /* A HX711 / VBAT / DS18B20 fázisban ilyen gyakran mintavételez (ms) — folyamatos aktivitás. */
  #define BENCH_SAMPLE_INTERVAL_MS 500
#else /* NODE_PROFILE_FIELD */
  /* Éles alvási periódus (s). ~20 perc (a végleges érték a 2-node kísérletből jön). */
  #define NODE_WAKE_PERIOD_SEC   1200
#endif

/* ===================== PIN-TERV (ESP32-C3 SuperMini) ===================== */
/* Kivezetve: GPIO0..10, 20, 21. ADC-képes: GPIO0..4. Strapping (kerülendő): GPIO2, 8, 9. */

/* HX711 — a CASINGBE ÉPÍTETT mérleg tényleges bekötése (NEM a H1/H2 GPIO5/6!): a fizikai
 * huzalozás miatt SCK=GPIO4, DT=GPIO3. Mindkettő a VDD3P3_RTC tartományban (GPIO0..5) -> a
 * PD_SCK deep-sleep HIGH-hold (gpio_hold) működik. NE változtasd (a HW már így van kötve). */
#define HX711_PD_SCK_GPIO    GPIO_NUM_4   /* PD_SCK: HIGH>60us = power-down; olvasáskor SCK */
#define HX711_DOUT_GPIO      GPIO_NUM_3   /* DOUT: adat (input), LOW = konverzió kész */

/* vbat feszültségosztó ADC + NPN low-side kapcsoló.
 * A C3-on CSAK az ADC1 (GPIO0..4) megbízható: az ADC2 (GPIO5) oneshot módban HW-korlátos +
 * WiFi-vel ütközik (Espressif dok/errata) -> a vbat ADC kényszerűen a 0..4 oldalra kerül.
 * Foglalt: GPIO4(SCK), GPIO3(DT), GPIO2 strapping -> vbat ADC = GPIO0 (ADC1_CH0), fizikailag
 * elérhető. A NPN-kapcsoló (digitális) az elérhető 5..10 oldalon (GPIO7). */
#define VBAT_ADC_GPIO        GPIO_NUM_0   /* ADC1 bemenet (info; a driver a csatornát használja) */
#define VBAT_ADC_UNIT        ADC_UNIT_1   /* ADC1 — WiFi mellett is megbízható (ADC2 nem!) */
#define VBAT_ADC_CHANNEL     ADC_CHANNEL_0/* GPIO0 == ADC1 csatorna 0 a C3-on */
#define VBAT_ADC_ATTEN       ADC_ATTEN_DB_12 /* ~0..2.5 V lineáris (a leosztott ~2.1 V belefér) */
#define VBAT_SW_GPIO         GPIO_NUM_7   /* NPN bázis (10k-n át); HIGH = osztó él, LOW = lekapcsol */

/* DS18B20 1-Wire adatláb (+ 4.7k felhúzó a HW-en). */
#define DS18B20_GPIO         GPIO_NUM_10

/* Beépített kék user LED (a mérés alatt kikapcsolva tartjuk; tipikusan aktív-alacsony). */
#define USER_LED_GPIO        GPIO_NUM_8

/* ===================== vbat osztó paraméterek ===================== */
/* Osztásarány: (R_top + R_bot) / R_bot. 100k/100k -> 2. A nyers mV-t ennyivel szorozzuk. */
#define VBAT_DIVIDER_RATIO   2
/* RC-beállás a 100 nF ADC-pufferre (NPN-bekapcsolás UTÁN várunk ennyit mérés előtt). */
#define VBAT_SETTLE_MS       30
/* Hány ADC-mintát átlagoljunk egy leolvasáshoz (zaj-csillapítás). */
#define VBAT_ADC_SAMPLES     16

/* ===================== HX711 paraméterek ===================== */
/* Hány nyers mintát átlagoljunk egy súly-leolvasáshoz. */
#define HX711_SAMPLES        8
/* Power-up után ennyit várunk az első stabil konverzióra (ms). */
#define HX711_SETTLE_MS      50

#endif /* NODE_CONFIG_H */

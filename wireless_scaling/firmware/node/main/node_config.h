/*
 * node_config.h — a node-firmware ÖSSZES hangolható paramétere egy helyen
 * bee_scale / wireless_scaling · 2. fázis · firmware/node
 *
 * CLAUDE.md elv: a hangolható paraméterek a fájl tetején/egy helyen, kommentálva. A modulok
 * (sens_*, espnow_tx, node_sleep, main) INNEN veszik a pineket, időzítést, profilt.
 *
 * PROFIL — a firmware fő kapcsolója (mint a H2 H2_MEASURE_MODE):
 *   NODE_PROFILE_BENCH : 1. tesztfázis. ÉBRENLÉT fázis-szekvencerrel — minden alrendszer
 *                        hosszú, egyenletes szakaszokban fut: baseline, összes szenzor együtt,
 *                        rádió-idle, ESP-NOW TX. A default fázislista 4 x 2.5 s = 10 s.
 *   NODE_PROFILE_FIELD : éles üzem. Egyszeri gyors mérés+küldés, majd hosszú deep sleep (~20 perc).
 */
#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* ===================== PROFIL ===================== */
#define NODE_PROFILE_BENCH   0
#define NODE_PROFILE_FIELD   1
#define NODE_PROFILE_SLEEP_FLOOR 2

/* Itt váltasz profilt (vagy -D NODE_PROFILE=... build-flaggel). Default: BENCH. */
#ifndef NODE_PROFILE
#define NODE_PROFILE         NODE_PROFILE_BENCH
#endif

/* ===================== NODE azonosító / rádió ===================== */

/* A node azonosítója (docs: unit_id). Több node esetén EGYEDI. */
#define NODE_ID              1

/* WiFi/ESP-NOW csatorna. EGYEZZEN a master/AP csatornájával! (1..13)
 * AMS-TP-2.4GHz a tesztkörnyezetben: ch9. */
#define NODE_WIFI_CHANNEL    9

/* Egy payloadot ennyiszer küld el back-to-back — olcsó broadcast-redundancia (Sz2/M1 minta). */
#define NODE_TX_REPEAT       3

/* ===================== IDŐZÍTÉS — profilfüggő ===================== */
#if NODE_PROFILE == NODE_PROFILE_BENCH
  /* Alvási idő két ébrenlét közt (s). */
  #define NODE_WAKE_PERIOD_SEC   60
  /* Egy BENCH-fázis hossza (s) — ennyi ideig aktív az adott mérési blokk, hogy a DMM tisztán mérje.
   * Gyors körméréshez: 4 fázis × 2.5 s = 10 s ébrenlét. */
  #define BENCH_PHASE_MS         2500
  /* Az ESPNOW_TX fázisban ilyen gyakran küld egy payloadot (ms) — folyamatos link-terhelés. */
  #define BENCH_TX_INTERVAL_MS   2000
  /* A közös szenzor-fázisban ilyen gyakran indul új mintavétel. DS18B20 miatt ~750 ms blokkol. */
  #define BENCH_SAMPLE_INTERVAL_MS 1000
#elif NODE_PROFILE == NODE_PROFILE_FIELD
  /* Éles alvási periódus (s). ~20 perc (a végleges érték a 2-node kísérletből jön). */
  #define NODE_WAKE_PERIOD_SEC   1200
#else /* NODE_PROFILE_SLEEP_FLOOR */
  /* Mérési izoláció: minimál firmware, csak deep sleep floor. */
  #define NODE_WAKE_PERIOD_SEC   60
#endif

/* ===================== PIN-TERV (ESP32-C3 SuperMini) ===================== */
/* Kivezetve: GPIO0..10, 20, 21. ADC-képes: GPIO0..4. Strapping (kerülendő): GPIO2, 8, 9. */

/* HX711 — a ténylegesen bekötött mérleg-erősítő lábak.
 * A sleep-floor diagnosztika igazolta: PD_SCK GPIO5-ön van, nem GPIO4-en. */
#define HX711_PD_SCK_GPIO    GPIO_NUM_5   /* PD_SCK: HIGH>60us = power-down; olvasáskor SCK */
#define HX711_DOUT_GPIO      GPIO_NUM_6   /* DOUT: adat (input), LOW = konverzió kész */

/* vbat feszültségosztó ADC + NPN low-side kapcsoló.
 * A C3-on CSAK az ADC1 (GPIO0..4) megbízható: az ADC2 (GPIO5) oneshot módban HW-korlátos +
 * WiFi-vel ütközik (Espressif dok/errata) -> a vbat ADC kényszerűen a 0..4 oldalra kerül.
 * Foglalt: GPIO5(SCK), GPIO6(DT), GPIO2 strapping -> vbat ADC = GPIO0 (ADC1_CH0), fizikailag
 * elérhető. A NPN-kapcsoló (digitális) az elérhető 5..10 oldalon (GPIO10).
 *
 * 2026-07-09: a low-side kapcsolt osztó GPIO0-ról le lett kötve, mert OFF állapotban a
 * mérőcsomópont VBAT-ra úszott. A VBAT mérés ezért firmware-ből tiltva van. */
#define VBAT_MEASURE_ENABLED 0
#define VBAT_ADC_GPIO        GPIO_NUM_0   /* ADC1 bemenet (info; a driver a csatornát használja) */
#define VBAT_ADC_UNIT        ADC_UNIT_1   /* ADC1 — WiFi mellett is megbízható (ADC2 nem!) */
#define VBAT_ADC_CHANNEL     ADC_CHANNEL_0/* GPIO0 == ADC1 csatorna 0 a C3-on */
#define VBAT_ADC_ATTEN       ADC_ATTEN_DB_12 /* ~0..2.5 V lineáris (a leosztott ~2.1 V belefér) */
#define VBAT_SW_GPIO         GPIO_NUM_10  /* NPN bázis (10k-n át); HIGH = osztó él, LOW = lekapcsol */

/* DS18B20 1-Wire. A GPIO9 alvásban explicit HIGH + hold, hogy a 4.7k felhúzó
 * ne tudjon LOW-ra hajtott padon át ~700 uA-t elfűteni. */
#define DS18B20_ENABLED      0
#define DS18B20_HOLD_HIGH_IN_SLEEP 1
#define DS18B20_GPIO         GPIO_NUM_9

/* Beépített kék user LED (a mérés alatt kikapcsolva tartjuk; tipikusan aktív-alacsony). */
#define USER_LED_GPIO        GPIO_NUM_8

/* ===================== vbat osztó paraméterek ===================== */
/* Osztásarány: (R_top + R_bot) / R_bot. 10k/10k -> 2. A nyers mV-t ennyivel szorozzuk.
 * 10k/10k: Thévenin 5 kΩ (< Espressif ~10 kΩ ajánlás) -> az ADC pontosan mér KONDI NÉLKÜL is.
 * A kapcsolt NPN miatt alvásban 0 szivárgás; a mérés-alatti ~210 µA elhanyagolható. */
#define VBAT_DIVIDER_RATIO   2
/* Beállás a NPN-bekapcsolás UTÁN, mérés előtt. Kondi NÉLKÜL a beállás <1 ms; 10 ms biztonságos
 * ráhagyás. (Ha később 100 nF ADC-puffert teszel be, ez nyugodtan maradhat.) */
#define VBAT_SETTLE_MS       10
/* Hány ADC-mintát átlagoljunk egy leolvasáshoz (zaj-csillapítás). */
#define VBAT_ADC_SAMPLES     16
/*
 * KÁRMENTÉS a low-side kapcsolt osztóhoz, ha még visszakötnénk:
 *   VBAT -> 10k -> GPIO0/ADC -> 10k -> NPN -> GND
 * Ha az NPN alvásban OFF, GPIO0 a felső 10k-n át VBAT-ra (~3.9-4.2 V) úszik, ami rosszabb,
 * mint a stabil, bekapcsolt osztó. Ezért deep sleepben bekapcsolva hagyjuk az osztót:
 * GPIO0 így VBAT/2-t lát, ára kb. 4.2 V / 20k = 210 uA.
 */
#define VBAT_KEEP_DIVIDER_ON_IN_SLEEP  0

/* ===================== HX711 paraméterek ===================== */
/* Hány nyers mintát átlagoljunk egy súly-leolvasáshoz. */
#define HX711_SAMPLES        8
/* Power-up után ennyit várunk az első stabil konverzióra (ms). */
#define HX711_SETTLE_MS      50

#endif /* NODE_CONFIG_H */

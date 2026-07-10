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
#ifndef NODE_ID
#define NODE_ID              1
#endif

/* WiFi/ESP-NOW csatorna. A master STA kapcsolata miatt az AP/router fixen ch6. */
#ifndef NODE_WIFI_CHANNEL
#define NODE_WIFI_CHANNEL    6
#endif

#ifndef NODE_TEST_MODE
#define NODE_TEST_MODE       0
#endif

/*
 * Hideginditas/power-cycle utani szervizablak. Csak nem timer-wakeup bootnal fut:
 * a node 20 s-ig ebren marad WiFi nelkul, HX711 power-downban, hogy az USB port
 * stabilan lathato legyen firmware feltolteshez/diagnosztikahoz.
 */
#ifndef NODE_POWERUP_SERVICE_WINDOW_SEC
#define NODE_POWERUP_SERVICE_WINDOW_SEC 20
#endif

/* Max TX power 0.25 dBm egységben. 84 = default 21 dBm; node2-nél 34 = 8.5 dBm. */
#ifndef NODE_TX_POWER
#define NODE_TX_POWER        84
#endif

/* BENCH broadcast ismétlés; FIELD módban az app-ACK-os út dönt a sikerességről. */
#ifndef NODE_TX_REPEAT
#define NODE_TX_REPEAT       3
#endif

/* FIELD megbízható átadás: Sz6 alapján app-ACK, jitter és oldest-first drain. */
#define NODE_ACK_MAX_ATTEMPTS        5
/* Az elso DATA ACK a master tartos flash-menteset is megvarja; retry/HELLO mar gyors lehet. */
#define NODE_DATA_FIRST_ACK_TIMEOUT_MS 2000
#define NODE_ACK_TIMEOUT_MS            120
#define NODE_UNICAST_ATTEMPTS        2
#define NODE_RETRY_JITTER_MIN_MS     20
#define NODE_RETRY_JITTER_MAX_MS     120
#define NODE_PRE_TX_JITTER_MAX_MS    150
#define NODE_SEND_TIMEOUT_MS         50
#define NODE_NO_MEM_RETRY_MS         2
#define NODE_NO_MEM_MAX_RETRY        100
#define NODE_DRAIN_MAX_RECORDS       32
#ifndef NODE_SLEEP_FAIL_JITTER_MAX_SEC
#define NODE_SLEEP_FAIL_JITTER_MAX_SEC 30
#endif
#ifndef NODE_COLD_SYNC_FAIL_SLEEP_SEC
#define NODE_COLD_SYNC_FAIL_SLEEP_SEC 30
#endif
#ifndef NODE_ACK_SLOT_MIN_SEC
#define NODE_ACK_SLOT_MIN_SEC        5
#endif
#ifndef NODE_ACK_SLOT_MAX_EXTRA_SEC
#define NODE_ACK_SLOT_MAX_EXTRA_SEC  300
#endif

/* 20 perces periódussal 7 nap = 504 rekord; 512 ad némi tartalékot. */
#ifndef NODE_PENDING_CAPACITY
#define NODE_PENDING_CAPACITY        512
#endif

/* ===================== IDŐZÍTÉS — profilfüggő ===================== */
#if NODE_PROFILE == NODE_PROFILE_BENCH
  /* Alvási idő két ébrenlét közt (s). */
  #ifndef NODE_WAKE_PERIOD_SEC
  #define NODE_WAKE_PERIOD_SEC   60
  #endif
  /* Egy BENCH-fázis hossza (s) — ennyi ideig aktív az adott mérési blokk, hogy a DMM tisztán mérje.
   * Gyors körméréshez: 4 fázis × 2.5 s = 10 s ébrenlét. */
  #define BENCH_PHASE_MS         2500
  /* Az ESPNOW_TX fázisban ilyen gyakran küld egy payloadot (ms) — folyamatos link-terhelés. */
  #define BENCH_TX_INTERVAL_MS   2000
  /* A közös szenzor-fázisban ilyen gyakran indul új mintavétel. DS18B20 miatt ~750 ms blokkol. */
  #define BENCH_SAMPLE_INTERVAL_MS 1000
#elif NODE_PROFILE == NODE_PROFILE_FIELD
  /* Éles alvási periódus (s). ~20 perc (a végleges érték a 2-node kísérletből jön). */
  #ifndef NODE_WAKE_PERIOD_SEC
  #define NODE_WAKE_PERIOD_SEC   1200
  #endif
#else /* NODE_PROFILE_SLEEP_FLOOR */
  /* Mérési izoláció: minimál firmware, csak deep sleep floor. */
  #ifndef NODE_WAKE_PERIOD_SEC
  #define NODE_WAKE_PERIOD_SEC   60
  #endif
#endif

/* ===================== PIN-TERV (ESP32-C3 SuperMini) ===================== */
/* Kivezetve: GPIO0..10, 20, 21. ADC-képes: GPIO0..4. Strapping (kerülendő): GPIO2, 8, 9. */

/* HX711 — a ténylegesen bekötött mérleg-erősítő lábak.
 * Node oldalon: SCK/PD_SCK=GPIO4, DOUT/DT=GPIO3. A master saját HX711-e ettől független. */
#define HX711_PD_SCK_GPIO    GPIO_NUM_4   /* PD_SCK: HIGH>60us = power-down; olvasáskor SCK */
#define HX711_DOUT_GPIO      GPIO_NUM_3   /* DOUT: adat (input), LOW = konverzió kész */

/* vbat feszültségosztó ADC + NPN low-side kapcsoló.
 * A C3-on CSAK az ADC1 (GPIO0..4) megbízható: az ADC2 (GPIO5) oneshot módban HW-korlátos +
 * WiFi-vel ütközik (Espressif dok/errata) -> a vbat ADC kényszerűen a 0..4 oldalra kerül.
 * Foglalt: GPIO4(SCK), GPIO3(DT), GPIO2 strapping -> vbat ADC = GPIO0 (ADC1_CH0), fizikailag
 * elérhető. A NPN-kapcsoló (digitális) az elérhető 5..10 oldalon (GPIO10).
 *
 * 2026-07-09: csak a node1 low-side kapcsolt osztója lett lekötve a GPIO0-ról, ezért a node1
 * env NODE_VBAT_ENABLED=0. A node2/node3 osztója működik és az env-ekben engedélyezett.
 * Az itt maradó 0 az ismeretlen/egyedi env-ek biztonságos alapértéke. */
#ifndef NODE_VBAT_ENABLED
#define NODE_VBAT_ENABLED    0
#endif
#define VBAT_MEASURE_ENABLED NODE_VBAT_ENABLED
#define VBAT_ADC_GPIO        GPIO_NUM_0   /* ADC1 bemenet (info; a driver a csatornát használja) */
#define VBAT_ADC_UNIT        ADC_UNIT_1   /* ADC1 — WiFi mellett is megbízható (ADC2 nem!) */
#define VBAT_ADC_CHANNEL     ADC_CHANNEL_0/* GPIO0 == ADC1 csatorna 0 a C3-on */
#define VBAT_ADC_ATTEN       ADC_ATTEN_DB_12 /* ~0..2.5 V lineáris (a leosztott ~2.1 V belefér) */
#define VBAT_SW_GPIO         GPIO_NUM_10  /* NPN bázis (10k-n át); HIGH = osztó él, LOW = lekapcsol */

/* DS18B20 1-Wire. A GPIO9 alvásban explicit HIGH + hold, hogy a 4.7k felhúzó
 * ne tudjon LOW-ra hajtott padon át ~700 uA-t elfűteni. */
#ifndef DS18B20_ENABLED
#define DS18B20_ENABLED      1
#endif
#define DS18B20_HOLD_HIGH_IN_SLEEP 1
#define DS18B20_GPIO         GPIO_NUM_9
#define DS18B20_CONVERSION_MS 750

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
/* Hány nyers mintából készüljön a medián-alapú súly-leolvasás. */
#define HX711_MEDIAN_SAMPLES 12
/* Régi BENCH API-kompatibilitás: ugyanazt a mintaszámot használja. */
#define HX711_SAMPLES        HX711_MEDIAN_SAMPLES
/* Power-up után ennyit várunk az első stabil konverzióra (ms). */
#define HX711_SETTLE_MS      50

#endif /* NODE_CONFIG_H */

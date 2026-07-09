# Node power debug - 2026-07-09

## Scope

Teljesen osszerakott ESP32-C3 SuperMini node fogyasztas-debug, akku oldali arammeressel.
A cel a deep sleep aram okainak szetvalasztasa volt a mar megforrasztott node-on.

## Aktualis firmware-allapot

- Normal BENCH profil: kb. 10 s ebrenlet, 60 s deep sleep.
- BENCH fazisok: `BASELINE`, `SENSORS_ALL`, `WIFI_IDLE`, `ESPNOW_TX`.
- `SENSORS_ALL`: HX711 + VBAT placeholder + DS18B20 placeholder egy fazisban logol.
- Sleep elott: ESP-NOW/WiFi explicit deinit, LED egyszer villan, majd sleep.
- Sleep-floor profil: HX711 power-down + minimal deep sleep, minden mas kihagyva.

## Mert eredmenyek

- Ebrenleti fogyasztas: feladatfuggoen kb. 20-90 mA.
- Normal firmware deep sleep: kb. 930-940 uA.
- Sleep-floor + HX711 power-down: kb. 930 uA.
- Sleep-floor rossz HX711 PD_SCK pinnel: kb. 1.8 mA.
- GPIO9 rossz/LOW allapot gyanus eseten a 4.7 kOhm felhuzo onmagaban kb. 700 uA-t tud elfuteni.

## Fontos tanulsagok

1. A 930-940 uA padlo nem a normal BENCH fazisok mellekhatasa.
   A sleep-floor profil is ugyanezt hozza, ha a HX711 valoban power-downban van.

2. A HX711 PD_SCK tenylegesen GPIO5-on van.
   A korabbi `GPIO4` firmware-konfig mellett a HX711 nem ment power-downba, ez okozta a
   sleep-floor kb. 1.8 mA merest. A javitott kiosztas:

   ```c
   #define HX711_PD_SCK_GPIO GPIO_NUM_5
   #define HX711_DOUT_GPIO   GPIO_NUM_6
   ```

3. A GPIO0 VBAT oszto le lett kotve.
   A low-side kapcsolt oszto OFF allapotban a mero-csomopontot VBAT-ra tudja usztatni.
   Firmware-ben a VBAT meres most tiltott:

   ```c
   #define VBAT_MEASURE_ENABLED 0
   ```

4. A GPIO9 / DS18B20 ag jelenleg firmware-ben nem mer homersekletet, de sleep elott HIGH + hold.
   Ennek oka, hogy a 4.7 kOhm felhuzo ne tudjon LOW-ra hajtott padon at aramot elfuteni.

   ```c
   #define DS18B20_ENABLED 0
   #define DS18B20_HOLD_HIGH_IN_SLEEP 1
   ```

5. Az "osszes GPIO-t allitsuk input/no-pullra" probalkozas rossz irany volt.
   A C3 deep sleepben a nem holdolt labakat alapbol high-Z allapotba engedi; a tomeges
   atkonfiguralas board-/strap-/LED-fuggo mA-es romlast okozhatott. Csak a kulso fogyasztot
   vezerlo labakat szabad determinisztikusan kezelni.

## Jelenlegi energia-becsles

940 uA deep sleep mellett 3500 mAh cellaval a puszta sleep plafon:

```text
3500 mAh / 0.94 mA = kb. 155 nap = kb. 5.1 honap
```

20 perces meresi intervallummal, napi 72 ebredessel:

```text
sleep: 0.94 mA * 24 h = 22.6 mAh/nap
active 50 mA, 10 s * 72 = 10.0 mAh/nap  -> kb. 107 nap
active 90 mA, 10 s * 72 = 18.0 mAh/nap  -> kb. 86 nap
```

Realista terepi becsles: kb. 2.5-4 honap, cellatol, homerseklettol es aktiv ciklushossztol fuggoen.

## Nyitott kovetkezo meresek

1. 3V3 oldali arammeres a TPS63000-M kimeneten.
   Ha 3V3 oldalon sokkal kisebb az aram, a maradekot a konverter kis terhelesu hatasfoka eszi meg.

2. Akku oldali vs. 3V3 oldali aram osszevetese ugyanazon sleep-floor firmware-rel.

3. GPIO9 feszultseg ellenorzese deep sleepben.
   Elvart: HIGH, kb. 3.3 V, nem LOW.

4. HX711 PD_SCK feszultseg ellenorzese deep sleepben.
   Elvart: HIGH, kb. 3.3 V.

## Kerulendo kovetkeztetesek

- A 930-940 uA padlot nem szabad automatikusan a HX711-re fogni.
- A VBAT oszto szetbontasa onmagaban nem csokkentette a padlot.
- Tovabbi firmware-es GPIO-probalgatas helyett kovetkezo lepes a taput szetmerese.

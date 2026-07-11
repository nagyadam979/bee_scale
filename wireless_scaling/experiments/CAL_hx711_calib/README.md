# CAL_hx711_calib — HX711 mérleg-cella kalibrációs firmware (NODE-variáns)

Önálló bench/off-grid eszköz a mérleg-cellák kalibrációjához. **Nem prod firmware**: a kalibráció
idejére ezt flasheljük egy egységre, kimérünk, majd a prod firmware visszakerül. A prod
`firmware/node` olvasói (`sens_hx711.c`, `sens_ds18b20.c`) ide **másolva** vannak — a prod kódhoz
nem nyúlunk.

Bekötés = **NODE** (ESP32-C3 SuperMini): HX711 `PD_SCK=GPIO4`, `DOUT=GPIO3`; DS18B20 `GPIO9`.
(A master S3-Zero bekötése eltér → külön variáns lesz.)

## Kalibrációs modell (felhő-oldali, cellánként)
```
kg = (raw − zero_count − tempCoef·(scale_c − refTemp)) / count_per_kg
```
- `zero_count`, `count_per_kg`: **súly-mód** (bench, stabil hőn, ismert súlyok).
- `tempCoef` (count/°C): **hő-mód** (off-grid, hűtő↔nap hőpontok, terheletlen). Sanity-check: M1b ~1031.
- `refTemp`: a súly-kalibráció bench-hőmérséklete (scale_c).

## Használat
Fordítás: `pio run -e cal_node` · Flash+monitor: `pio run -e cal_node -t upload -t monitor`

Boot után 10 s figyelő-ablak: ha jön parancs → interaktív; ha néma → **AUTOSTART hő-log** (off-grid).

Parancsok (egy karakter):
- `w` — súly-stream (`WGT <raw> <scale_c>`), `x` = stop
- `g` — hő-log indítása (append a `callog` partícióba)
- `d` — dump (`REC <idx> <raw> <scale_c>` … `# DUMP END n=`)
- `s` — státusz, `e` — log törlése (megerősítés: `Y`), `?` — menü

## Soros protokoll (PC-script ezt parse-olja)
- Súly-stream: `WGT <raw_median> <scale_c|nan>`
- Hő-log élő: `TLG <idx> <raw> <scale_c|nan>`
- Dump rekord: `REC <idx> <raw> <scale_c|nan>` ; vége: `# DUMP END n=<count>`
- Minden `#`-kezdetű sor és minden ESP-LOG sor (`W (…`, `E (…`) **figyelmen kívül hagyandó**.

## Adattárolás
`callog` nyers partíció (256 KiB), fix 16 B rekord (`magic,raw,scale_c,seq`), append-log az
`esp_partition` API-val — per-rekord durable (a tápot akku-kihúzással szüntetjük). Kapacitás
**16384 rekord (~68 óra @15 s)**. NVS helyett szándékos: a ritka NVS-flush power-loss-nál
veszíthetne egyensúlyi pontokat.

## Bench-protokoll cellánként
1. **Súly**: USB-ről `w`, üres platform → ismert súly-lépcsők; a PC-script rögzíti a stabil
   mediánokat → `zero_count` + `count_per_kg`.
2. **Hő**: `e` (töröl), akkuról hűtőbe/napra (terheletlen), mindegyik hőponton egyensúlyig; a
   szonda a cella tényleges hőjét mérje. Vissza USB-re → `d` → `tempCoef` illesztés.

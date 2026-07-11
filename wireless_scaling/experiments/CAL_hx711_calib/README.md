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

## PC-oldali eszköz — `tools/cal_capture.py`
A firmware csak nyers adatot streamel; a gyűjtést/illesztést ez a script végzi, és cellánként
egy közös `calibration.json`-be írja az eredményt. A `fit` stdlib-only (OLS); a `weight`/`temp`
capture-höz **pyserial** kell:
```
.venv/bin/pip install pyserial
```
Soros port (macOS, C3 natív USB): `/dev/cu.usbmodem*` (dugd be, `ls /dev/cu.*`).

Alparancsok (a `--unit` a cella neve: `node1`/`node2`/`node3`):
```
# 1) SÚLY — interaktív kg-címkézés (üres platform = 0), majd súly-lépcsők
python3 tools/cal_capture.py weight --port /dev/cu.usbmodemXXXX --unit node1
#    -> ../data/node1_weight.csv   (kg,count_median,scale_c,n)

# 2) HŐ — a board off-grid loggolt (hűtő/nap); visszadugva a dump lehúzása
python3 tools/cal_capture.py temp   --port /dev/cu.usbmodemXXXX --unit node1
#    -> ../data/node1_temp.csv     (seq,raw,scale_c)

# 3) ILLESZTÉS (serial nélkül) -> a node1 bejegyzés a calibration.json-ben
python3 tools/cal_capture.py fit --unit node1
python3 tools/cal_capture.py show          # a teljes calibration.json
python3 tools/cal_capture.py selftest      # a fit-mag öntesztje (board nélkül)
```
A `fit` kiírja a diagnosztikát is: súly-`R²` + RMS-reziduál (count és kg), hő-`tempCoef` + `R²` +
`scale_c` span, és figyelmeztet, ha a `tempCoef` az M1b ~1031-től nagyságrendben eltér, vagy ha a
hő-span < 10 °C (bizonytalan meredekség).

## Bench-protokoll cellánként
1. **Súly**: USB-ről `weight` alparancs, üres platform → ismert súly-lépcsők; a script rögzíti a
   stabil mediánokat → `zero_count` + `count_per_kg`.
2. **Hő**: `e` (töröl), akkuról hűtőbe/napra (terheletlen), mindegyik hőponton egyensúlyig; a
   szonda a cella tényleges hőjét mérje. Vissza USB-re → `temp` alparancs (`d` dump) → `tempCoef`.
3. **Illesztés**: `fit --unit <cella>` → `calibration.json`. A 4 cellát (node1..3 + master)
   egymás után kalibrálva egy fájl épül, amit a felhő-oldali konverzió használ.

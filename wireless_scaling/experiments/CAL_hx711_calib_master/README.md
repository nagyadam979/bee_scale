# CAL_hx711_calib_master — HX711 mérleg-cella kalibrációs firmware (MASTER-variáns)

A [`CAL_hx711_calib`](../CAL_hx711_calib/README.md) (node) firmware **master-variánsa** (ESP32-S3-Zero).
A keret (soros protokoll, `callog` nyers partíció, boot-listen ablak, AUTOSTART, w/g/d/s/e parancsok)
azonos; a lenti eltérések a master HW-ből fakadnak. A PC-oldali eszköz **közös**: a node
`tools/cal_capture.py`-t használja (lásd lentebb a `--temp-col`-t).

## Eltérések a node-variánstól

| | node (C3 SuperMini) | master (S3-Zero) |
|---|---|---|
| HX711 | SCK=GPIO4, DOUT=GPIO3 | **SCK=GPIO2, DOUT=GPIO4** |
| DS18B20 | 1 szonda, GPIO9, Skip-ROM | **2 szonda egy buszon, GPIO5, ROM-search + match-ROM** |
| Rekord | 16 B (`magic,raw,scale_c,seq`) | **20 B (`magic,raw,ds_a_c,ds_b_c,seq`)** |
| Kapacitás | 16384 rekord (~68 h) | **13107 rekord (~54 h)** @15 s |
| LED életjel | kék GPIO8 villan | WS2812 RGB → **kikapcsolva** (életjel a TLG sor) |
| Konzol | USB-Serial-JTAG | USB-Serial-JTAG (a prod master UART-ot használ — ez itt szándékos) |

## A 2 DS18B20 és a „scale" azonosítása

A masteren egy 1-Wire buszon **két** DS18B20 van (kaptár-hő + mérleg-cella-hő). A firmware a
bootkor ROM-search-el megtalálja mindkettőt, **rendezett ROM-sorrendben** (`a`=index 0, `b`=index 1)
tárolja, és **mindkét hőt** logolja/streameli. Szerep-mappinget (hive/scale) NEM végez — a bootkor
kiírt sorból azonosítod, melyik oszlop a scale:

```
# DS ROLES count=2 a=28FF64... b=28AB12...
```

A prod master élesítésekor dokumentált `scale_c` ROM-ot összeveted az `a`/`b` ROM-mal, és a fit-nél
a megfelelő oszlopot választod: **`--temp-col ds_a_c`** vagy **`--temp-col ds_b_c`**.

## Fordítás / flash

```
pio run -e cal_master
pio run -e cal_master -t upload -t monitor
```

## PC-oldali workflow (közös cal_capture.py, `--unit master`)

```
# 1) SÚLY (bench, USB): a WGT most 2 hőt streamel -> master_weight.csv (kg,count_median,ds_a_c,ds_b_c,n)
python3 ../CAL_hx711_calib/tools/cal_capture.py weight --port /dev/cu.usbmodemXXXX --unit master

# 2) HŐ (off-grid): a dump 2 hőt ad -> master_temp.csv (seq,raw,ds_a_c,ds_b_c)
python3 ../CAL_hx711_calib/tools/cal_capture.py temp   --port /dev/cu.usbmodemXXXX --unit master

# 3) ILLESZTÉS — a scale-oszlop megadásával (a # DS ROLES alapján):
python3 ../CAL_hx711_calib/tools/cal_capture.py fit --unit master --temp-col ds_b_c
```

A `--temp-col` UGYANAZT az oszlopnevet választja a temp- ÉS a súly-CSV-ből is (a `refTemp`-hez):
`ds_a_c` → az `a` szonda, `ds_b_c` → a `b` szonda. Ha a súly-CSV-ben nincs meg az oszlop
(pl. csak 1 hő volt), a `refTemp` a `scale_c`/első hő-oszlopra esik vissza (figyelmeztetéssel).
A node workflow (`--temp-col` nélkül, `scale_c`) változatlan.

## Adatformátum (soros)

- Súly-stream: `WGT <raw_median> <ds_a_c|nan> <ds_b_c|nan>`
- Hő-log élő: `TLG <idx> <raw> <ds_a_c|nan> <ds_b_c|nan>`
- Dump rekord: `REC <idx> <raw> <ds_a_c|nan> <ds_b_c|nan>` ; vége: `# DUMP END n=<count>`
- Szerep-lista: `# DS ROLES count=<n> a=<rom> b=<rom>`
- Minden `#`-kezdetű sor és minden ESP-LOG sor figyelmen kívül hagyandó.

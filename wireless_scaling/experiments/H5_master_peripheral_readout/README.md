# H5 — master periferia-olvaso hardverteszt

Ez nem fo firmware, hanem egyszeru hardver-validacios experiment. Celja csak annak igazolasa,
hogy a master vegleges periferiai stabilan olvashatok. Szandekosan nincs benne ESP-NOW, cloud,
deep sleep vagy kalibracio.

## GPIO kiosztas

| Periféria | GPIO |
| --- | --- |
| HX711 SCK | GPIO2 |
| HX711 DT/DOUT | GPIO4 |
| 2x DS18B20+ 1-Wire busz | GPIO5 |
| RJ11 esőkapcsoló | GPIO7, külső pull-up + kondenzátoros szűrés, zárt kapcsoló = LOW |
| AHT20 + BMP280-M I2C SDA | GPIO8 |
| AHT20 + BMP280-M I2C SCL | GPIO9 |

## Használat

```sh
cd /Users/adamnagy/Documents/bee_scale/wireless_scaling/experiments/H5_master_peripheral_readout
~/.platformio/penv/bin/pio run -e esp32-s3-zero
~/.platformio/penv/bin/pio run -e esp32-s3-zero -t upload --upload-port /dev/cu.usbmodemXXXX
~/.platformio/penv/bin/pio device monitor --port /dev/cu.usbmodemXXXX --baud 115200
```

A log fő sora `MASTER_READ`, mellette van egy egyszerű `CSV` sor is feldolgozáshoz.

Elvárt első ellenőrzés:

- `ds_count=2` jelenjen meg.
- `aht_ok=1` és `bmp_ok=1` legyen.
- HX711 terhelésre változó nyers `hx711_raw` értéket adjon.
- Az RJ11 esőkapcsoló zárásakor `rain_closed=1` legyen.

## Validalt allapot

2026-07-09, ESP32-S3-Zero port: `/dev/cu.usbmodem21201`.

Fontos javitas: a HX711 DT/DOUT labja GPIO4, nem GPIO3. A rossz GPIO3 olvasas `0x000000`
kereteket adott; a firmware ezt mar `hx711_ok=0` hibakent jeloli. GPIO4-en a HX711 normalis
konverzios ciklust mutatott.

Ellenorzott minta:

```text
hx711_ok=1 hx711_raw=-637084
ds_ok=1 ds_count=2
rain_level=1 rain_closed=0
aht_ok=1 aht_t=29.352 aht_rh=26.242
bmp_ok=1 bmp_t=29.680 bmp_hpa=1001.954
```

Kovetkezo lepcso: a fo master firmware kulon letrehozasa vagy bovites elott a merleg
nullpont/kalibracios faktor felvetele.

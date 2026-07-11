# Master pin-kiosztas

| Periferia | GPIO |
| --- | --- |
| HX711 SCK | GPIO2 |
| HX711 DT/DOUT | GPIO4 |
| 2x DS18B20+ 1-Wire busz | GPIO5 |
| RJ11 eso kapcsolo | GPIO7 |
| AHT20 + BMP280-M I2C SDA | GPIO8 |
| AHT20 + BMP280-M I2C SCL | GPIO9 |

Eso szamolas: GPIO7-en egy stabil HIGH->LOW atmenet egy billenes, az osszes eso `billenesek_szama * mm_per_billenes (0,2794 mm)`.

DS18B20 szerepek:

- `ds0`: kaptar szonda homerseklet
- `ds1`: merlegkalibracios onboard homero

## Firmware

Build:

```sh
cd firmware/master
~/.platformio/penv/bin/pio run -e esp32-s3-zero
~/.platformio/penv/bin/pio run -e esp32-s3-zero-cloud
```

## Végleges terepi build: felhő + helyi státusz oldal

Az `esp32-s3-zero-local-status` a teljes éles firmware: node-vétel, master
szenzorok, flash backlog, Firebase feltöltés **és** a master saját HTTP státuszoldala.
A master és a telefon ugyanarra a 2,4 GHz-es WiFi-re csatlakozik; a státuszoldal
internet nélkül is működik, a Firebase-hez természetesen internetkapcsolat kell.

```sh
cd firmware/master
# main/cloud_config.h kell (benne a WiFi és Firebase beállítások)
~/.platformio/penv/bin/pio run -e esp32-s3-zero-local-status
```

Feltoltes utan a soros logban megjelenik a `LOCAL_STATUS open http://...` sor.
Telefonrol ezt a helyi IP-cimet nyisd meg. A lap 5 masodpercenkent frissul,
es node-onkent mutatja az utolso vetel idejet, RSSI-t, seq-et, meresi adatokat es
a kommunikacios hibaszamlalokat. A router WiFi-csatornajanak egyeznie kell a
`MASTER_ESPNOW_CHANNEL` ertekevel (production default: 6), mert a WiFi STA es
az ESP-NOW ugyanazt a radiot hasznalja.

Cloud buildhez:

```sh
cp main/cloud_config.example.h main/cloud_config.h
```

Az `esp32-s3-zero-cloud` env WiFi STA + ESP-NOW parhuzamos uzem, Firebase RTDB `PUT`, es sikertelen feltoltes utan flash backup. A merlegadat nyersen megy fel (`weight_raw` / `hx711_raw`), a kg konverzio felhoben tortenik.

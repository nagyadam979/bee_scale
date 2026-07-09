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

Cloud buildhez:

```sh
cp main/cloud_config.example.h main/cloud_config.h
```

Az `esp32-s3-zero-cloud` env WiFi STA + ESP-NOW parhuzamos uzem, Firebase RTDB `PUT`, es sikertelen feltoltes utan flash backup. A merlegadat nyersen megy fel (`weight_raw` / `hx711_raw`), a kg konverzio felhoben tortenik.

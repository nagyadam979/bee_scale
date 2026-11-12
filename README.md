# bee_scale

Okos méhkaptár-mérleg hobbi méhészethez: folyamatosan méri a kaptár súlyát, és az adatot
távolról, webes dashboardon teszi elérhetővé (hordás/fogyás trend, rajzás, akku- és szenzorhibák).

## Felépítés

- [`prototype/`](prototype) – az első, vezetékes prototípus: ESP32 + HX711 + mérőcellák,
  Firebase Realtime Database feltöltés (`firmware/bee_scale_rtdb`), kísérleti sketch-ek
  és egy statikus, read-only web dashboard (Firebase Hosting, `public/`).
- [`wireless_scaling/`](wireless_scaling) – a vezeték nélküli változat: ESP32-C3 node-ok
  ESP-NOW-n egy master felé, ESP-IDF firmware (`firmware/master`, `firmware/node`),
  host-oldali szimulációk és fuzz tesztek (`firmware/test_host`), mérési kísérletek
  (`experiments/`), parametrikus 3D nyomtatott ház (build123d), KiCad PCB és egy
  több telepítést kezelő dashboard (`dashboard/`).

A `CLAUDE.md`, `MEMORY.md` és `agents/` fájlok a fejlesztést segítő AI-ágensek munkajegyzetei;
a döntések és mérési tanulságok ezekben is dokumentálva vannak.

## Titkok és adatok

A WiFi-, Firebase-eszköz- és OTA-adatok helyi `secrets.h` / `creds.ini` fájlokban vannak
(verziókezelésen kívül); mindegyikhez van `.example` sablon. A Firebase **web** konfiguráció
(apiKey stb.) nem titok: a hozzáférést a Realtime Database Security Rules szabályozza.

A nyers mérési adatok (CSV-k, terepi és kalibrációs logok) nem részei a nyilvános repónak;
a kísérletek eredményeit az egyes kísérleti mappák `.md` összefoglalói írják le.

## Licenc

MIT – lásd a [LICENSE](LICENSE) fájlt.

# Sz5 — Master idő + uplink

**Típus:** Szoftver chunk · **Setup:** 2 / 3 · **Irány:** (B)/(C), nagyrészt **párhuzamosan fejleszthető**

## Cél
A master abszolút időtengelye és a felhő-uplink: **NTP + szabadonfutó óra**, az NTP–belsőóra
eltérés logolása, **utólagos lineáris korrekció Pythonban**, valamint a **4G uplink** (ZTE MF79U).
Cél: rekonstruált abszolút timestamp ±1 perc tűréssel egy mesterséges kimaradás után.

## Függőségek
- **Nagyrészt nem blokkol és nem blokkolt** — nyers master logból és szimulált kimaradásokból
  tesztelhető, nem várja meg a rádiós réteget. A leggyorsabb javítási ciklus (offline Python).
- Kapcsolódik: H3 (uplink a duty-cycle átlagot torzítja → külön mérni).

## Bemenet / előfeltételek
- Master board (ESP32-C3 SuperMini; 2026-07-02 pivot az S3-Zero-ról, lásd H4), NTP-elérés (uplinken át), nyers master log.
- ZTE MF79U 4G stick (WiFi hotspot, Yettel SIM, B20/B8), MOSFET/load switch a stick 5V tápján.
- Python offline elemző réteg (a nyers log újrajátszásához).

## Megvalósítás vázlata (NEM kód)
1. **Idő:** NTP-szinkron + szabadonfutó belső óra; az **NTP–belsőóra eltérés folyamatos logolása**.
2. **Offline korrekció:** Pythonban lineáris óra-korrekció a nyers logra → abszolút timestamp-rekonstrukció.
3. **Uplink:** a master a stick WiFi hotspotjára csatlakozik STA-ként; MQTT QoS1 / HTTPS batch, sikeres
   feltöltés után EEPROM checkpoint. Load switch kapcsolja a stick tápját (napi 1 ablak / folyamatos).
4. **Kimaradás-szimuláció:** mesterséges net-kimaradás → a rekonstruált timestamp ±1 perc-en belül.

## Kimenet / deliverable
- Rekonstruált abszolút timestamp **±1 perc** tűréssel egy mesterséges kimaradás után + működő 4G uplink.

## Pass / fail
| Feltétel | Pass |
|---|---|
| Timestamp-rekonstrukció kimaradás után | **±1 perc**-en belül |
| Offline Python korrekció | nyers logból újrajátszva reprodukálható |
| 4G uplink | batch feltöltés sikeres, utána EEPROM checkpoint |

## Nyitott kérdések / kockázatok
- **MF79U firmware** tápra-felregisztrálása: tápot adva jön-e net beavatkozás nélkül? Ha nem →
  HTTP POST a `192.168.x.1` admin végpontra, vagy „mindig bekapcsolva" (csak folyamatos tápnál). Éles darabon mérendő (MEMORY.md #1).
- 2.4 GHz koegzisztencia: feltöltés alatt **ESP-NOW vétel szünetel** (seq+PSRAM miatt nincs adatvesztés).
- Tápzaj: 4G adás-tüskék → külön táprail / 1000 µF puffer a stick tápján.

## Hivatkozások
- `../../docs/new_base_components.md` (Uplink, Master táp, EEPROM checkpoint), `../../MEMORY.md` (7. nyitott kérdés #1).
- Kapcsolódik: [`../H3_master_power_duty_cycle/`](../H3_master_power_duty_cycle/).

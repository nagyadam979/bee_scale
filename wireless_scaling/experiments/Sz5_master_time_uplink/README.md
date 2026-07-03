# Sz5 — Master idő + uplink

**Típus:** Szoftver chunk · **Setup:** 2 / 3 · **Irány:** (B)/(C), nagyrészt **párhuzamosan fejleszthető**

## 🔄 RESCOPE (2026-07-03 — always-on master)
Hálózati tápon a master **folyamatosan neten van** → az **NTP végig él**, így a szabadonfutó-óra ±1 perces
**rekonstrukció csak egy net-kimaradásra** releváns (nem a normál üzemre). A „napi 1 ablak" / load-switch /
MF79U-boot részek **historikusak** (a stick tiszta hálózati tápon mindig bekapcsolva). Az uplink-koegzisztencia
most **D1** szerint: egyidejű WiFi STA + ESP-NOW (az **Sz2b** validálja), nem időbeli szétválasztás.

## Cél
A master abszolút időtengelye és a felhő-uplink: **NTP + szabadonfutó óra**, az NTP–belsőóra
eltérés logolása, **utólagos lineáris korrekció Pythonban**, valamint a **4G uplink** (ZTE MF79U).
Cél: rekonstruált abszolút timestamp ±1 perc tűréssel egy mesterséges kimaradás után.

## Függőségek
- **Nagyrészt nem blokkol és nem blokkolt** — nyers master logból és szimulált kimaradásokból
  tesztelhető, nem várja meg a rádiós réteget. A leggyorsabb javítási ciklus (offline Python).
- Kapcsolódik: H3 (uplink a duty-cycle átlagot torzítja → külön mérni).

## Bemenet / előfeltételek
- Master board (ESP32-S3-Zero), NTP-elérés (uplinken át), nyers master log.
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
- **MF79U áramszünet utáni auto-recovery** (alacsony prioritás — a stick mindig tápon): egy áramszünet
  után magától visszaregisztrál-e a mobilnetre? Ha nem → HTTP POST a `192.168.x.1` admin végpontra. Éles darabon mérendő (MEMORY.md #1).
- 2.4 GHz koegzisztencia: **D1 — egyidejű WiFi STA + ESP-NOW** a terv (az Sz2b validálja); a régi „feltöltéskor ESP-NOW szünetel" csak fallback (seq+PSRAM miatt akkor sincs adatvesztés).
- Tápzaj: 4G adás-tüskék → külön táprail / 1000 µF puffer a stick tápján.

## Hivatkozások
- `../../docs/new_base_components.md` (Uplink, Master táp, EEPROM checkpoint), `../../MEMORY.md` (7. nyitott kérdés #1).
- Kapcsolódik: [`../H3_master_power_duty_cycle/`](../H3_master_power_duty_cycle/).

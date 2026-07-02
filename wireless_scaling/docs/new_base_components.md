# Méhkaptár-mérleg rendszer — projektdokumentáció

## Rendszerarchitektúra

**Topológia:** master–slave, csillag elrendezés. Több slave node (kaptáronként), egy központi master.

**Kommunikáció:** ESP-NOW (slave→master) + WiFi koegzisztencia. A master WiFi STA módban csatlakozik az uplinkhez, és az ESP-NOW-t ugyanarra a csatornára húzza; a slave-ek ehhez igazodnak (különben csomagvesztés).

**Vételi pipeline (a master gerince):** producer–consumer szétválasztás.
- Producer = ESP-NOW receive callback: csak bedob a queue-ba (`xQueueSendFromISR`), semmi mást.
- Consumer = dedikált task a másik magon: kivesz, időbélyegez, pufferel, felhőz.
- Az ESP32 két magja: a vétel és a felhős I/O nem akadályozza egymást.

**Időbélyegzés:** a master feladata. A slave-en mért idő megbízhatatlan (RTC drift deep sleep alatt); a hiteles időtengely a központi vételi időbélyeg (`rx_millis` + NTP-korrekció).

**Adatperzisztencia:** SD elhagyva. A tömeget a master PSRAM-ja puffereli; reset-túlélő folytonosságot külső EEPROM checkpoint biztosítja. Részletek lent.

### Adatformátum

Fixed-size struct (`payload_t`), nincs parse/string. Slave és master ugyanazt használja; ajánlott `__attribute__((packed))`.

| Mező | Típus | Funkció |
|---|---|---|
| `unit_id` | uint8_t | melyik kaptár/sarok |
| `seq` | uint32_t | szekvencia, hiányzó csomag detektálás |
| `weight` | float | nyers/kalibrált terhelés |
| `temp` | float | drift-korrekció |
| `vbat` | uint16_t | elemfeszültség (mV) |

**Felhőzés:** MQTT (QoS 1) vagy HTTPS, retry-kezeléssel, batch-elve.

---

## Kommunikációs protokollok

### Node → Master: ESP-NOW
- Connectionless, nincs WiFi-asszociáció → rövid ébredés→küldés→alvás ciklus, alacsony node-fogyasztás.
- **MAC-szintű ACK ≠ alkalmazás-szintű kézbesítés.** Megbízható átvitelhez app-szintű ack kell: a master visszaigazol, a node timeoutra retransmittál; a `seq` kiszűri a duplikátumot és detektálja a hiányt.
- **Adatvesztés vs. energia kompromisszum:** vagy "tűzz-és-felejts" + `seq`-alapú gapdetektálás az elemzésben (kevesebb node-fogyasztás), vagy app-ack (több ébrenlét, garantáltabb kézbesítés). Kutatási idősornál a detektálható gap kezelhető → a könnyebbik út is védhető.
- Titkosítás opcionálisan: PMK/LMK AES-128.

### Master → Felhő: MQTT QoS 1 (vagy HTTPS)
- A PSRAM-ban felgyűlt rekordok ablakosan, batch-elve kimennek; sikeres feltöltés után EEPROM checkpoint.

---

## Adattárolás és perzisztencia

### Napi puffer az EEPROM-ban (nincs PSRAM)
- **2026-07-02 pivot:** a master ESP32-C3 SuperMini lett (az S3-Zero deep sleepben mA-t húz, H4) → **nincs PSRAM**, a korábbi ~40 napos RAM-puffer terve elesik.
- A valós igény nem 40 nap, hanem a **napi feltöltési ablakok közti** adat megtartása + reset-túlélés — ezt közvetlenül az **AT24C256 EEPROM** adja (perzisztens → a reset-túlélés is ingyen).
- **Kapacitás:** a fixed-size `payload_t` ~10-12 byte/rekord → 32 KB ≈ ~2800-3200 rekord. A napi puffer akkor elég, ha `node_szám × napi_mintaszám ≲ ~3000`. Példák: 1 node @ negyedóra (96/nap) → ~1 hónap; 4 node → ~1 hét; 12 node → ~2-3 nap. A hobbi-skálán (pár node) egy nap bőven belefér, több napnyi tartalékkal.
- Ha a node-szám × mintaszám kinőné a 32 KB-ot: sűrűbb uplink-ablak, nagyobb EEPROM (AT24C512), vagy külső SPI flash — de a jelenlegi skálán nem kell.

### EEPROM checkpoint — AT24C256-I2C
- I²C, **32 KB** (256 kbit; a "256kB" bolti jelölés félrevezető), jumperes cím (0x50–0x57) + hardveres write-protect, fedélzeti pull-up.
- Lapméret 64 byte → a checkpoint-slotokat lap-igazítva, hogy egy írás egyetlen page-write legyen.
- Endurance 1M írás/cella + slot-alapú wear leveling → gyakorlatilag soha nem kopik el ritka checkpointnál.
- Tartalom: **(a)** a még fel nem töltött rekordok napi puffere, **(b)** node-onkénti utolsó `seq` + timestamp + integritás-jelző (a legfrissebb érvényes slot kiválasztásához reset után).
- Bekötés: VCC (3.3V), GND, SDA, SCL — `Wire.begin(sda, scl)`.

---

## Uplink (master → internet)

### Eszköz: ZTE MF79U 4G LTE USB WiFi stick (~14 800 Ft, magyar bolt)
- LTE-FDD **B1/B3/B5/B7/B8/B20** (800/850/900/1800/2100/2600 MHz) — a **B20 (800) és B8 (900) a Yettel vidéki sávjai**, így külterületen is van lefedettség.
- 150/50 Mbps, USB 5V/1A táp, **külső antenna 2× TS-9** (gyenge külterületi térerőhöz kivezethető), nano SIM, microSD (utóbbi nem releváns).
- A master a stick **WiFi hotspotjára** csatlakozik STA-ként → nincs USB host / driver gond (a "Windows/Mac/Linux" megkötés csak USB-adatmódra vonatkozik, a WiFi-mód OS-független).

### Vezérlés / táp
- USB 5V MOSFET / load switch a master GPIO-járól → a master kapcsolja a stick tápját.
- Boot: ~30–45 s teljes felállás (WiFi + LTE registráció).
- **Firmware-buktató:** egyes MF79U firmware-ek teljes tápvesztés után nem kapcsolják vissza maguktól a mobiladatot (web UI-n vagy AT-paranccsal kéne). Tesztelendő a konkrét darabon: tápra adva jön-e net beavatkozás nélkül.
  - Ha igen → teljes-lekapcsolós ablakos üzem mehet.
  - Ha nem → a master HTTP POST-tal a `192.168.x.1` admin végpontra (alap jelszó `admin`) visszakapcsolja az adatot bekapcsolás után; vagy "mindig bekapcsolva" üzem (csak folyamatos tápnál).

### Uplink-master koegzisztencia (közös doboz, ~2 mm)
1. **2.4 GHz önzavar:** a stick WiFi hotspotja és az ESP WiFi/ESP-NOW-ja is 2.4 GHz. Időbeli szétválasztás: **feltöltési ablak alatt az ESP-NOW vétel szünetel** (a `seq`+PSRAM miatt nincs adatvesztés, csak késleltetés).
2. **Tápzaj:** a 4G adás-tüskék rángatják az 5V-ot → külön táprail vagy bő puffer-elkó (pl. 1000 µF) a stick tápján.
3. **RF-áthallás:** a TS-9 külső antennát vezesd ki a doboz szélére/kívülre (egyben jobb térerő).
4. Opcionálisan földelt árnyékolólemez a két panel közé (általában felesleges, ha az antenna kint van).

### Energiamérleg (uplink)
- Napi 1 bekapcsolás, ~2 perc aktív: ~2 W × 2 perc ≈ **0,067 Wh/nap** ≈ **~13 mAh/nap** (5V-on). Elhanyagolható.
- A master táp-méretezésnél nem ez dominál, hanem a **master ESP saját fogyasztása** a vételi ablakok között (mennyit alszik vs. mennyit figyel ESP-NOW-ra).

---

## Master táp — kétféle üzemmód
A hardvert a szigorúbb (akkus) esetre tervezzük, a szoftver flaggel kezeli a különbséget; a helyszín dönt.

- **A) Folyamatos táp** (hálózat / nagy napelem+puffer): a load switch "mindig be", uplink folyamatosan mehet, nincs boot-probléma.
- **B) Csak akku/napelem:** a master a nap 99%-ában lekapcsolva tartja az uplinket, napi 1 ablakra kapcsol; a load switch + (szükség esetén) HTTP-vezérlés kezeli.

A napi 1 feltöltés az EEPROM napi pufferével biztonságos a hobbi-skálán (pár node); a puffer-tartalék (lásd fenti kapacitás-becslés) a node-szám függvényében pár kihagyott ablakot is elnyel.

---

## BOM — Slave node (kaptáronként)

| Alkatrész | Típus | Funkció | Megjegyzés |
|---|---|---|---|
| MCU | ESP32-C3 SuperMini | mérés + ESP-NOW + deep sleep | egymagos RISC-V; gyári LED leszedendő |
| Mérlegcella | HBS-SCL-50 | terhelés (50 kg) | csak hídkapcsolás, ADC kell |
| ADC/erősítő | HX711-M | 24-bites mérőátalakító | 2.6–5.5 VDC; PD_SCK magasra húzva altatható; LDO-ról szűrt 3.3V! |
| Hőmérő | DS18B20+ | hőmérséklet + drift-korrekció | 1-Wire, 4.7 kΩ felhúzó |
| Elemmérés | külső feszültségosztó | `vbat` logolás | TP4056 OUT+ pontról |
| Cella | EVE INR18650/35V | Li-ion, ~3500 mAh | 4.2 V → ~3.0 V |
| Töltő/védelem | TP4056-1A-PROT-USBC | CC/CV töltés + védelem | DW01+FS8205 |

## BOM — Master / központ

| Alkatrész | Típus | Funkció | Megjegyzés |
|---|---|---|---|
| Board | ESP32-C3 SuperMini | vétel + időbélyeg + EEPROM-puffer + felhőzés | egymagos RISC-V; **2026-07-02 pivot az S3-Zero-ról** (az S3 deep sleepben mA-t húz, H4); ~185 µA alvó /H1/; PSRAM nem kell (szekvenciális RX→uplink, napi EEPROM-puffer) |
| Checkpoint | AT24C256-I2C modul | reset-túlélő `seq`-mentés | I²C, 32 KB, wear leveling |
| Uplink | ZTE MF79U | 4G WiFi stick (Yettel SIM) | B20+B8; opcionális TS-9 külső antenna |
| Táp-kapcsoló | MOSFET / load switch | uplink 5V kapcsolása | master GPIO vezérli |
| Táp | nyitott | folyamatos vagy akku/napelem | helyszínfüggő |

---

## Tervezési pontok / kockázatok
- **HX711 tápja kritikus** a méréspontossághoz: tiszta, LDO-ról szűrt 3.3V, ne a süllyedő cellasínről — különben a `weight` az akku töltöttségével driftel.
- **TP4056 csak töltő**, nem buck/boost — a 3.3V külön réteg.
- **WiFi/ESP-NOW azonos csatorna** — korán tesztelendő (kész).
- **MF79U firmware** tápra-felregisztrálása — tesztelendő (lásd uplink).
- **2.4 GHz koegzisztencia** — feltöltés alatt ESP-NOW szünetel.
- **Master ESP nyugalmi fogyasztása** dominálja a táp-mérleget — külön végigszámolandó akkus esetben.

# Méhkaptár-mérleg rendszer — projektdokumentáció

## Rendszerarchitektúra

**Topológia:** master–slave, csillag elrendezés. Több slave node (kaptáronként), egy központi master. A **master hibrid** (D4, 2026-07-03): a vétel/időbélyeg/felhőzés mellett **maga is mér** a saját kaptárán, saját `unit_id`-vel.

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
- **Adatvesztés vs. energia kompromisszum:** vagy "tűzz-és-felejts" + `seq`-alapú gapdetektálás az elemzésben (kevesebb node-fogyasztás), vagy app-ack (több ébrenlét, garantáltabb kézbesítés). Kutatási idősornál a detektálható gap kezelhető → a könnyebbik út is védhető. **Always-on masterrel** (hálózati táp, 2026-07-03) a master mindig figyel → a node tűzz-és-felejts broadcastja mindig megérkezik, nem kell a node-ébredést a master ablakához igazítani (a rendezvous/drift-guard nagyrészt felesleges). A master WiFi STA + ESP-NOW **egyidejű üzeme** a terv (**D1**) — az **Sz2b** validálja.
- Titkosítás opcionálisan: PMK/LMK AES-128.

### Master → Felhő: MQTT QoS 1 (vagy HTTPS)
- A PSRAM-ban felgyűlt rekordok ablakosan, batch-elve kimennek; sikeres feltöltés után EEPROM checkpoint.

---

## Adattárolás és perzisztencia

### PSRAM mint tömeg-puffer
- Becslés ~32 byte/rekord, ~1.5 MB elérhető PSRAM → ~49 000 rekord. 12 node negyedóránként ~1150 rekord/nap → **~40 napnyi** puffer felhő-kimaradás esetén is.
- **Megszorítás:** a PSRAM nem perzisztens — reset/áramszünet törli. Ezt veszi át az EEPROM checkpoint + `seq`-gapdetektálás.

### EEPROM checkpoint — AT24C256-I2C
- I²C, **32 KB** (256 kbit; a "256kB" bolti jelölés félrevezető), jumperes cím (0x50–0x57) + hardveres write-protect, fedélzeti pull-up.
- Lapméret 64 byte → a checkpoint-slotokat lap-igazítva, hogy egy írás egyetlen page-write legyen.
- Endurance 1M írás/cella + slot-alapú wear leveling → gyakorlatilag soha nem kopik el ritka checkpointnál.
- Tartalom: node-onkénti utolsó `seq` + timestamp + integritás-jelző (a legfrissebb érvényes slot kiválasztásához reset után).
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
1. **2.4 GHz önzavar:** a stick WiFi hotspotja és az ESP WiFi/ESP-NOW-ja is 2.4 GHz. **D1 (2026-07-03): a terv az egyidejű üzem** — a master a stick hotspotjára STA-ként kapcsolódva figyel, közben ESP-NOW-t is vesz (azonos csatorna). Ezt az **Sz2b validálja**; ha nem stabil, **fallback: ablakos** üzem (feltöltéskor az ESP-NOW rövid szünet — a `seq`+PSRAM miatt csak késleltetés, nincs adatvesztés).
2. **Tápzaj:** a 4G adás-tüskék rángatják az 5V-ot → külön táprail vagy bő puffer-elkó (pl. 1000 µF) a stick tápján.
3. **RF-áthallás:** a TS-9 külső antennát vezesd ki a doboz szélére/kívülre (egyben jobb térerő).
4. Opcionálisan földelt árnyékolólemez a két panel közé (általában felesleges, ha az antenna kint van).

### Energiamérleg (uplink)
- Napi 1 bekapcsolás, ~2 perc aktív: ~2 W × 2 perc ≈ **0,067 Wh/nap** ≈ **~13 mAh/nap** (5V-on). Elhanyagolható.
- Hálózati tápon (2026-07-03) a master energiamérlege nem szorít (always-on); ez a becslés a korábbi akkus tervhez készült.

---

## Master táp — TISZTA HÁLÓZATI (D3, eldöntve 2026-07-03)
A master **hálózati tápot** kap → **always-on**: az ESP sosem alszik, a WiFi + ESP-NOW folyamatosan
él, az uplink bármikor mehet. A deep-sleep / duty-cycle energiakérdés így **tárgytalan** a masteren.

- **Táplánc:** **5V adapter → sima 5V→3.3V konverter** (LDO vagy buck) a master 3.3V sínjéhez. A stick 5V-ot közvetlenül az adapterről kapja.
- **Akkus áthidalás / UPS NINCS** (D3): egy hálózati áramszünet a mastert leállítja; a node-ok tovább mérnek és pufferelnek, a `seq`-gap az újraindulás után detektálja a kimaradást.
- A korábban tervezett **akku/napelem üzemmód elesik** (nincs napi 1 ablakra korlátozott feltöltés).
- A ~40 napos PSRAM-puffer megmarad, de már csak **hálózat-/felhő-kimaradás** elleni tartalék (amíg van hálózati táp, a master fut).

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
| Board | ESP32-S3-Zero | vétel + időbélyeg + felhőzés + **saját mérés** | 4 MB flash / 2 MB PSRAM; kétmagos; **hálózati táp/always-on** → a deep-sleep áram (H4: mA) tárgytalan |
| Mérő-lánc | HBS-SCL-50 + HX711-M + DS18B20+ | **a master maga is mér** (saját kaptár, saját `unit_id` — D4) | mint a node mérőlánca; LDO-ról szűrt 3.3V |
| Checkpoint | AT24C256-I2C modul | reset-túlélő `seq`-mentés | I²C, 32 KB, wear leveling |
| Uplink | ZTE MF79U | 4G WiFi stick (Yettel SIM) | B20+B8; opcionális TS-9 külső antenna |
| Táp | 5V hálózati adapter | always-on master (tiszta hálózati, D3) | nincs akkus áthidalás |
| 3.3V rail | 5V→3.3V konverter (LDO/buck) | master 3.3V ellátás | az 5V adapterről |
| Táp-kapcsoló | MOSFET / load switch | uplink 5V kapcsolása (opcionális) | master GPIO; a stick amúgy is mindig tápon |

---

## Tervezési pontok / kockázatok
- **HX711 tápja kritikus** a méréspontossághoz: tiszta, LDO-ról szűrt 3.3V, ne a süllyedő cellasínről — különben a `weight` az akku töltöttségével driftel.
- **TP4056 csak töltő**, nem buck/boost — a 3.3V külön réteg.
- **WiFi/ESP-NOW azonos csatorna** — korán tesztelendő (kész).
- **MF79U firmware** tápra-felregisztrálása — tesztelendő (lásd uplink).
- **2.4 GHz koegzisztencia** — feltöltés alatt ESP-NOW szünetel.
- ~~**Master ESP nyugalmi fogyasztása** dominálja a táp-mérleget~~ — **tárgytalan** (2026-07-03: hálózati táp, always-on master).

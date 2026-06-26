
# Méhkaptár-mérleg rendszer — projektösszefoglaló

## Rendszerarchitektúra

**Topológia:** master–slave, csillag elrendezés. Több slave node (kaptáronként), egy központi master.

**Kommunikáció:** ESP-NOW (slave→master) + WiFi koegzisztencia. A master WiFi STA módban csatlakozik a routerhez, és az ESP-NOW-t ugyanarra a csatornára húzza; a slave-ek ehhez igazodnak (különben csomagvesztés). Ez nálad ismert terep.

**Vételi pipeline (a master gerince):** producer–consumer szétválasztás.
- Producer = ESP-NOW receive callback: csak bedob a queue-ba (`xQueueSendFromISR`), semmi mást.
- Consumer = dedikált task a másik magon: kivesz, időbélyegez, pufferel, felhőz.
- Az S3 két magja: vétel és felhős I/O nem akadályozza egymást.

**Időbélyegzés:** a master feladata. A slave-en mért idő megbízhatatlan (RTC drift deep sleep alatt), a hiteles időtengely a központi vételi időbélyeg (`rx_millis` + NTP-korrekció).

**Adatperzisztencia:** net-kimaradáskor a consumer azonnal SD-re ment és továbblép. A queue (32 elem) csak a vételi burst pufferelésére van; a kimaradás áthidalása az SD réteg dolga.

**Adatformátum:** fixed-size struct (`payload_t`), nincs parse/string. Slave és master ugyanazt használja, ajánlott `__attribute__((packed))`.

| Mező | Típus | Funkció |
|---|---|---|
| `unit_id` | uint8_t | melyik kaptár/sarok |
| `seq` | uint32_t | szekvencia, hiányzó csomag detektálás |
| `weight` | float | nyers/kalibrált terhelés |
| `temp` | float | drift-korrekció |
| `vbat` | uint16_t | elemfeszültség (mV) |

**Felhőzés:** MQTT/HTTP, retry-kezeléssel.

## BOM — Slave node (kaptáronként)

| Alkatrész | Típus | Funkció | Megjegyzés |
|---|---|---|---|
| MCU | ESP32-C3 SuperMini | mérés + ESP-NOW + deep sleep | egymagos RISC-V; gyári LED leszedendő (rejtett fogyasztó) |
| Mérlegcella | HBS-SCL-50 | terhelés (50 kg) | csak hídkapcsolás, ADC kell hozzá |
| ADC/erősítő | HX711-M | 24-bites mérőátalakító cellához | bemenet 2.6–5.5 VDC; `PD_SCK` magasra húzva altatható |
| Hőmérő | DS18B20+ | hőmérséklet + load cell drift-korrekció | 1-Wire, 4.7 kΩ felhúzó kell |
| Elemmérés | külső feszültségosztó | `vbat` logolás | a TP4056-PROT OUT+ pontjáról, nem a nyers cellapólusról |
| Cella | EVE INR18650/35V | Li-ion akku, ~3500 mAh | 4.2 V töltve → ~3.0 V ürítve |
| Töltő/védelem | TP4056-1A-PROT-USBC | CC/CV töltés USB-C + védelem | 1 A; DW01+FS8205 védelem; kimenet szabályozatlan cellafeszültség |

## BOM — Master / központ

| Alkatrész | Típus | Funkció | Megjegyzés |
|---|---|---|---|
| Board | ESP32-S3-DEV-N16R8-US | vétel + időbélyeg + SD + felhőzés | 16 MB flash / 8 MB PSRAM; kétmagos. *Valószínű, még nem fix.* |
| Tárolás | SD (kártya/modul) | net-kimaradás áthidalása | tartós réteg a queue mögött |
| Táp | **nyitott** | — | nem alszik → folyamatos táp kell (adapter / UPS-jellegű buffer), nem cellás logika |

## Fontos tervezési pontok / kockázatok

- **HX711 tápja kritikus a méréspontossághoz:** az AVDD adja a load cell gerjesztését, így az ADC csak annyira stabil, amennyire a tápja. A HX711-et tiszta, LDO-ról szűrt 3.3 V-ról járasd, ne közvetlenül a süllyedő cellasínről — különben a `weight` az akku töltöttségével driftel, és nehéz elkülöníteni a valódi tömegváltozástól. Low-dropout LDO kell, hogy 3.0 V cellánál is tartsa a 3.3 V-ot.
- **TP4056 csak töltő**, nem buck/boost — a 3.3 V előállítása külön réteg.
- **WiFi/ESP-NOW azonos csatorna** — korán tesztelendő (nálad kész).
- **Master táp még megoldatlan** — külön körüljárandó.
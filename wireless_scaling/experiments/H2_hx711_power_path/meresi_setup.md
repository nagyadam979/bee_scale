# H2 — Mérési setup (HX711 power-path / excitation-lekapcsolás)

Ez a doksi a **konkrét mérési elrendezést** írja le: bekötés, pinkiosztás, hol és hogyan
mérj, milyen alvariánsokat, és mit csinálj, ha túl nagy az áram / nem kapcsol le az
excitation. A firmware: lásd `main/h2_hx711_power_path.c` (a hangolható paraméterek a fájl
tetején). Eredmények: `mereseredmenyek.csv`. Cél/küszöbök: `README.md` pass/fail tábla.

> **A H2 LÉNYEGE — két mérés (lásd 3. szakasz):**
> **(A)** a HX711 modul nyugalmi árama a modul SAJÁT VCC ágában (NEM a teljes node tápjában!),
> **(B)** a load cell excitation feszültsége az E+/E− pontok között (alvásban cél ~0 V).
> A pass CSAK akkor áll, ha az áram < ~5 µA ÉS az excitation ~0 V ÉS nincs LED/szivárgás.

> **Pinout / modul-tények (webről ellenőrizve, 2026-06-28)** — források a 6. szakaszban:
> ESP32-C3 SuperMini pinout: espboards.dev / mozelectronics / lastminuteengineers. A
> klasszikus zöld/piros HX711 breakout-on **NINCS power/státusz LED**, és **NINCS külön
> LDO/regulátor** — a VCC közvetlenül a chipre megy (2.6–5.5 V), a hídgerjesztést a HX711
> **chipen belüli AVDD-regulátora** adja (a zöld boardon R12/R13 osztó állítja, gyárilag
> ~4.3 V, ami 5 V VCC-t feltételez; 3.3 V-ról kisebb E+, de jól megy). Datasheet power-down
> áram ~1 µA (akár ~70 nA a chipre). **A te darabodon ellenőrizd.**

---

## 1. Pinkiosztás

| Funkció | ESP32-C3 SuperMini | Megjegyzés |
|---|---|---|
| HX711 `PD_SCK` (SCK) | **GPIO5** | a firmware MAGASra húzza (power-down), alvás alatt GPIO-hold tartja HIGH-on |
| HX711 `DOUT` (DT) | **GPIO6** | a firmware bemenetnek hagyja, nem olvassa |
| HX711 `VCC` | 3V3 (LDO-ról szűrt 3.3V) | **a mérés alatt a modul VCC ágába kerül a DMM** sorba — lásd 3.(A) |
| HX711 `GND` | GND | közös föld |
| Load cell `E+` | HX711 `E+` | gerjesztés + (excitation, B mérés egyik pontja) |
| Load cell `E−` | HX711 `E−` | gerjesztés − (excitation, B mérés másik pontja) |
| Load cell `A+` | HX711 `A+` | jel + (itt nem olvassuk) |
| Load cell `A−` | HX711 `A−` | jel − (itt nem olvassuk) |
| Kék user LED | GPIO8 | a firmware kikapcsolva tartja (mérés alatt sötét) |
| Vörös power LED | — (nem GPIO) | a node „rejtett fogyasztója"; a HX711-modulon tipikusan NINCS LED |

A `PD_SCK`/`DOUT` GPIO-k a firmware tetején (`HX711_PD_SCK_GPIO`, `HX711_DOUT_GPIO`) — a
H1 pin-terv, NE változtasd. Strapping lábakat (GPIO2/8/9) kerülj általános IO-nak.

---

## 2. Bekötési diagram (ASCII)

Load cell 4 vezeték (HBS-SCL-50; a színek a cella adatlapja dönti el, de gyakorlatilag
univerzális): **piros=E+, fekete=E−, zöld=A+, fehér=A−** (sárga = árnyékolás, ha van).
A jel-vezetékek (zöld/fehér) felcserélése csak a mért érték előjelét fordítja.

```
              ESP32-C3 SuperMini                         HX711-M modul                 load cell
           +----------------------+                   +----------------+              (HBS-SCL-50)
           |                      |    (A) DMM uA      |                |
           |                  3V3 |---[ µA ]---------->| VCC            |
           |                  GND |------------------->| GND            |
           |   GPIO5 (PD_SCK) o---|------------------->| PD_SCK / SCK   |
           |   GPIO6 (DOUT)   o<--|-------------------| DOUT / DT      |
           |   GPIO8 (kek LED) .  |                   |                |
           |   [piros power LED]  |                   |  E+ ---------- piros  (E+)
           +----------------------+                   |  E- ---------- fekete (E-)
                                                       |  A+ ---------- zold   (A+)
           (A) µA a HX711 VCC agaban SOROSAN           |  A- ---------- feher  (A-)
                                                       +----------------+
                                                          ^   ^
                                                          |   |
                                          (B) DMM V ----[ V ]----  a load cell E+/E- koze
                                                  (excitation: ebren > 0 V, alvasban ~0 V = PASS)
```

A node kék LED (GPIO8) a firmware-ben kikapcsolva; a vörös power LED a SuperMinin tápra mindig
világít (a HX711-modulon tipikusan nincs LED). A két DMM-bekötést (A és B) **külön** mérd.

---

## 3. A KÉT MÉRÉS — ez a H2 lényege

### (A) HX711 modul nyugalmi árama — a modul SAJÁT VCC ágában

A DMM-et (DC áram, **µA tartomány**) **SOROSAN a HX711 modul VCC ága közé** kösd: a board
**3V3** lába és a modul **VCC** lába közé. **NEM** a teljes node tápjába!

> **Miért a modul VCC ágában, és nem a teljes node tápjában?** A node maga ~185 µA-t vesz fel
> (H1-ben mért alapszám: AMS1117 LDO + GPIO/RTC). E mellett a HX711 ~1–5 µA-ja
> **differenciál-méréssel** (teljes node HX711-gyel mínusz HX711 nélkül) elveszne a zajban
> (185 vs 186 µA — leolvashatatlan). Ezért a HX711-modul áramát **KÜLÖN**, a saját VCC
> ágában mérjük: így közvetlenül a modul nyugalmi árama jön ki, ez hasonlítható a < ~5 µA
> küszöbhöz.

- DMM-beállítás: DC áram, µA tartomány (auto-range esetén várj a stabil értékre). A piros
  vezeték a műszer **A/µA hüvelyébe** (külön bemenet az árammérésnek), különben rövidre zárod.
- Olvasd le a stabil deep-sleep értéket, és írd a CSV `hx711_aram_uA` oszlopába.

### (B) Excitation feszültség — a load cell E+/E− pontjai között

A DMM-et (**V tartomány**) a load cell **E+ és E−** pontjai közé kösd (a HX711 modul E+/E−
kivezetésére).

> **FONTOS — az "ébren" referencia kell:** a firmware alapból a boot legelején AZONNAL
> power-downba teszi a HX711-et, így nincs ablak, amikor a chip aktív. A puszta ~0 V alvásban
> egy szakadt/rosszul bekötött cellánál is teljesülne → az önmagában NEM bizonyít
> *lekapcsolást*. Ezért a (B) méréshez kell egy "be" referencia, amit az `EXCITATION_PROBE_MS`
> paraméter ad (lásd a firmware tetején). **A két mérés KÉT KÜLÖN flasheléssel történik:**
>
> - **(A) áram-mérés:** `EXCITATION_PROBE_MS = 0` (a tiszta, azonnali power-down).
> - **(B) excitation-mérés:** `EXCITATION_PROBE_MS = 30000` (30 s) — vagy amennyi a méréshez
>   kell —, majd **flashelj újra**. Boot után a firmware ennyi ms-ig AKTÍVAN tartja a HX711-et
>   (PD_SCK LOW, órajel-impulzus nélkül), és a logban kiírja:
>   `EXCITATION PROBE: HX711 AKTIV ... mERd most az E+/E-...`.

- **A probe-ablakban (HX711 aktív, a log jelzi):** a chip ad gerjesztést. 3.3 V-os VCC-nél a
  gyári R12/R13 osztóval kisebb (~2–3 V nagyságrend) — a lényeg, hogy **NEM nulla**. Ezt mérd
  az E+/E− között → ez a "be" referencia (> 0 V).
- **Alvásban** (a probe lejárt → power-down → deep sleep, PD_SCK magas): a chipen belüli
  AVDD-regulátor leáll → a cél **~0 V**. Mérd újra az E+/E−-t.
- **Várt eredmény:** probe-ablakban > 0 V, alvásban ~0 V → az excitation lekapcsol = **PASS**
  (a B feltétel). Az "ébren" (probe) mért E+/E− értéket a CSV `megjegyzes` oszlopába jegyezd fel.

---

## 4. Mérési menet

> **A két mérés két külön flashelés** (külön `EXCITATION_PROBE_MS` értékkel a firmware tetején):
> az (A) áram-méréshez `0`, a (B) excitation-méréshez `> 0` (pl. `30000`).

1. **Flashelés.** Állítsd be az `EXCITATION_PROBE_MS`-t az aktuális méréshez (lásd fent), majd
   `idf.py set-target esp32c3 && idf.py build`, és `idf.py -p <PORT> flash monitor` (vagy
   `pio run -t upload -t monitor`).
   - **Tartós deep sleepnél (`DEEP_SLEEP_WAKEUP_SEC` == 0):** a board alvásban NEM enumerál
     USB-n → nincs port. **Download mód kell:** BOOT gomb nyomva + USB ki/be (vagy reset) →
     download mód, majd BOOT elengedése, és flashelj.
   - Ciklikus méréshez `DEEP_SLEEP_WAKEUP_SEC` > 0 megkerüli (periodikusan ébred → enumerál).
2. **(A) áram-mérés — `EXCITATION_PROBE_MS = 0`.** A boot-log kiírja: `Deep sleepbe lepunk...`
   — **innentől** van a modul power-downban, a multiméter ettől mutat nyugalmi áramot. Olvasd
   le a HX711 VCC ágában (3.(A)), és írd a CSV `hx711_aram_uA` oszlopába. (A monitor
   lecsatlakoztatása nem ébreszti fel; tartós alvásnál csak RESET ébreszt.)
3. **(B) excitation-mérés — `EXCITATION_PROBE_MS = 30000`, újraflash.** Boot után a log kiírja:
   `EXCITATION PROBE: HX711 AKTIV ...` — **a probe-ablakban** (HX711 aktív) mérd az E+/E−-t
   (várt > 0 V = "be" referencia). Amikor a probe lejár és `Deep sleepbe lepunk...` jön
   (power-down → deep sleep), mérd újra az E+/E−-t (várt ~0 V) → a lekapcsolás igazolva.
   A "be" értéket a CSV `megjegyzes`-be, az alvásbeli ~0 V-ot az `excitation_V` oszlopba.

---

## 5. Hibakeresés (sorrendben — a modulcsere a VÉGE, nem az eleje)

1. **Áram 100+ µA ÉS a cellán nem 0 V → szinte biztos, hogy a `PD_SCK` nem marad magasan
   deep sleepben** (az ESP elengedte a lábat). Ez a leggyakoribb valós ok — **NEM modulhiba**
   (lásd kohacraft). Ellenőrizd, sorban:
   - (i) a boot-log kiírta-e: `GPIO hold: PD_SCK(GPIO5) HIGH es LED(GPIO8) megtartva...`?
   - (ii) mérd meg DMM-mel a `PD_SCK` (GPIO5) lábat **alvásban** — tényleg HIGH (~3.3 V)?
   - (iii) **biztonsági háló:** tegyél **10–50 kΩ felhúzót a PD_SCK-ra 3.3 V-ra**
     (a 47–50 kΩ energiatakarékosabb), hogy a láb akkor is magasan maradjon, ha a hold elengedne.
2. **Ha a `PD_SCK` bizonyítottan magasan van (>60 µs), de a modul mégis 100+ µA-t visz ÉS a
   cellán feszültség marad → atipikus modul** (rossz klón chip, vagy egy külön, mindig-
   bekapcsolt LDO a hídon, ami a chip power-downja ellenére táplálja). **EKKOR indokolt a
   modulcsere** ismert jó, dokumentált schematic-ú darabra (SparkFun / Soldered).
3. **LED.** Ha mA-es fogyasztást látsz: ellenőrizd, van-e LED a modulon. A tipikuson NINCS
   (SparkFun + Soldered nyílt referenciaterv LED nélküli). Ha mégis → atipikus/hibás variáns,
   modulcsere.
4. **3.3 V-os üzem megjegyzése.** A gyári R12/R13 osztó **5 V-ra** van hangolva → 3.3 V-on
   kisebb az E+ (ezért láthatsz ébren ~2–3 V-ot, nem ~4.3-at). A H2 méréshez ez **nem baj**
   (úgyis a *lekapcsolást* igazoljuk), de a HW-pontossághoz (Sz1, mérési pontosság) később
   **R12-csere** kellhet (Hackaday „Honeybee hive monitoring" R12/R13 módosítás).

---

## 6. Mérési napló — kitöltésre

> Ide a tényleges mérés napján írd be a setupot és az eredményt (modul, PD_SCK ellenőrzés,
> (A) áram, (B) excitation ébren/alvásban, pass/fail). A számokat a `mereseredmenyek.csv`-be is.

### YYYY-MM-DD — (kitöltendő)

ESP32-C3 SuperMini, **3.3V a 3V3 lábra**, HX711-M modul + load cell (HBS-SCL-50) bekötve.

| Lépés | Mérés | Érték | Megjegyzés |
|---|---|---|---|
| PD_SCK (GPIO5) alvásban | DMM V a lábon | _(HIGH? ~3.3 V?)_ | a log kiírta a GPIO-hold sort? |
| (A) HX711 nyugalmi áram | DMM µA, VCC ág sorban | _(µA)_ | < ~5 µA = jó |
| (B) excitation ÉBREN | DMM V, E+/E− | _(V)_ | > 0 V várt (3.3 V-on ~2–3 V) |
| (B) excitation ALVÁSBAN | DMM V, E+/E− | _(V)_ | ~0 V = lekapcsol = PASS |
| **Összesítés** | pass/fail | _(pass csak ha mind3)_ | áram < ~5 µA + excitation ~0 V + nincs LED |

**Tanulságok:** _(kitöltendő — mi működött, mi nem; pl. kellett-e PD_SCK felhúzó, modulcsere)_

---

## 7. Források (webről, 2026-06-28)

- **HX711 modul (LED, LDO, AVDD-regulátor, R12/R13):**
  Soldered HX711 hardware docs — https://docs.soldered.com/hx711/hardware/ ;
  SparkFun HX711 breakout (nyílt schematic, PowerTest 70 nA) ;
  Hackaday „Honeybee hive monitoring — Modifying the HX711 Breakout Board for 3.3V" (R12/R13).
- **Power-down áram / PD_SCK magasan tartás (a fő valós ok):**
  kohacraft — https://en.kohacraft.com/archives/esp32-reduced-current-consumption-during-deepsleep-by-putting-hx711-into-power-down-mode.html (~120 µA, fő ok: PD_SCK nem maradt magasan) ;
  Arduino Forum „Reducing current consumption of HX711" ; Hiveeyes community.
- **ESP32-C3 SuperMini pinout:** espboards.dev/esp32/esp32-c3-super-mini/ ;
  mozelectronics.com ; lastminuteengineers.com ESP32-C3 Super Mini pinout.

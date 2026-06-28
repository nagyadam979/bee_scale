# H1 — Mérési setup (deep-sleep áram)

Ez a doksi a **konkrét mérési elrendezést** írja le: bekötés, pinkiosztás, hol és hogyan
mérj, milyen alvariánsokat, és mit csinálj, ha túl nagy az áram. A firmware: lásd
`main/h1_node_board_prep.c` (a hangolható paraméterek a fájl tetején). Eredmények:
`mereseredmenyek.csv`. Cél/küszöbök: `README.md` pass/fail tábla.

> **Pinout forrás (webről ellenőrizve, 2026-06-28):** espboards.dev ESP32-C3 Super Mini,
> mozelectronics.com, lastminuteengineers.com. A user LED a legtöbb darabon **GPIO8**
> (tipikusan aktív-alacsony), a **vörös power LED nem GPIO-vezérelt** (tápra mindig világít),
> a gyári deep-sleep áram **~43 µA** körül. **A te darabodon ellenőrizd** (a LED-GPIO és a
> power-LED jelenléte gyártófüggő lehet).

---

## 1. Pinkiosztás

| Funkció | ESP32-C3 SuperMini | Megjegyzés |
|---|---|---|
| HX711 `PD_SCK` | **GPIO5** | a firmware MAGASra húzza (power-down), alvás alatt RTC-hold tartja |
| HX711 `DOUT` | **GPIO6** | a firmware bemenetnek hagyja, nem olvassa |
| HX711 `VCC` | 3V3 (LDO-ról szűrt 3.3V) | üzemben tiszta 3.3V kell; mérésnél a power-downba küldött modul ~1 µA jó modulon |
| HX711 `GND` | GND | közös föld |
| Kék user LED | GPIO8 | a firmware kikapcsolva tartja (mérés alatt sötét) |
| Vörös power LED | — (nem GPIO) | **HW-ben leszedendő** — ez a „rejtett fogyasztó" |
| Táp a méréshez | 3V3 + GND (vagy 5V + GND) | lásd 3. szakasz: hol fűzd be a multimétert |

A `PD_SCK`/`DOUT` GPIO-k a firmware tetején (`HX711_PD_SCK_GPIO`, `HX711_DOUT_GPIO`)
szabadon átírhatók, ha máshová kötöd. Strapping lábakat (GPIO2/8/9) kerülj általános IO-nak.

---

## 2. Bekötési diagram (ASCII)

```
                 ESP32-C3 SuperMini                         HX711-M modul
              +----------------------+                    +----------------+
              |                  3V3 |-------+----------->| VCC            |
              |                  GND |----+--|----------->| GND            |
              |   GPIO5 (PD_SCK) o---|----|--|----------->| PD_SCK / SCK   |
              |   GPIO6 (DOUT)   o<--|----|--|------------| DOUT / DT      |
              |   GPIO8 (kek LED) .  |    |  |            |                |
              |   [piros power LED]  |    |  |            |   E+  E-  A+ A-  |
              +----------------------+    |  |            +----|---|---|--|-+
                                          |  |                 |   |   |  |
            piros power LED = leszedni    |  |              load cell (HBS-SCL-50)
                                          |  |              4 vezetékes híd
                                          |  |
   MÉRÉS:  a táp + ágába SOROSAN a multiméter (µA) — lásd 3. szakasz
                                          |  |
        +-----[ µA multiméter ]-----+     |  |
        |                           |     |  |
   3.3V tápforrás (+) -------------(+)   (-)=GND-re
```

A load cell híd (E+/E-/A+/A-) a HX711-re köt; a deep-sleep áramméréshez a load cell
maradhat bekötve (a power-downban az excitation lekapcsol — ennek 0 V igazolása a **H2**
chunk dolga). H1-nél a lényeg, hogy a HX711 power-downban van.

---

## 3. Hol és hogyan mérj (a lényeg)

A deep-sleep áramot a **táp + ágába SOROSAN kötött multiméterrel** méred — nem párhuzamosan!

1. **Multiméter beállítás:** DC áram, **µA tartomány** (a node deep sleepben ~tíz–száz µA).
   Ha a műszer auto-range, várj a stabil értékre; ha kézi, indulj a legkisebb µA tartományból,
   ami nem ír túlcsordulást. Ügyelj, hogy a műszer **A/µA hüvelyébe** van a piros vezeték
   (külön bemenet az árammérésnek a legtöbb DMM-en), különben rövidre zárod a tápot.
2. **Beillesztési pont:** a tápforrás **plusz ága** és a board táp-bemenete közé sorba.
   - **LDO fent alvariáns:** 5V-ot adsz a board 5V/VBUS lábára (vagy USB-ről egy árammérő
     break-out kábellel), és a multimétert az **5V + ágába** fűzöd. Így a board saját
     LDO-ját (és a vörös power LED-et, ha még rajta van) is méred.
   - **LDO megkerülve alvariáns:** közvetlen **3.3V**-ot adsz a board **3V3** lábára (az
     LDO-t kikerülve), a multimétert a **3.3V + ágába** fűzöd. Így csak az ESP + HX711
     fogyasztását méred, a board LDO nyugalmi árama nélkül.
3. **A boardot deep sleepbe kell hozni a méréshez:**
   - Flasheld a firmware-t: `idf.py set-target esp32c3 && idf.py build`,
     majd `idf.py -p <PORT> flash monitor`.
   - A boot-log kiírja: `Deep sleepbe lepunk...` — **innentől** mutat a multiméter
     nyugalmi áramot. (A monitor lecsatlakoztatása nem ébreszti fel; a `DEEP_SLEEP_WAKEUP_SEC`
     == 0 miatt csak RESET ébreszti.)
   - Várj pár másodpercet a stabil leolvasásig.
4. **Olvasd le és írd be** a `mereseredmenyek.csv`-be (állapot → µA → pass/fail).

> **Megjegyzés a tápról:** ha USB-ről táplálsz a méréshez, az USB-Serial-JTAG (GPIO18/19)
> él marad — ez torzíthat. A legtisztább deep-sleep számhoz **külső labortáp**ról (3.3V a
> 3V3 lábra) mérj, USB lecsatlakoztatva. Az USB-s és külső-tápos érték közti különbséget
> jegyezd fel a megjegyzés oszlopba.

---

## 4. Mérendő alvariánsok (sorrend)

Mérd végig és minden sort rögzíts a CSV-be. A „cél állapot" a 4. sor (teljes node).

| # | board_allapot | ldo_allapot | hx711_allapot | mit mutat |
|---|---|---|---|---|
| 1 | gyari LED | LDO fent | HX711 lecsatlakoztatva | gyári alapszám, csak a board (~43 µA várt) |
| 2 | LED leszedve | LDO fent | HX711 lecsatlakoztatva | a power LED hatása (1. − 2. = a LED árama) |
| 3 | LED leszedve | LDO megkerulve | HX711 lecsatlakoztatva | csak az ESP, board-LDO nélkül |
| 4 | LED leszedve | (a választott) | HX711 csatlakoztatva (power-down) | **TELJES NODE — ide kell a < 50 µA** |

A 4. sorban az LDO-állapotot a 2.–3. eredménye alapján választod (egyszerűség vs. fogyasztás
trade-off — lásd README nyitott kérdés). A teljes-node pass a **H2**-vel közös (HX711 < ~5 µA
+ excitation 0 V).

---

## 5. Hibakeresés, ha > 500 µA (problémás)

1. **Még rajta a vörös power LED?** Ez önmagában nagyságrendet ronthat — szedd le, mérj újra.
2. **HX711 valóban power-downban?** Ellenőrizd, hogy `PD_SCK` (GPIO5) tényleg MAGAS deep
   sleep alatt (mérd meg DMM-mel a lábon). Ha nem, a GPIO-hold nem fog — nézd meg a logot
   (`GPIO hold: PD_SCK(GPIO5) HIGH ... megtartva`). Olcsó HX711-modulokon saját LED / szivárgó tápút lehet
   (~120 µA) — ez a H2 tárgya, de itt is jelentkezik.
3. **A board tényleg deep sleepben van?** A boot-log kiírta a `Deep sleepbe lepunk...`-ot?
   Ha folyamatosan újraindul, valami wakeup-forrás aktív vagy brown-out van (gyenge táp).
4. **USB csatlakoztatva van?** Az USB-Serial-JTAG fogyaszt — mérj külső tápról, USB nélkül.
5. **Lebegő GPIO-k?** Bekötetlen, magas-impedanciás bemenetek oszcillálhatnak. A firmware a
   használt lábakat definiálja; ha sok minden lóg a boardon, az is szivároghat.
6. **Maga az LDO nyugalmi árama** (LDO fent esetben) — hasonlítsd az „LDO megkerülve"
   sorhoz; ha az LDO a fő fogyasztó, az megkerülés melletti érv.
7. **USB-Serial-JTAG pad** — a C3-on deep sleepben is bekapcsolva marad (~500–800 µA!), ha a
   console a USB-JTAG-on van. A firmware ezt deep sleep előtt lekapcsolja
   (`sleep_console_usj_pad_backup_and_disable()`). Ha mégis ~700 µA-t mérsz, ellenőrizd, hogy
   a friss firmware van-e flashelve (lásd lent a naplót).

---

## 6. Mérési napló — 2026-06-28

ESP32-C3 SuperMini, **3.3V a 3V3 lábra** adva (az on-board AMS1117 chip rajta), **HX711 modul nélkül**.

| Lépés | Állapot | Áram | Megjegyzés |
|---|---|---|---|
| Eredeti fw, power-LED-del | USB-JTAG pad ébren | ~1 mA | a USB-JTAG nem volt lekapcsolva |
| + power-LED ellenállás leszedve | USB-JTAG pad ébren | 731 µA | a LED ~270 µA-t vitt |
| + USB-JTAG pad off (**fw fix**) | deep sleep, pad lekapcsolva | **~185 µA** | a USB-JTAG ~550 µA volt! |

**Tanulságok:**
- **A USB-Serial-JTAG pad volt a fő fogyasztó (~550 µA).** A firmware deep sleep előtt
  lekapcsolja; a boot-log USB-n megmarad. (Commit `5a34a60`.)
- **Maradék ~185 µA ≈ AMS1117 LDO (~100 µA) + GPIO/RTC.** Az LDO leszedése (a <50 µA „jó"-hoz)
  **fejlesztés alatt felejtős**: az LDO állítja elő a 3.3V-ot az USB 5V-ból, leszedve **nincs
  USB-flash**. Prodban dedikált boardokon áldozható be; addig a **~185 µA „elfogadható"** szint marad.
- **Flashelés tartós deep sleepnél:** a board alvásban nem enumerál USB-n → nincs port.
  Download mód kell: **BOOT gomb nyomva + USB ki/be (vagy reset)**, majd BOOT elengedése.
  (Ciklikus méréshez `DEEP_SLEEP_WAKEUP_SEC` > 0 megkerüli, mert periodikusan ébred.)

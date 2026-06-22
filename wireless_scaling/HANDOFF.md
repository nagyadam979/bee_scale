# wireless_scaling — node ház: tervezési státusz + átadási doksi

> Cél: parametrikus, vízálló (~IP65), 3D-nyomtatható (ASA) ház a méhkaptár-mérleg
> **slave node-hoz**. build123d (Python) + ocp-vscode élő nézet. Ez a doksi mindent
> rögzít, ami eddig megszületett, hogy egy friss ágens folytatni tudja.
>
> Utolsó frissítés: 2026-06-22.

---

## 1. KONCEPCIÓ (tisztázott, 2026-06-22)

- **1 node = 1 önálló egység**, a következő alkatrészekkel:
  ESP32-C3 SuperMini + HX711 + **1 db négyzetes fél-hidas 50 kg load cell** + 18650 cella
  (holderrel) + TP4056 USB-C töltő/védő + DS18B20 (TO-92).
- **4 ilyen node** kerül a kaptár **4 sarka alá**; mindegyik node a kaptársúly **~negyedét**
  méri. → 4×50 kg = 200 kg rendszer-kapacitás, bőven elég.
- **Háztömeg célméret: ~50–100 mm él (5–10 cm).** A **18650 holder 74.5 mm hosszú** a
  limitáló alkatrész → a doboz egyik éle **min. ~80 mm** marad. A jelenlegi modell
  100×80 mm belmérete TÚL NAGY egy node-hoz → **újra kell méretezni** (lásd 5. szakasz).
- Anyag: **ASA** (UV-álló, kültéri) — csak megjegyzés, geometriát nem érint.
- Védelem: ~**IP65** — körbefutó tömítőhornyok + PG7 (vagy kisebb) tömszelencék, nincs nyitott rés.

---

## 2. ALKATRÉSZ-SPECIFIKÁCIÓK (web-kutatás, forrásolt — 2026-06-22)

Minden adat 6 párhuzamos kutató-ágens eredménye. **A ⚠️-vel jelölt értékek a fizikai
darabon LEMÉRENDŐK** a végleges geometria előtt (kínai OEM-tűrés / hiányzó kóta).

| Alkatrész | Befoglaló (mm) | Rögzítés / furat | Megjegyzés a házhoz |
|---|---|---|---|
| **Mérőcella** (négyzetes fél-híd, SC902/YZC) | **34 × 34 × 7.8** (perem 2.5, dóm 7.8, sarok 4×R10, belső kontúr 28) | 3 furat egy középvonalon: **1 nagy közép (terhelő, dóm, felfelé)** + **2 kis fix (M4)** | ⚠️ furat-középtáv és átmérő NEM kótázott → LEMÉRENDŐ. Datasheet-képek: `datasheets/cell_SC902/` |
| **ESP32-C3 SuperMini** | 22.5 × 18 (24 × 18 USB-C-vel) | **NINCS furat** → szorítóbordás fogás | USB-C a rövid él közepén, antennával átellenes vég, ~2 mm túlnyúlás |
| **HX711** (zöld generic) | 34 × 21 × 3 | 4 sarokfurat ~Ø2.8 ⚠️ (osztás klónfüggő) → szorítóbordás fogás | csatlakozók a 2 szemközti rövid élen (load cell oldal: E+/E-/A-/A+/B-/B+; digitális: GND/DT/SCK/VCC) |
| **TP4056 USB-C PROT** (DW01+FS8205) | 29 × 17.3 × 4.3 | **NINCS furat** → szorítóbordás fogás | USB-C az egyik rövid élen (töltéshez hozzáférés kell!); B+/B-/OUT+/OUT- a szemközti élen |
| **18650 holder** (HESTORE/generic, vezetékkel) | **74.5 × 21.5 × 17.7** | nincs fül; **vezeték a KÉT ELLENTÉTES végén** | a hossz (74.5) ~kitölti a dobozt → kábelhajlatnak hely kell a végeken |
| **18650 cella** (EVE INR18650/35V) | Ø18.5 × 65 | — | a holderben |
| **DS18B20** | **TO-92 tok** (~4.6×4.6×3.9, láb 1.27 raszter) | — | ⚠️ **SIMA TO-92, NEM vízálló szonda** → kábel = 3 vékony ér (nincs Ø4 köpeny); PG7 lazán tömít rá → PG5/M8 vagy közös köpeny |

### Mérőcella — szerelési elv (datasheet + több forrás megerősítve)
A négyzetes fél-hidas cella egy középső **E-gerenda**:
- **Fix vég** (2 kis M4 furat) = a cella pereme **felfekszik a ház tetejére**, alulról/felülről csavar.
- **Terhelt vég** (nagy közép furat, a dóm) = **felfelé** áll, ide nyom a kaptár súlya (a gerenda hajlik).
- A perem (2.5) és a dóm (7.8) eltérő síkon → a cella a tető **sík** lapján ül a fix peremnél;
  a dóm alá **nem** kell süllyesztés (a cella saját geometriája adja a hajlásteret).
- A teher útja: kaptár → (teherbevivő elem a dómra) → cella → fix vég → háztető → állvány/talaj.

```
   plan (felülnézet, 34×34, R10 sarok):        side (oldalprofil):
   ┌───────────────┐                              ___/‾dóm 7.8‾\___
   │  ╔═════════╗  │   o = 2× kis FIX furat       ▔▔perem 2.5▔▔▔▔▔▔
   │  ║ o  O  o ║  │   O = 1× nagy TERHELŐ furat   (fix perem felfekszik a tetőre,
   │  ╚════╤════╝  │       (középen, felfelé)       dóm felfelé viszi a terhet)
   └───────│───────┘
        kábel (3 ér: Piros=Sig, Fehér=Exc+, Fekete=Exc−)
```

### Források (kulcs)
- Mérőcella SC902 datasheet (méretezett rajz): https://akizukidenshi.com/goodsaffix/SC902-50kg.pdf
- Mérőcella szerelés (fix/floating, spacer): https://circuitjournal.com/50kg-load-cells-with-HX711
- ESP32-C3 SuperMini: https://www.espboards.dev/esp32/esp32-c3-super-mini/ , https://sigmdel.ca/michel/ha/esp8266/super_mini_esp32c3_en.html
- HX711 modul: https://www.electrokit.com/en/hx711-lastcellsforstarkare-monterad-pa-kort
- TP4056 USB-C: https://grobotronics.com/lithium-battery-charger-and-protection-module-1a-usb-c-tp4056.html
- 18650 holder: https://www.sparkfun.com/battery-holder-1x18650-wire-leads.html
- DS18B20 TO-92: https://eesemi.com/to92.htm , https://en.wikipedia.org/wiki/TO-92

---

## 3. MEGSZÜLETETT DÖNTÉSEK

- **PCB-rögzítés: szorítóbordás, CSAVAR NÉLKÜL** (a SuperMini és TP4056 furat nélküli, a HX711
  furatképe bizonytalan). Becsúsztatós sarok-oszlopok (oldalfogás) + sarok-padok (forrasztás-hézag).
  → már implementálva a `bottom_lid`-ben (`pcb_nests`).
- **Mérőcella: NÉGYZETES fél-hidas** (SC902/YZC ~34×34), NEM TAL220 gerenda, NEM single-point.
  → a tető furatképe MÉG NINCS átírva erre (lásd 5. szakasz, ez a fő nyitott munka).
- **Ház architektúra** (változatlan, jó): alul nyitott doboz; tömör+bordázott felső plató =
  mérőcella-felfogó; fő fedél = alsó lap (PCB-tartó); belső válaszfal + oldalsó akku-ajtó a 18650-hez.

---

## 4. JELENLEGI MODELL ÁLLAPOTA (`enclosure.py`)

Működik, fordul, exportál (STL+STEP mindhárom partra + assembly.step az `export/`-ba),
az ocp-vscode viewerbe push-ol. **Mindhárom alkatrész 1-1 összefüggő solid** (igazolva).

3 part:
- **`housing`** — falak + tömör felső plató (bordák + mérőcella-furatkép), belső válaszfal
  (kábel-átvezető furattal), PG7 tömszelence-furatok (−Y fal), akku-ajtó nyílás + sarok-bossok,
  alsó-zárás körbefutó tömítőhoronnyal + sarok-bossok (gussettel a horony fölött).
- **`bottom_lid`** — fő fedél = PCB-tartó alaplap, szorítóbordás `pcb_nests` fészkekkel, 4 sarok
  átmenő csavarfurat.
- **`battery_door`** — oldalsó akku-ajtó, 4 sarok csavarfurat (assembly-orientáció: `Rot(0,90,90)`).

**Minden méret a fájl tetején, kommentált modul-szintű változó.** Fontos build123d részletek
(amit egy folytató ágensnek tudni kell):
- algebra API; `Box(...,align=(C,C,MIN))` az aljat z=0-ra teszi; boolean `+`/`-`/`&`.
- Henger más tengelyre: `Rot(0,90,0)` → +X, `Rot(-90,0,0)` → −Y. (Empirikusan ellenőrzött.)
- `bounding_box()` ~0.3 mm toleranciát ad hozzá — nem valódi geometria.
- A `show()` `try/except`-ben van → headless futáskor sem dől el az export.

### ⚠️ Ami a modellben MÉG NEM HELYES (a node-koncepcióhoz képest)
1. **Tető furatkép**: MÉG a régi single-point 2×2 minta (`cell_hole_pattern`). ÁT KELL ÍRNI a
   négyzetes cella furatképére: 2 kis fix (M4) + 1 nagy közép terhelő, egy középvonalon.
2. **Dobozméret**: `inner_x/y/z = 100×80×55` egy 1-node egységhez TÚL NAGY. Csökkentendő
   ~80×60×(40–50) körülre (a 18650 holder 74.5 mm-e a limit egy él mentén).
3. **USB-C hozzáférés**: a TP4056 és SuperMini USB-C-jéhez nincs házfal-nyílás (a panelek középen
   ülnek). El kell dönteni: töltés az akku-ajtón át / dedikált tömített USB-port / pad-fal mellé.

---

## 5. KÖVETKEZŐ LÉPÉSEK (prioritás szerint)

1. **Dobozméret újrahangolása** 1-node-ra (~80×60×45), a tényleges alkatrész-elrendezéssel:
   1 cella a tetőn + alatta HX711 + SuperMini + TP4056 + 18650 oldalrekeszben.
2. **Tető furatkép átírása** a négyzetes cellára (2 fix M4 + 1 terhelő közép). Furat-középtáv
   PLACEHOLDER + jól kommentált, amíg a felhasználó le nem méri. Heat-set M4 betét a 2 fix furatba.
3. **Teherbevivő megoldás** a dómra (a kaptár felőli teher hogyan jut a cella nagy középfuratára):
   pl. egy felső teherbevivő csavar/gomb a platón áthaladva — egyeztetendő.
4. **USB-C hozzáférés** eldöntése + nyílás.
5. PCB-fészek pozíciók véglegesítése a kábelezés szerint; gland-furatok finomítása (DS18B20 vékony kábel).
6. Opcionális: külső fülek/talp a node rögzítéséhez a kaptárállványhoz.

**A felhasználónak le kell mérnie (tolómérő) a fizikai darabokon:** a cella 2 fix furatának
középtávját + átmérőjét; a HX711 sarokfurat-osztást (ha mégis csavaroznánk); a 18650 holder pontos méretét.

---

## 6. KÖRNYEZET

```bash
cd wireless_scaling
python3.11 -m venv .venv          # 3.11 kell: a 3.14-hez nincs cadquery-ocp wheel
source .venv/bin/activate
pip install -r requirements.txt   # build123d==0.11.0, ocp_vscode==3.4.0
```
VS Code: „OCP CAD Viewer" extension → `Cmd+Shift+P` → „OCP CAD Viewer: Open viewer" (port 3939),
majd `python enclosure.py` (vagy `python watch.py` mentésre-frissüléshez). Részletek: `README.md`.

Commit-történet (dev_01 branch) tartalmazza a fejlesztési lépéseket és a megtalált+javított hibákat.

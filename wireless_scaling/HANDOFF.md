# wireless_scaling — node ház: tervezési státusz + átadási doksi

> Cél: parametrikus, vízálló (~IP65), 3D-nyomtatható (ASA) ház a méhkaptár-mérleg **slave
> node-hoz**. build123d (Python) + ocp-vscode élő nézet. Ez a doksi a FRISS állapotot rögzíti,
> hogy egy új ágens folytatni tudja.
>
> Utolsó frissítés: 2026-06-22 (falvékonyítás + alkatrész-pakolás session után).

---

## 0. KÖRNYEZET (kész, NE telepítsd újra)

A `.venv` létezik Python 3.11-gyel (a 3.14-hez nincs cadquery-ocp wheel).

```bash
cd /Users/adamnagy/Documents/bee_scale/wireless_scaling && source .venv/bin/activate
python enclosure.py   # ház export (STL+STEP+assembly.step) + viewer push (port 3939)
python validate.py    # mindhárom part: 1 összefüggő solid? + befoglaló méretek
python layout.py      # alkatrész-PAKOLÁS vizualizáció (ház átlátszón + alkatrész-makettek)
```

A `show()` try/except-ben van → headless futáskor sem dől el az export. Branch: **dev_01**
(a részletes commit-történet tükrözi a döntéseket és a mi-működött/mi-nem lépéseket).

### Fájlok
- **`enclosure.py`** — a parametrikus ház (3 part: `housing`, `bottom_lid`, `battery_door`).
  Minden méret a fájl tetején, kommentált változó. `stadium_x()` helper az USB-C stadion-nyíláshoz.
- **`layout.py`** — alkatrész-makettek (valós befoglalók) a `COMPONENTS` táblában PAKOLHATÓK
  (`pos`/`axis`); a ház átlátszón; konzolra kiírja a befoglalókat + a befoglaló-ÜTKÖZÉSEKET.
- **`validate.py`** — gyors solid-szám + bbox ellenőrző.

---

## 1. KONCEPCIÓ

- **1 node = 1 önálló sarokegység:** négyzetes fél-hidas mérőcella (SC902/YZC ~34×34) +
  ESP32-C3 SuperMini + HX711 + TP4056 USB-C + 18650 (holderrel) + DS18B20. 4 node a kaptár 4
  sarka alatt, mindegyik ~kaptársúly/4-et mér.
- A doboz **alul nyitott**; a teteje tömör plató: ide süllyed a mérőcella, és a **kaptár
  KÖZVETLENÜL a cella felfelé álló dómjára fekszik** (nincs közbeiktatott teherbevivő elem).
- **Fő fedél = az alsó lap** (`bottom_lid`), sík záró lap. Anyag: ASA. Védelem: ~IP65.

### Koordináták
X = **RÖVID** él (`inner_x`=60), Y = **HOSSZÚ** él (`inner_y`=80, ide fekszik a 18650 holder ∥ Y),
Z fel. A ház alja z=0, XY-ban origóra centrálva. **Külső méret: 68 × 86 × 39.5 mm.**

---

## 2. JELENLEGI ÁLLAPOT (mind validálva: 3 part = 1-1 összefüggő solid, export fut)

### Méretek / falak (minimalizálva — a statikát a FELHASZNÁLÓ tervezi)
- Belső: `inner_x=60`, `inner_y=80`, `inner_z=36`.
- Falak a **spec-feature minimumon**: `wall=3` (±Y rövid falak — a körbefutó tömítőhorony
  pinneli), `long_wall_th=4` (±X hosszú falak — az ajtó/USB rebate 3 + ~1 fenék),
  `top_th=3.5` (cella-pocket 2.5 + ~1 fenék), `lid_th=1.5`, `partition_th=1.5`.
- **Merevítő bordák ELTÁVOLÍTVA** (tisztán merevítő, nem spec; a statikát a felhasználó végzi).
- A magasság (inner_z=36) **kötő korlátja** az oldalsó akku-ajtó: a nyílás ≥ a cella Ø18.5,
  és a 18650 a Z-középsávot kitölti. (Az oldalajtó elhagyásával ~22-ig lehetne menni.)

### Tető / mérőcella
- Besüllyesztett **pocket** (34×34 + clearance, mély `cell_recess_d=2.5`). A **pocket FENEKE
  TÖMÖR** — nincs furat a cella alatt → felülről teljesen vízálló.
- **Rögzítés:** 2× M4 heat-set furat a süllyesztésen **KÍVÜL**, a hosszú (Y) középvonalon (x=0),
  a recess pereme mellett `cell_screw_clearance=3` mm-re (furat-közép y≈±23.25). Felülről
  **BLIND** (lefelé lógó boss zárja → vízálló). Ide egy áthidaló leszorító bracket csavarozható.

### Akku-ajtó (+X hosszú fal)
- **SÜLLYESZTETT (flush) fedő, CSAVAR NÉLKÜL** (a sarok-bossok + betétfuratok + ajtó-furatok
  eltávolítva). Felépítés: rebate-pocket (`bat_recess_d=3`) + átmenő nyílás (`bat_open_y=68`,
  `bat_open_z=20`) + **tömítőhorony az AJTÓBAN** (vak gyűrű).
- ⚠️ **RETENCIÓ NINCS** (se csavar, se pattintó) → jelenleg súrlódó/becsúsztatott fedő.

### USB-C (−X hosszú fal)
- 2 **stadion (obround) nyílás** (`stadium_x` helper), USB-IF spec méret: `usb_open_w=9`,
  `usb_open_h=3` (plug 8.4×2.56 + hézag, lekerekített végek). A recess (overlap-dugó) bővebb,
  PLACEHOLDER. `usb_ports`: TP4056 (y=−11), SuperMini (y=+13), mindkettő z=18.8.

### PCB-fészkek
- A **HOUSING tartja**, a tető plató ALÁ lógatva (`pcb_seat_z=22`, `pcb_grip=6`); a `bottom_lid`
  sík. `pcb_nests` (név, x, y, w, h): HX711 (−12, 0, 34, 21), SuperMini (−14, **21**, 24, 18.4),
  TP4056 (−14, **−21**, 18, 14). A SuperMini/TP4056 mérete a felhasználó által mért (24×18.4 / 18×14).

---

## 3. ⚠️ A LEGFONTOSABB NYITOTT MUNKA — enclosure.py ↔ layout.py ELTÉRÉS

A **`layout.py` a KÍVÁNT pakolást** mutatja (a felhasználó ott rendezte be); ezt kell átvezetni
az `enclosure.py` mountolásába. A jelenlegi eltérések:

1. **HX711 — vertikális slot:** a layoutban a HX711 a hosszanti (X) tengelye körül 90°-kal
   ELFORGATVA, **ÁLLÓ** (34×1.6×21), z[6,27] (alatta hézag, **NEM ér le az aljáig**), egy kis
   **SLOT**-ba csúszna. Az enclosure.py-ban viszont MÉG a régi VÍZSZINTES szorítóbordás fészek
   van. → Cseréld VERTIKÁLIS SLOT-ra (vezető-horony / sín-pár), aljától elemelve.
2. **SuperMini/TP4056 pozíció:** a layoutban y=+13 / −11 (≈3 mm hézag a HX711-től). Az enclosure
   `pcb_nests` pozíciói még y=±21 → igazítsd y=+13 / −11-re. (A méretük már stimmel.)
3. **18650 holder tartója:** a layoutban a TETŐ ALÁ lógatva (z[18.3,36], +X strip, nyitott oldala
   +X felé), hogy elkerülje a z[0,14] sarok-csavar-bossokat. Az enclosure.py-ban a holdernek
   **NINCS tartója** → tegyél clamp/standoffot a tetőből (mint a paneloknál).
4. **Akku-ajtó nyílás Z:** most z[8,28]-ra centrált, de a holder feljebb került (z[18.3,36]) → a
   nyílást igazítsd a holderhez, hogy a 18650 kivehető legyen az ajtón át.
5. **USB-portok** már a connectorokhoz igazítva (y=−11/+13, z=18.8) — ha tovább pakolsz, tartsd szinkronban.

---

## 4. TOVÁBBI NYITOTT PONTOK

- **Akku-ajtó RETENCIÓ:** jelenleg semmi → kell snap/klips vagy más rögzítés (főleg az IP65
  tömítés-nyomáshoz). A módot egyeztesd a felhasználóval.
- **KÁBEL-BEVEZETÉS:** a −Y fali PG7 gland-furatok törölve → a cella-kábel + DS18B20 bevezetése
  NYITOTT (tető-gland / fal / alsó perem?). Egyeztetendő.
- **Statika:** a teherviselést a FELHASZNÁLÓ tervezi (a top_th=3.5 alatt a cella-pocket ~1 mm
  fenék visz ~50 kg-ot → ő vastagít). NE tegyél vissza merevítést kérés nélkül.
- A `layout.py` panel-MAGASSÁGAI az alaplap-vastagsággal (1.6) számolnak; a komponens-kilógás
  (header, HX711 sorkapocs) NINCS modellezve → ha a pakolásnál kell, add hozzá.
- Cella furat-középtáv/átmérők, USB-port és holder pontos pozíciók: PLACEHOLDER → fizikai
  darabon LEMÉRENDŐK / a végleges elrendezéskor pontosítandók.

---

## 5. MUNKAMÓD (kötelező)

- Minden méret a fájl tetején, kommentált változó maradjon; a placeholder/lemérendő értéket jelöld.
- Validálj MINDEN változás után: `validate.py` (3 part = 1-1 solid), `python enclosure.py`
  (export fut), `layout.py` (nincs befoglaló-ütközés), a befoglaló méretek értelmesek.
- **Commit-fegyelem:** minden értelmezhető lépés után RÉSZLETES commit (mit / miért /
  mi-működött-mi-nem / nyitott pontok). Branch: dev_01.
- VALÓDI tervezési elágazásnál vagy hiányzó méretnél NE tippelj — kérdezz vissza a felhasználótól.
- A felhasználó erős HW+SW hátterű; magyarul, tömören, alapfogalmak magyarázata nélkül.

**ELŐSZÖR:** olvasd be az `enclosure.py`-t és a `layout.py`-t, futtasd a `validate.py`-t és a
`layout.py`-t a friss állapot megtekintéséhez, MIELŐTT bármit módosítasz.

---

## 6. ALKATRÉSZ-SPECIFIKÁCIÓK (web-kutatás + felhasználói mérés)

| Alkatrész | Befoglaló (mm) | Megjegyzés |
|---|---|---|
| Mérőcella (négyzetes fél-híd, SC902/YZC) | 34 × 34 × 7.8 | ⚠️ saját furatképét NEM használjuk (külső M4 bracket); datasheet: `datasheets/cell_SC902/` |
| ESP32-C3 SuperMini | **24 × 18.4** | USB-C a rövid él közepén; furat nincs (user-mért) |
| HX711 (zöld generic) | 34 × 21 × 3 | layoutban ÁLLÓ, slotba; sorkapocs magas lehet |
| TP4056 USB-C | **18 × 14** | USB-C az egyik élen; furat nincs (user-mért) |
| 18650 holder | 74.5 × 21.5 × 17.7 | vezeték a két ellentétes végén; ∥ Y; a tető alá lógatva |
| 18650 cella | Ø18.5 × 65 | a holderben |
| DS18B20 | TO-92 (~4.6×4.6×3.9) | SIMA TO-92 (nem vízálló szonda) → 3 vékony ér |

### Források (kulcs)
- Mérőcella SC902 datasheet: https://akizukidenshi.com/goodsaffix/SC902-50kg.pdf
- USB Type-C plug méret: USB-IF USB Type-C Cable & Connector Spec R2.0 (2019), usb.org
- ESP32-C3 SuperMini: https://www.espboards.dev/esp32/esp32-c3-super-mini/
- TP4056 USB-C: https://grobotronics.com/lithium-battery-charger-and-protection-module-1a-usb-c-tp4056.html
- 18650 holder: https://www.sparkfun.com/battery-holder-1x18650-wire-leads.html

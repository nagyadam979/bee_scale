# wireless_scaling — parametrikus, vízálló ház (build123d)

Kültéri, elemes IoT szenzoregység (méhkaptár-mérleg node) 3D-nyomtatható háza.
Anyag: **ASA** (UV-álló). Védelmi cél: ~**IP65** (tömítőhornyok + PG7 tömszelencék).

## Alkatrészek
- **`housing`** — falak + tömör felső plató (merevítő bordákkal + mérőcella-furatképpel),
  belső válaszfal a 18650-rekeszhez, PG7 tömszelence-furatok, akku-ajtó nyílás +
  betét-bossok, alsó-zárás körbefutó tömítőhoronnyal és sarok-bossokkal.
- **`bottom_lid`** — alsó fedél = egyben a PCB-tartó alaplap (felfelé álló standoffok).
- **`battery_door`** — oldalsó akku-ajtó (laposan nyomtatható).

A doboz **alul nyitott**; a teteje tömör, merev plató, ide kerül FELÜLRE a single-point
mérőcella (HBS-SCL-50, 50 kg). Minden méret az `enclosure.py` tetején, kommentált változókban.

> ⚠️ **PLACEHOLDER:** a mérőcella furatképe (`cell_hole_spacing_x/y`, `cell_hole_pattern`)
> és a PCB-standoff pozíciók szándékosan ideiglenes értéken vannak — az adatlap / panelek
> alapján írd át őket.

## Környezet
```bash
cd wireless_scaling
python3.11 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Élő nézet VS Code-ban (ocp-vscode)
1. Telepítsd a **„OCP CAD Viewer"** VS Code extensiont (szerző: Bernhard Walter).
2. Parancspaletta (`Cmd+Shift+P`) → **„OCP CAD Viewer: Open viewer"** — ez megnyitja a
   3D viewer panelt és elindítja a szervert a **3939** porton. (Ha kéri, válaszd a `.venv`
   Python interpretert.)
3. Élő frissítés mentésre — futtasd egy terminálban:
   ```bash
   source .venv/bin/activate
   python watch.py
   ```
   Mostantól az `enclosure.py` minden mentése újraépíti a modellt és frissíti a viewert.
   (Alternatíva: az extension „run" gombjával is futtathatod a fájlt.)

## Export
Az `enclosure.py` minden futáskor az `export/` mappába írja (nyomtatáshoz + CAD-hez):
`housing`, `bottom_lid`, `battery_door` → `.stl` **és** `.step`, plusz `assembly.step`.

A kitöltés %, rétegmagasság, support a **slicer** dolga — a modell ettől független.

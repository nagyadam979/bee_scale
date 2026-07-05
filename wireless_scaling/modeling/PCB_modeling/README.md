# PCB_modeling — KiCad-tervezés az IPC API-n (kipy) keresztül

Útmutató ágenseknek: hogyan tervezünk/szerkesztünk PCB-t **programozottan**, egy
futó KiCad-példányban a Python IPC API-val. A leírtak **élesben validálva** vannak
(KiCad 10.0.4 + `kicad-python` 0.7.1, macOS). A működő eszközök a
`wireless_scaling/tools/kicad_ipc/` alatt vannak.

> **Aranyszabály:** az API hívásneveit/szignatúráit **ne memóriából** használd —
> `inspect`-tel nézd meg a telepített `kipy`-ben, és a változást a board-ról
> **visszaolvasva** igazold. A `create_items` sikeres visszatérése önmagában NEM
> bizonyíték (lásd a réteg-buktatót lentebb).

---

## 0. Környezet

- **venv:** `wireless_scaling/.venv` (Python 3.11). Innen futtass mindent.
- **Csomag:** `kicad-python` **0.7.1** — importnév: **`kipy`**. (NEM a régi, elavult
  0.0.2.) Telepítés: `pip install kicad-python`.
- **KiCad:** 10.0.4 (API-verzió 10.0.1). Minimum 9.0+.
- **Egységek:** a KiCad belül **nanométerben** dolgozik. Mindig `from_mm()` /
  `Vector2.from_xy_mm()` konverzióval adj meg méretet — sose nyers integert.

---

## 1. A csatlakozás előfeltételei (a leggyakoribb hibaforrás)

Sorrendben ellenőrizd, ha `Connection refused` vagy `no handler` jön:

1. **Fut a KiCad GUI.**
2. **IPC szerver bekapcsolva:** `KiCad → Settings…` (macOS: `Cmd + ,`) → bal oldalt
   **Plugins** → **`Enable KiCad API`** pipa → `OK`. A bekapcsolás után **teljes
   újraindítás** (`Cmd + Q`, nem csak ablakbezárás) — a szerver csak induláskor jön fel.
   - Ez a **program-beállítás**, NEM a „Plugin and Content Manager" (az a bővítménybolt).
3. **Socket:** a `kipy` alapból ide csatlakozik: `/tmp/kicad/api.sock`. Ha bekapcsolt
   szerverrel fut a KiCad, ennek léteznie kell (`srwxr-xr-x … api.sock`). Nincs
   szükség `KICAD_API_SOCKET` env-változóra.
4. **⚠️ NYITOTT SZERKESZTŐ KELL:** a dokumentum-parancsok (`get_board`, ami belül a
   `GetOpenDocuments`-et hívja) **csak akkor élnek, ha egy PCB Editor nyitva van egy
   board-dal**. A Project Manager főablak önmagában NEM elég — ilyenkor jön a
   `no handler available for request of type kiapi.common.commands.GetOpenDocuments`.
   A `get_version()`/`ping()` viszont a menedzser-szinten is megy, ezért félrevezető:
   **csatlakozni tudsz, szerkeszteni nem**, amíg nincs nyitott board.

Gyors diagnózis parancssorból:
```bash
ls -la /tmp/kicad/            # van-e api.sock?
lsof -p "$(pgrep -f 'KiCad.app/Contents/MacOS/kicad')" -a -U   # nyit-e socketet a KiCad?
```

---

## 2. API-alapok

```python
from kipy import KiCad
k = KiCad(client_name="bee_scale-<feladat>")   # egyedi, beszédes név (lásd commit-buktató)
k.ping(); k.get_version(); k.get_api_version()
board = k.get_board()                            # a nyitott board

board.get_footprints(); board.get_pads(); board.get_shapes()
board.get_copper_layer_count()
board.create_items(items)     # 1 elem VAGY iterable; visszaad List[...]
board.remove_items(items)     # törlés
board.save(); board.save_as(path)
```

**Commit (undo-csoportosítás, opcionális):**
```python
c = board.begin_commit()
board.create_items(...)
board.push_commit(c, "üzenet")   # vagy board.drop_commit(c)
```
Commit **nélkül** minden módosítás azonnal érvényre jut (több undo-lépésként). Kezdéshez
a commit **elhagyása a legegyszerűbb**.

---

## 3. Élesben tapasztalt buktatók

| Tünet | Ok | Megoldás |
|---|---|---|
| `Connection refused` | nem fut / nincs bekapcsolva az IPC szerver | 1. szakasz |
| `no handler … GetOpenDocuments` | nincs nyitott PCB Editor | nyiss board-ot a PCB Editorban |
| `create_items` OK, de a board **üres** | a `FootprintInstance.layer` nincs beállítva | **kötelező** `fp.layer = BoardLayer.BL_F_Cu` — enélkül a KiCad csendben eldobja |
| `the client … already has a commit in progress` | előző futás `begin_commit` után elhalt `push`/`drop` nélkül | friss/egyedi `client_name`, vagy commit elhagyása; a lógó commit KiCad-újraindítással tűnik el |
| `ConnectionError` nem záródik el a `try`-ban | `kipy.errors.ConnectionError` **árnyékolja** a beépítettet | `from kipy.errors import ApiError, ConnectionError as KiCadConnectionError` és azt kapd el |

A legalattomosabb a **réteg-buktató**: a `create_items` visszaad egy elemet (látszólag
siker), de réteg nélkül a footprint nem perzisztál. Ezért mindig **olvasd vissza**:
`len(board.get_footprints())`, `len(board.get_pads())`.

---

## 4. Recept — átmenő furatos (PTH) pad + footprint + kontúr

A teljes, paraméterezett, idempotens implementáció:
**`wireless_scaling/tools/kicad_ipc/make_perfboard.py`** — ezt másold/általánosítsd.
A mag:

```python
from kipy.board_types import (FootprintInstance, Pad, PadType, PadStackType,
    PadStackShape, DrillShape, BoardLayer, BoardSegment)
from kipy.geometry import Vector2

# --- egy kör alakú PTH pad (footprint-relatív pozíció) ---
pad = Pad()
pad.number = "1"
pad.pad_type = PadType.PT_PTH
pad.position = Vector2.from_xy_mm(x_mm, y_mm)
ps = pad.padstack
ps.type = PadStackType.PST_NORMAL              # felveszi a BL_F_Cu copper layer-t
fcu = ps.copper_layer(BoardLayer.BL_F_Cu)
fcu.shape = PadStackShape.PSS_CIRCLE
fcu.size = Vector2.from_xy_mm(1.8, 1.8)        # pad-átmérő
ps.drill.shape = DrillShape.DS_CIRCLE
ps.drill.diameter = Vector2.from_xy_mm(1.0, 1.0)   # furat-átmérő
ps.drill.start_layer = BoardLayer.BL_F_Cu
ps.drill.end_layer = BoardLayer.BL_B_Cu

# --- footprint (a pad-ek konténere) ---
fp = FootprintInstance()
fp.reference_field.text.value = "REF1"
fp.value_field.text.value = "MyFootprint"
fp.layer = BoardLayer.BL_F_Cu                  # ⚠️ KÖTELEZŐ
fp.position = Vector2.from_xy_mm(0.0, 0.0)     # a pad-koordináták ehhez képest relatívak
fp.definition.add_item(pad)

# --- tábla-kontúr: szakasz az Edge.Cuts rétegen ---
seg = BoardSegment()
seg.start = Vector2.from_xy_mm(0, 0)
seg.end   = Vector2.from_xy_mm(60, 0)
seg.layer = BoardLayer.BL_Edge_Cuts

board.create_items(fp)
board.create_items([seg, ...])
```

Hasznos enum-ok / rétegek: `PadType.PT_PTH|PT_SMD|PT_NPTH`,
`PadStackShape.PSS_CIRCLE|PSS_RECTANGLE|PSS_ROUNDRECT|…`,
`BoardLayer.BL_F_Cu|BL_B_Cu|BL_Edge_Cuts|BL_F_SilkS|…` (`list(BoardLayer.keys())`).

---

## 5. Munkamódszer (ajánlott hurok)

1. **Ellenőrizd a kapcsolatot:** `tools/kicad_ipc/ipc_connect_test.py` — ha ez zöld,
   a socket és a szerver rendben.
2. **Nyiss board-ot** a PCB Editorban (különben 1.4 szerinti hiba).
3. **Ismeretlen API?** `python3 -c "import inspect, kipy.board_types as bt;
   print(inspect.signature(bt.Pad.__init__))"` — a szignatúrát/settert nézd meg, ne tippelj.
4. **Kis próbával validálj** (pl. 2×2 rács), olvasd vissza a board-ot, és csak utána
   futtasd a teljes generálást.
5. **Idempotencia:** a generátor induláskor törölje a saját korábbi elemeit
   (`remove_items(board.get_footprints() + board.get_shapes())`), hogy újrafuttatható legyen.
6. **Mentés:** a `create_items` azonnal látszik a GUI-ban, de fájlba a
   `board.save()` vagy a felhasználó `Cmd+S`-e ír.

---

## 6. KiCad projektfájlok kezelése (fontos!)

Egy KiCad projekt = `<név>.kicad_pro` + `<név>.kicad_pcb` (+ `.kicad_sch`), **azonos
bázisnévvel, egy mappában**. A társítás bázisnév-egyezésre épül, nem abszolút útra.

- ✅ **Teljes projekt-mappát egészben mozgatni/átnevezni** → rendben (zárt projektnél).
- ✅ **Szülő-mappát átnevezni** (a projekt saját mappája alatta marad) → rendben.
- ❌ **A projektfájlokat külön kézzel átnevezni/szétszórni** (bázisnév megbontása) →
  a KiCad nem fogadja el. Bázisnév-váltás **csak** `File → Save As`-szal, vagy fájlszintű
  „Save As"-szal: a 3 fájlt **együtt** átmásolni új bázisnévre egy új mappába, **zárt**
  KiCad mellett (`lsof` a fájlokra, hogy semmi ne tartsa nyitva).
- A `~<név>.kicad_pcb.lck` a szerkesztő lock-ja; bezárás után eltűnik. Árva lock
  törölhető, ha `lsof` szerint semmi nem használja a fájlt.

**Git:**
- A `.kicad_pro/.kicad_pcb/.kicad_sch` szöveges, git-barát, **kicsi → verziózzuk**.
- A generált CAD-export (`modeling/enclosure/export/`, STL/STEP/…) **nagy + reprodukálható
  → `.gitignore`** (lásd `wireless_scaling/.gitignore`). Commit előtt mindig ellenőrizd,
  hogy nem került-e be >~300 KB blob.

---

## 7. Ismert korlát

`kicad-python` 0.7.1 + KiCad 10 párosításon a `get_open_documents` **`no handler`** hibát
ad — a `get_board()` viszont működik nyitott szerkesztővel, így ezt megkerüli. Ha a jövőben
több dokumentum-szintű parancs is elhasal, érdemes lehet a KiCad 10-hez igazított
`kicad-python` verziót felrakni (git-forrásból, ha a PyPI-n még a 0.7.1 a legfrissebb).

---

## Eszközök (repo)

- `wireless_scaling/tools/kicad_ipc/ipc_connect_test.py` — csatlakozás- és alap-lekérdezés-teszt.
- `wireless_scaling/tools/kicad_ipc/make_perfboard.py` — paraméterezett, idempotens
  perfboard-generátor (`--w --h --pitch --pad --drill --margin`); a §4 recept referencia-implementációja.

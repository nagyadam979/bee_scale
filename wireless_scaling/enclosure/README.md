# enclosure/ — 3D nyomtatott ház (build123d)

A wireless node **3D nyomtatott háza**, build123d-vel parametrikusan modellezve. Ez a mappa a teljes geometriai munka belépője. A modul általános munkamódszere: `../CLAUDE.md`. Az aktuális tervezési tények/döntések: `../MEMORY.md`.

---

## Mappa-tartalom

- **`enclosure.py`** — fő modell. **Csak a tetején lévő, kommentált paramétereket** hangold.
- **`validate.py`** / **`preview.py`** / **`_zsection.py`** — eszközök; **EGYÜTT tartandók** az `enclosure.py`-nal, mert `import enclosure`.
- **`datasheets/`** — alkatrész-adatlapok.
- **`export/`** — generált STL/STEP/PNG (gitignored).
- **`requirements.txt`** — Python függőségek.

---

## Napi parancsok

A `.venv` a **wireless_scaling gyökérben** van, kész — NE telepíts újra.

```bash
source ../.venv/bin/activate     # a .venv a szülő (wireless_scaling) gyökérben van
python validate.py        # 2 part (housing, bottom_lid): 1-1 összefüggő solid? + bbox épelméjű?
python preview.py [X..]   # ⭐ GYORS tömör METSZET PNG (export/preview.png), STL nélkül (~12s)
python enclosure.py       # STL+STEP export az export/-ba + ocp-vscode VIEWER PUSH (3939)
```

**Setup nulláról (új gép):** a wireless_scaling gyökérben `python3.11 -m venv .venv && source .venv/bin/activate` (⚠️ 3.11 — a 3.14-hez nincs ocp wheel), majd `pip install -r enclosure/requirements.txt`. Élő nézethez VS Code-ban: **„OCP CAD Viewer"** extension (Bernhard Walter) → `Cmd+Shift+P` → „OCP CAD Viewer: Open viewer" (3939 port; válaszd a `.venv` interpretert).

---

## A geometriai munka KÖTELEZŐ hurka

1. **SZERKESZTÉS** — csak az `enclosure.py` tetején lévő kommentált paraméterek.
2. **VALIDÁLÁS** — `python validate.py` → `housing` + `bottom_lid` **1-1 összefüggő solid** + bbox épelméjű. Anyag-eltávolításnál térfogat-diffel számszerűsíts.
3. **VIZUÁLIS ELLENŐRZÉS ⭐** — `python preview.py [X-síkok]` → **Read** `export/preview.png`. Tömör keresztmetszet (NEM féltér-vetítés — az félrevezet). **EZT kell megnézni, MIELŐTT bármit késznek mondunk** — különben vak iteráció.
4. **ÉLŐ NÉZET (user)** — `python enclosure.py` → push az ocp-vscode-ba (3939). (`validate.py`/`preview.py` NEM pushol.)
5. **EXPORT** — az `enclosure.py` STL+STEP-et is ír az `export/`-ba.
6. **COMMIT** — részletes üzenet (lásd `../CLAUDE.md` Commit-fegyelem).

**Tervezési elvek** (a teljes lista + alkatrész-tények: `../MEMORY.md`): kontakt felületek élesek, minden más kerekíthető (`print_round_enable`); OCCT fillet csak szűk, célzott él-csoportokra; 2D-profil fillet az extrude előtt robusztusabb; statikát/merevítést a USER tervez.

---

## Token-takarékos DELEGÁLÁS (orchestrátornak, KÖTELEZŐ a geom. munkára)

A geom. iteráció (szerkesztés → `validate.py` → `preview.py` → **Read PNG** → újra) sok tokent éget, mert minden kör PNG-t + dumpot hagy a beszélgetésben. Ezért **delegáld az `enclosure-builder` al-ágensnek** (`../agents/enclosure-builder.md`, friss/izolált kontextus).

- **Az orchestrátornál marad:** a *mit* — a tervezési döntés a felhasználóval (forma, radius/Z/vastagság). Ezt NEM delegáljuk.
- **Az ágens kap:** egy MÁR ELDÖNTÖTT, körülhatárolt változást — **cél, hangolandó paraméterek, korlátok, kész-kritérium**.
- **Az ágens visszaad:** tömör összefoglaló (paraméter régi→új, validate-eredmény, térfogat-diff) + a **végső** `export/preview.png` útja. A sok köztes PNG/iteráció NÁLA marad.
- **Ellenőrzés:** az orchestrátor a végén **egyszer** Read-eli a PNG-t és jóváhagyja. Ha az ágens nyitott döntésbe ütközik, visszakérdez → az orchestrátor a felhasználóval dönt.
- Triviális, egyértelmű paraméter-tweak is mehet az ágenshez. Közvetlenül csak akkor csináld, ha gyorsabb, mint a feladatot megfogalmazni.

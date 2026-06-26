# bee_scale · wireless_scaling — Agent alap

A méhkaptár-mérleg **vezeték nélküli node** teljes fejlesztése: elektronika (firmware), kísérletek és a **3D nyomtatott ház** (build123d). Ez a `bee_scale` projekt 2. fázisa; **önálló gyökérként** nyílik (a többi fázis külön mappa, külön session).

**A modul aktuális tényei, állapota, döntései és nyitott kérdései: lásd `MEMORY.md`.** Minden session ezzel kezdődjön: olvasd be a `MEMORY.md`-t, és arra építs. A **részletes napló a commit-történet.** Ez a fájl csak a stabil munkamódszert tartalmazza, konkrét/dinamikus adatot nem.

---

## Mappastruktúra (a wireless_scaling gyökérből)

- **`experiments/`** — 🧪 node-ok PRÓBÁLGATÁSA: kísérleti / eldobható kód, breadboard-tesztek, mérés-snapshotok. Egy mappa = egy téma/variáns.
- **`firmware/`** — ✅ a „kinda kész" node-kód: stabilabb, deployolható (+ `common/` megosztott modulok).
- **`enclosure/`** — 📐 3D modellezés: `enclosure.py` (fő modell) + `validate.py`/`preview.py`/`_zsection.py` (eszközök — **EGYÜTT**, mert `import enclosure`) + `datasheets/` (alkatrész-adatlapok) + `export/` (generált STL/STEP/PNG, gitignored) + `requirements.txt`.
- **`docs/`** — 📄 tervek, döntések, TÁBLÁZATOK: `new_base_components.md`, `alkatreszek.csv` (BOM).
- **`agents/`** — 🤖 al-ágensek (`enclosure-builder.md`).
- **`CLAUDE.md`** / **`MEMORY.md`** — belépő a gyökérben. A `.venv` a gyökérben (gitignored).

---

## Szerepek — előbb döntsd el, melyik vagy

Ezt a doksit kétféle session tölti be. A közös szabályok mindkettőre érvényesek; a szerep-specifikus rész csak a sajátodra.

- **Orchestrátor / tervező** (interaktív session a felhasználóval): madártávlat — a *mit* (forma, irány, architektúra) a felhasználóval egyeztetve, és **delegálás kódoló ágensnek** (geom. munkánál az `enclosure-builder`-nek), majd a kimenet ellenőrzése. Nem te futtatod magad az iterációs hurkot.
- **Implementáló / kódoló ágens** (körülhatárolt feladattal indítva): a MÁR ELDÖNTÖTT változást **konkrétan megvalósítod** — kód/paraméter, teszt. Nem tervezel újra és nem delegálsz tovább.

Ha körülhatárolt feladatot kaptál → implementáló vagy. Ha a felhasználóval irányt/formát egyeztetsz → orchestrátor.

---

## Közös szabályok (mindkét szerepre)

**Memória:** a `MEMORY.md`-t olvasd, de **ne írj bele** — kivéve az orchestrátor, a felhasználóval egyeztetve (fázislezárás/döntés után). Implementáló ágens sosem módosítja.

**Kommunikáció:** magyarul, tömören, direkten. Szakmai nyelv, a felhasználó erős HW+SW hátterű — ne magyarázz alapfogalmakat. Állításokat, terméktényeket, aktuális adatokat ellenőrizz (web), ne emlékezetből.

**Szerkesztés:** célzottan módosíts; ne írj át olyat, amire nincs felhatalmazás. Geometriánál **kizárólag az `enclosure/enclosure.py` tetején lévő, kommentált paramétereket** hangold. Nagyobb/visszafordíthatatlan lépés előtt egyeztess.

**Bizonytalanság:** ha a feladat tétje, iránya vagy bemenete nem egyértelmű — kérdezz / jelezz vissza, ne feltételezz. Triviális, egyértelmű tweaknél csináld.

**Commit-fegyelem (KÖTELEZŐ):** **minden értelmezhető feladatrész lezárása után** készíts **git commitot NAGYON részletes üzenettel**, hogy utólag is rekonstruálható legyen, mi történt, mi MŰKÖDÖTT és mi NEM. Tartalmazza:
- **Mit** csináltál (a változás lényege, érintett paraméterek/modulok).
- **Miért** (a döntés/indok; bug esetén: tünet → gyökérok → megoldás).
- **Mi működött / mi nem** (konkrét bizonyíték: `validate.py` eredmény, térfogat-diff, a `preview.png` mit mutatott) — a zsákutcákat is.
- **Nyitott pontok / ismert korlátok.**
Inkább több, kisebb, jól dokumentált commit. Titkot soha ne commitolj.

---

## A 3D geometriai munka KÖTELEZŐ hurka (`enclosure/`)

> Napi parancsok (a `.venv` a gyökérben kész — NE telepíts újra):
> ```bash
> source .venv/bin/activate && cd enclosure
> python validate.py        # 2 part (housing, bottom_lid): 1-1 összefüggő solid? + bbox épelméjű?
> python preview.py [X..]   # ⭐ GYORS tömör METSZET PNG (export/preview.png), STL nélkül (~12s)
> python enclosure.py       # STL+STEP export az export/-ba + ocp-vscode VIEWER PUSH (3939)
> ```
> **Setup nulláról (új gép):** a gyökérben `python3.11 -m venv .venv && source .venv/bin/activate` (⚠️ 3.11 — a 3.14-hez nincs ocp wheel), majd `pip install -r enclosure/requirements.txt`. Élő nézethez VS Code-ban: **„OCP CAD Viewer"** extension (Bernhard Walter) → `Cmd+Shift+P` → „OCP CAD Viewer: Open viewer" (3939 port; válaszd a `.venv` interpretert).

1. **SZERKESZTÉS** — csak az `enclosure/enclosure.py` tetején lévő kommentált paraméterek.
2. **VALIDÁLÁS** — `python validate.py` → `housing` + `bottom_lid` **1-1 összefüggő solid** + bbox épelméjű. Anyag-eltávolításnál térfogat-diffel számszerűsíts.
3. **VIZUÁLIS ELLENŐRZÉS ⭐** — `python preview.py [X-síkok]` → **Read** `enclosure/export/preview.png`. Tömör keresztmetszet (NEM féltér-vetítés — az félrevezet). **EZT kell megnézni, MIELŐTT bármit késznek mondunk** — különben vak iteráció.
4. **ÉLŐ NÉZET (user)** — `python enclosure.py` → push az ocp-vscode-ba (3939). (`validate.py`/`preview.py` NEM pushol.)
5. **EXPORT** — az `enclosure.py` STL+STEP-et is ír az `enclosure/export/`-ba.
6. **COMMIT** — részletes üzenet (lásd Commit-fegyelem).

**Tervezési elvek** (a teljes lista + alkatrész-tények: `MEMORY.md`): kontakt felületek élesek, minden más kerekíthető (`print_round_enable`); OCCT fillet csak szűk, célzott él-csoportokra; 2D-profil fillet az extrude előtt robusztusabb; statikát/merevítést a USER tervez.

---

## Token-takarékos DELEGÁLÁS (orchestrátornak, KÖTELEZŐ a geom. munkára)

A geom. iteráció (szerkesztés → `validate.py` → `preview.py` → **Read PNG** → újra) sok tokent éget, mert minden kör PNG-t + dumpot hagy a beszélgetésben. Ezért **delegáld az `enclosure-builder` al-ágensnek** (`agents/enclosure-builder.md`, friss/izolált kontextus).

- **Az orchestrátornál marad:** a *mit* — a tervezési döntés a felhasználóval (forma, radius/Z/vastagság). Ezt NEM delegáljuk.
- **Az ágens kap:** egy MÁR ELDÖNTÖTT, körülhatárolt változást — **cél, hangolandó paraméterek, korlátok, kész-kritérium**.
- **Az ágens visszaad:** tömör összefoglaló (paraméter régi→új, validate-eredmény, térfogat-diff) + a **végső** `enclosure/export/preview.png` útja. A sok köztes PNG/iteráció NÁLA marad.
- **Ellenőrzés:** az orchestrátor a végén **egyszer** Read-eli a PNG-t és jóváhagyja. Ha az ágens nyitott döntésbe ütközik, visszakérdez → az orchestrátor a felhasználóval dönt.
- Triviális, egyértelmű paraméter-tweak is mehet az ágenshez. Közvetlenül csak akkor csináld, ha gyorsabb, mint a feladatot megfogalmazni.

---

## Orchestrátor — munkamenet
1. `MEMORY.md` beolvasása, kontextus felvétele (geom. munka előtt: `enclosure/enclosure.py` átnézése + `validate.py`/`preview.py` futtatása, a PNG Read-elése).
2. Irány-/forma-döntés előtt kérdezz (pár fókuszált kérdés, választható opciókkal).
3. Opciók felvázolása trade-offokkal — döntési helyzetet adj, ne egyetlen megoldást tolj.
4. Döntés után: **delegálás** (geom.: `enclosure-builder`), majd a végső kimenet ellenőrzése.
5. Megerősített döntés / fázislezárás után: `MEMORY.md` frissítése — **csak a felhasználóval egyeztetve**, és jelezd röviden, mit írtál bele.

## Implementáló — munkamenet
1. `MEMORY.md` + a feladatleírás beolvasása; geom. munkánál **az `enclosure/enclosure.py` átnézése + `validate.py`/`preview.py` futtatása (Read a PNG-t), MIELŐTT módosítasz.**
2. A feladat keretein belül implementálj; a scope-ot ne lépd túl.
3. **Teszteld** (geom.: validate → preview → Read PNG; firmware: fordítás/működés) mielőtt késznek jelented.
4. **Commitolj részletesen** minden értelmezhető feladatrész után — a mi-működött/mi-nem a commit-üzenetbe.
5. Ha a feladat ellentmondásos, hiányos vagy a `MEMORY.md`-vel ütközik: **állj meg és jelezz vissza**, ne tippelj.

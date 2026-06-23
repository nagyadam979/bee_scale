# wireless_scaling — node ház: munkafolyamat + állapot

> Parametrikus, ~IP65, ASA-nyomtatható ház a méhkaptár-mérleg node-hoz. **build123d + ocp-vscode.**
> Ez a doksi az **EGYETLEN belépő**: környezet + munkafolyamat + a friss állapot. A **commit-történet
> (dev_01) a részletes napló** — mit/miért/mi-működött/mi-nem.
> Utolsó frissítés: 2026-06-23.
>
> **STÁTUSZ:** a geometria stabil és validált (2 part, 1-1 solid). **Jelenleg senki nem dolgozik a
> modell geometriáján** — a következő geometriai munka KÜLÖN döntéssel indul (akkor dől el, ki csinálja).
> Most a fókusz: a munkafolyamat és az eszközök karbantartása.

---

## 0. KÖRNYEZET

**Napi parancsok** (a `.venv` ezen a gépen már kész — NE telepíts újra):
```bash
cd /Users/adamnagy/Documents/bee_scale/wireless_scaling && source .venv/bin/activate
python validate.py      # 2 part (housing, bottom_lid): 1-1 összefüggő solid? + bbox épelméjű?
python preview.py [X..] # ⭐ GYORS tömör METSZET+nézet PNG (export/preview.png), STL nélkül (~12s)
python enclosure.py     # STL+STEP export az export/-ba + ocp-vscode VIEWER PUSH (port 3939)
```
Minden méret az `enclosure.py` tetején, kommentált változó.

**Setup nulláról** (új gép):
```bash
python3.11 -m venv .venv && source .venv/bin/activate   # ⚠️ 3.11 — a 3.14-hez nincs ocp wheel
pip install -r requirements.txt
```
Élő 3D nézethez VS Code-ban: telepítsd az **„OCP CAD Viewer"** extensiont (Bernhard Walter) →
`Cmd+Shift+P` → **„OCP CAD Viewer: Open viewer"** (a **3939** porton; ha kéri, válaszd a `.venv`
interpretert). Ezután az `enclosure.py` futtatása ide pushol.

---

## 1. MUNKAFOLYAMAT (a kötelező hurok — geometriai munkánál)

1. **SZERKESZTÉS** — kizárólag az `enclosure.py` tetején lévő, kommentált paramétereket hangold.
   *(A geometriai munka most szünetel; ez a lépés a következő, dedikált feladatban él.)*
2. **VALIDÁLÁS** — `python validate.py` → `housing` + `bottom_lid` **1-1 összefüggő solid** + bbox
   épelméjű. Anyag-eltávolításnál térfogat-diffel számszerűsíts.
3. **VIZUÁLIS ELLENŐRZÉS ⭐** — `python preview.py [X-síkok]` → **Read** `export/preview.png`.
   Tömör keresztmetszet (build123d sík-metszet) → a lépcsők/ívek/falvastagság PONTOSAN leolvashatók.
   Tetszőleges metsző-sík: `python preview.py 0 6 11 -7.9`. **EZT kell megnézni, MIELŐTT bármit
   késznek mondunk** — különben vak iteráció (lásd 2/#1).
4. **ÉLŐ NÉZET (user)** — `python enclosure.py` → push az ocp-vscode-ba (3939). (`validate.py` és
   `preview.py` NEM pushol.)
5. **EXPORT** — az `enclosure.py` egyúttal STL+STEP-et ír az `export/`-ba (nyomtatás + CAD).
6. **COMMIT** — részletes üzenet: **mit / miért / mi-működött-mi-nem / nyitott**. Branch: `dev_01`.

---

## 1b. DELEGÁLÁS — token-takarékos geom. munka (KÖTELEZŐ az orchestrátornak)

A geometriai iteráció (szerkesztés → `validate.py` → `preview.py` → **Read PNG** → újra) sok tokent
éget, mert minden kör PNG-t + validate-dumpot + diffet hagy a beszélgetésben. **Tanulság: a köztes
hurok az orchestrátor kontextusát terhelte.** Megoldás: **delegálj az `enclosure-builder` al-ágensnek**
(`.claude/agents/enclosure-builder.md`, friss/izolált kontextus).

- **Mi marad az orchestrátornál:** a *mit* — a tervezési döntés a felhasználóval (hogyan nézzen ki,
  milyen radius/Z/vastagság). Ezt NEM delegáljuk.
- **Mit kap az ágens:** egy MÁR ELDÖNTÖTT, körülhatárolt változás — **cél, hangolandó paraméterek,
  korlátok, kész-kritérium**. (Pl. „R8 folytonos tölcsér a felfekvéstől az oszlopig, kontakt-élek
  élesen, validálj + metszet".)
- **Mit ad vissza:** tömör összefoglaló (paraméter régi→új, validate-eredmény, térfogat-diff) + a
  **végső** `export/preview.png` útja. A sok köztes PNG/iteráció NÁLA marad → a fő beszélgetés tiszta.
- **Ellenőrzés:** az orchestrátor a végén **egyszer** Read-eli a PNG-t és jóváhagyja — egy képread N
  helyett. Ha az ágens nyitott döntésbe ütközik, visszakérdez, az orchestrátor a felhasználóval dönt.
- **Triviális, egyértelmű paraméter-tweak is mehet az ágenshez** (a validate/preview hurok ne minket
  terheljen). Csak akkor csináld közvetlenül, ha gyorsabb, mint a feladatot megfogalmazni.

## 2. TERVEZÉSI ELVEK / TANULSÁGOK (nem nyilvánvalók)

1. **NE dolgozz vakon — `preview.py` az agent SZEME.** Minden geom. változás után Read-eld az
   `export/preview.png`-t (tömör metszet, NEM féltér-vetítés — az zsúfolt és FÉLREVEZET). Tanulság:
   egy 1.5 mm fillet „láthatatlan" maradt vak vetítésen — a tömör metszet rögtön megmutatta, hogy érdemtelen.
2. **OCCT fillet/chamfer összetett testen:** a „minden élre egyszerre" **elhasal** ("try a smaller length").
   - Szűk, célzott él-csoportokat válassz (hosszú/külső élek; lyuk- és apró éleket KIZÁRD).
   - **`round_edges()` hibatűrő wrapper** (try/except) — egy csoport bukása ne döntse el az exportot.
   - **2D-profil fillet az extrude ELŐTT sokkal robusztusabb**, mint 3D-élt vágni (így készült az
     Y-oszlop profil-lekerekítése: `fillet(polygon.vertices(), r)` majd `extrude`).
3. **Kontakt felületek ÉLESEK maradnak**, minden más kerekíthető (nyomtatás-szilárdság). Kontakt =
   USB-nyílások, furatok belseje, cella-ágy (pocket/dent/karima-felfekvés), ház↔fedél felület +
   gasket-horony, fül-alja (z=0). Mester kapcsoló: `print_round_enable`.
4. **Kézi ütközés-ellenőrzés** (alkatrész vs. ház, ha kell): `(e.build_housing() & maquette).volume`
   (0 = szabad). A behelyezési utat is: a makett footprintjét z=0-ig lehúzva.
5. **Bizonytalan tervezési/anyag-eltávolítási döntésnél KÉRDEZZ** (a user erős HW+SW háttér, magyarul,
   tömören, ő dönt a hangolható értékekről — pl. radiusok, V-csúcs Z, X-vastagság). Egyértelműnél csináld.
6. **Commit-fegyelem:** minden lépés után részletes commit (lásd 1/#6). Inkább több kicsi, mint egy nagy.

---

## 3. KOORDINÁTÁK + BEFOGLALÓ

X = rövid él (`inner_x=60`), Y = hosszú él (`inner_y=78`, ide a 18650 holder ∥ Y), Z fel. Alj z=0,
XY-origóra centrálva. **Külső bbox: 72.5 × 86 × 28.5 mm** (a +X fülek +4.5-öt nyújtanak). 2 nyomtatott
part: `housing`, `bottom_lid` (akku-ajtó törölve).

## 4. JELENLEGI FELÉPÍTÉS (mind validálva, 1-1 solid)

- **Falak:** `wall = long_wall_th = 4` (egyenletes), `top_th=3.5`, `lid_th=1.5`, `inner_z=25`.
- **Tető-platform (cella):** besüllyesztett pocket (34.8) + **karima** (teherhordó talp, a felfekvésen
  kívül kifaragva, csak boss-collarok maradtak) + **dent** (közép-üreg, ide hajlik a cella) + 2× M2.5
  önmetsző cella-fix boss (x=0, y=±21.4). A felfekvő gyűrű/dent/karima átmeneteibe fillet (teherúti ív).
- **Y-OSZLOP** (teherút): álló „Y" a Y-Z síkban, x[−6,6] (nem ér a +X holderhez); profil-sarkok
  lekerekítve; alján pilot + középső fedélcsavar.
- **Fedél-csavarok:** 5 BELSŐ (−X 2 sarok + −X/+Y/−Y közép) félkör (D) collarral; **+X 3 KÜLSŐ FÜL**
  (a seal-vonalon kívül → IP65 ok, a holder helyét felszabadítja), agresszív fül↔fal kónusszal.
- **bottom_lid:** sík lap + **megemelt belső dugó** (perem tömör + diagonális rács; kihagyások az
  oszlopnál + bossoknál).
- **Lekerekítés/kónusz:** külső sarkok, felső perem, fülek, collarok, Y-oszlop, fedél-alj — kontaktok élesek.

## 5. NYITOTT PONTOK

- **Kábel-bevezetés** (cella-kábel, DS18B20): a −Y PG7 furatok törölve → MÓD NYITOTT (tető/fal/perem?).
- **Cella-ágy teherúti ívek:** az 1.5 mm fillet first-pass — a user szerint inkább **folytonos, nagy
  sugarú „tölcsér"** kell a felfekvéstől az oszlopig (érdemi profil-átépítés), + a két függőleges
  lightening-hasadék felülvizsgálata. *(A következő geometriai feladat fő tárgya.)*
- **batt_z0=3** és a 18650-magasság feltételezés → **éles darabon mérendő** (~1.75 mm/vég a holdernél).
- **Belső élek** még élesek (collar-tövek, nyitott-alj fal-élek) — kérésre célzottan kerekíthetők.
- **18650 holder tartója:** a házban nincs fizikai tartó (a user oldja meg); a pozíció-referencia a
  commit-történetben (a törölt `layout.py` STRIP_CX-e, git history).
- Statikát a USER tervezi — merevítést kérés nélkül NE tegyél vissza.

**ELŐSZÖR** (geometriai munka előtt): olvasd az `enclosure.py`-t, futtasd `validate.py` + `preview.py`
(Read a PNG-t!), MIELŐTT módosítasz. Alkatrész-méretek: `new_base_components.md` + `datasheets/` + commit-történet.

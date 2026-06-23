# wireless_scaling — node ház: átadás a következő ágensnek

> Parametrikus, ~IP65, ASA-nyomtatható ház a méhkaptár-mérleg node-hoz. build123d + ocp-vscode.
> A **commit-történet (dev_01) a részletes napló** — mit/miért/mi-működött. Ez a doksi a munkafolyamat
> + a friss állapot. Utolsó frissítés: 2026-06-23.

---

## 0. KÖRNYEZET (kész, NE telepítsd)

```bash
cd /Users/adamnagy/Documents/bee_scale/wireless_scaling && source .venv/bin/activate
python validate.py    # 2 part (housing, bottom_lid): 1-1 összefüggő solid? + bbox
python layout.py      # ütközés-check + VIEWER PUSH (ház átlátszón + elem-makett), port 3939
python enclosure.py   # STL+STEP export + viewer push
```
Minden méret az `enclosure.py` tetején, kommentált változó. Python 3.11 venv (3.14-hez nincs ocp wheel).

## 1. MUNKAFOLYAMAT-TANULSÁGOK (FONTOS — ezek nem nyilvánvalók)

1. **A viewer NEM frissül magától.** Szerkesztés után **futtass `python layout.py`-t** (vagy
   `enclosure.py`-t) → az pushol az ocp-vscode-ba (3939). A user többször "nem látott változást", mert
   csak `validate.py`/inline szkript futott (azok nem pusholnak). **Minden látványbeli változás után pushölj.**
2. **A `layout.py` ütközés-check CSAK a maketteket** (`COMPONENTS`) veti össze egymással — a `housing`-ot
   (benne oszlop, bossok, fülek) **NEM**. Holder↔oszlop/boss ütközést kézzel ellenőrizz:
   `(e.build_housing() & maquette).volume` (0 = szabad). A behelyezési utat is: a makett footprintjét z=0-ig lehúzva.
3. **OCCT fillet/chamfer összetett testen:** a "minden élre egyszerre" **elhasal** ("try a smaller length").
   - Szűk, célzott él-csoportokat válassz (hosszú/külső élek; lyuk- és apró éleket KIZÁRD).
   - **`round_edges()` hibatűrő wrapper** (try/except) — egy csoport bukása ne döntse el az exportot.
   - **2D-profil fillet az extrude ELŐTT sokkal robusztusabb**, mint 3D-élt vágni (így készült az Y-oszlop
     profil-lekerekítése: `fillet(polygon.vertices(), r)` majd `extrude`).
4. **Kontakt felületek ÉLESEK maradnak**, minden más kerekíthető (nyomtatás-szilárdság). Kontakt =
   USB-nyílások, furatok belseje, cella-ágy (pocket/dent/karima-felfekvés), ház↔fedél felület + gasket-horony,
   fül-alja (z=0). Mester kapcsoló: `print_round_enable`.
5. **Validálj MINDEN változás után** (`validate.py`: 1-1 solid + bbox épelméjű). Anyag-eltávolításnál
   térfogat-diffel számszerűsíts.
6. **Bizonytalan tervezési/anyag-eltávolítási döntésnél KÉRDEZZ** (a user erős HW+SW háttér, magyarul,
   tömören, ő dönt a hangolható értékekről — pl. V-csúcs Z, X-vastagság, radiusok). Egyértelmű feladatnál csinálj.
7. **Commit-fegyelem:** minden lépés után részletes commit (mit/miért/mi-működött-mi-nem/nyitott). Branch: `dev_01`.

## 2. KOORDINÁTÁK + FRISS BEFOGLALÓ

X = rövid él (`inner_x=60`), Y = hosszú él (`inner_y=78`, ide a 18650 holder ∥ Y), Z fel. Alj z=0, XY-origóra
centrálva. **Külső bbox most: 72.5 × 86 × 28.5 mm** (a régi 68×86×33.5 már NEM áll: a +X fülek +4.5-öt nyújtanak,
a doboz −5 mm-rel alacsonyabb). 2 nyomtatott part: `housing`, `bottom_lid` (akku-ajtó törölve).

## 3. JELENLEGI FELÉPÍTÉS (mind validálva, 1-1 solid)

- **Falak:** `wall=long_wall_th=4` (mindkét fal 4 mm → egyenletes), `top_th=3.5`, `lid_th=1.5`, `inner_z=25`.
- **Tető-platform (cella):** besüllyesztett pocket (34.8) + **karima** (teherhordó talp, a cella-felfekvésen
  KÍVÜL kifaragva, csak boss-collarok maradtak) + **dent** (közép-üreg, ide hajlik a cella) + 2× M2.5 önmetsző
  cella-fix boss (x=0, y=±21.4, tail levágva).
- **Y-OSZLOP** (teherút): álló "Y" a Y-Z síkban, x[−6,6] (nem ér a +X holderhez); profil-sarkok lekerekítve;
  alján pilot + középső fedélcsavar.
- **Fedél-csavarok:** 5 BELSŐ (−X 2 sarok + −X/+Y/−Y közép) félkör (D) collarral; **+X 3 KÜLSŐ FÜL** (lug,
  a seal-vonalon kívül → IP65 ok, a holder helyét felszabadítja), agresszív fül↔fal kónusszal.
- **bottom_lid:** sík lap + **megemelt belső dugó** (perem tömör + diagonális rács, a kavitásba illeszkedik;
  kihagyások az oszlopnál + bossoknál).
- **Lekerekítés/kónusz:** külső sarkok, felső perem, fülek, collarok, Y-oszlop, fedél-alj — kontaktok élesek.

## 4. NYITOTT PONTOK

- **Kábel-bevezetés** (cella-kábel, DS18B20): a −Y PG7 furatok törölve → MÓD NYITOTT (tető/fal/perem?).
- **batt_z0=3** és a 18650-magasság feltételezés → **éles darabon mérendő** (a holder végein ~1.75 mm/vég).
- **Belső élek** még élesek (collar-tövek, nyitott-alj fal-élek) — kérésre célzottan kerekíthetők.
- **18650 holder tartója:** a házban nincs fizikai tartó (a user oldja meg); a `layout.py` STRIP_CX csak referencia.
- Statikát a USER tervezi — merevítést kérés nélkül NE tegyél vissza.

**ELŐSZÖR:** olvasd `enclosure.py` + `layout.py`, futtasd `validate.py` + `layout.py` (viewer!), MIELŐTT módosítasz.
Alkatrész-méretek/források: lásd `README.md` / `new_base_components.md` + a commit-történet.

# bee_scale · wireless_scaling — Agent alap

A méhkaptár-mérleg **vezeték nélküli node** teljes fejlesztése: elektronika/firmware, kísérletek és a 3D nyomtatott ház. Ez a `bee_scale` projekt 2. fázisa; **önálló gyökérként** nyílik (a többi fázis külön mappa, külön session).

**A modul aktuális tényei, hardvere, döntései és nyitott kérdései: lásd `MEMORY.md`.** Minden session ezzel kezdődjön: olvasd be a `MEMORY.md`-t, és arra építs. A részletes napló a commit-történet. Ez a fájl csak a stabil munkamódszert tartalmazza, konkrét/dinamikus adatot nem.

---

## Mappastruktúra (a wireless_scaling gyökérből)

- **`experiments/`** — 🧪 node-ok PRÓBÁLGATÁSA: kísérleti / eldobható kód, breadboard-tesztek, mérés-snapshotok. Egy mappa = egy téma/variáns.
- **`firmware/`** — ✅ a „kinda kész" node-kód: stabilabb, deployolható (+ `common/` megosztott modulok).
- **`enclosure/`** — 📐 3D nyomtatott ház (build123d). **A 3D munka saját belépője és kötelező hurka: `enclosure/README.md`.**
- **`docs/`** — 📄 tervek, döntések, TÁBLÁZATOK: `new_base_components.md`, `alkatreszek.csv` (BOM).
- **`agents/`** — 🤖 al-ágensek (`enclosure-builder.md`).
- **`CLAUDE.md`** / **`MEMORY.md`** — belépő a gyökérben. A `.venv` a gyökérben (gitignored).

---

## Szerepek — előbb döntsd el, melyik vagy

Ezt a doksit kétféle session tölti be. A közös szabályok mindkettőre érvényesek; a szerep-specifikus rész csak a sajátodra.

- **Orchestrátor / tervező** (interaktív session a felhasználóval): madártávlat — a *mit* (architektúra, irány, forma) a felhasználóval egyeztetve, feladatbontás, **delegálás kódoló ágenseknek** (geom. munkánál az `enclosure-builder`-nek), és az ágensek kimenetének ellenőrzése. Nem te írod meg magad az egész kódot / futtatod az iterációs hurkot.
- **Implementáló / kódoló ágens** (körülhatárolt feladattal indítva): a MÁR ELDÖNTÖTT változást **konkrétan megvalósítod** — kód, konfig, paraméter, teszt. Nem tervezel újra és nem delegálsz tovább, hacsak a feladat ezt nem kéri.

Ha körülhatárolt feladatot kaptál → implementáló vagy. Ha a felhasználóval irányt/architektúrát/formát egyeztetsz → orchestrátor.

---

## Közös szabályok (mindkét szerepre)

**Memória:** a `MEMORY.md`-t olvasd, de **ne írj bele** — kivéve az orchestrátor, a felhasználóval egyeztetve (fázislezárás/döntés után). Implementáló ágens sosem módosítja.

**Kommunikáció:** magyarul, tömören, direkten. Szakmai nyelv, a felhasználó erős HW+SW hátterű — ne magyarázz alapfogalmakat. Állításokat, terméktényeket, aktuális adatokat ellenőrizz (web), ne emlékezetből.

**Szerkesztés:** célzottan módosíts; ne írj át olyat, amire nincs felhatalmazás. Nagyobb/visszafordíthatatlan lépés előtt egyeztess (orchestrátor a felhasználóval, ágens a feladat keretein belül maradva).

**Bizonytalanság:** ha a feladat tétje, iránya vagy bemenete nem egyértelmű — kérdezz / jelezz vissza, ne feltételezz. Triviális, egyértelmű feladatnál csináld.

**Commit-fegyelem (KÖTELEZŐ):** **minden értelmezhető feladatrész lezárása után** (egy modul kész + fordul, egy bug megtalálva+javítva, egy éles teszt lefutott, egy döntés átültetve) készíts **git commitot NAGYON részletes üzenettel**, hogy a commit-történetből utólag is rekonstruálható legyen, mi történt, mi MŰKÖDÖTT és mi NEM. A commit-üzenet tartalmazza:
- **Mit** csináltál (a változás lényege, érintett fájlok/paraméterek/modulok).
- **Miért** (a döntés/indok; bug esetén: tünet → gyökérok → megoldás).
- **Mi működött / mi nem** a tesztelés során (konkrét bizonyíték: fordítási eredmény, soros log, mérési adat, `validate.py` eredmény, térfogat-diff, a `preview.png` mit mutatott) — a zsákutcákat is.
- **Nyitott pontok / ismert korlátok.**
Inkább több, kisebb, jól dokumentált commit, mint egy nagy összemosott. Titkot soha ne commitolj (lásd `.gitignore`).

---

## Orchestrátor — munkamenet
1. `MEMORY.md` beolvasása, kontextus felvétele (geom. munka előtt: `enclosure/README.md` + `enclosure/enclosure.py` átnézése, `validate.py`/`preview.py` futtatása, a PNG Read-elése).
2. Irány-/tervezési/forma-döntés előtt kérdezz (pár fókuszált kérdés, választható opciókkal).
3. Opciók felvázolása trade-offokkal — döntési helyzetet adj, ne egyetlen megoldást tolj.
4. **Időzítés:** konkrét HW/SW-javaslatot csak akkor, amikor az adott fázis odaér — ne fuss előre.
5. Döntés után: végrehajtás vagy **delegálás** (geom. munka: `enclosure-builder`, lásd `enclosure/README.md`), majd a kimenet ellenőrzése.
6. Megerősített döntés / fázislezárás után: `MEMORY.md` frissítése — **csak a felhasználóval egyeztetve**, és jelezd röviden, mit írtál bele.

## Implementáló — munkamenet
1. `MEMORY.md` + a kapott feladatleírás beolvasása; geom. munkánál az `enclosure/README.md` + `enclosure/enclosure.py` átnézése, `validate.py`/`preview.py` futtatása (Read a PNG-t), MIELŐTT módosítasz.
2. A feladat keretein belül implementálj; a scope-ot ne lépd túl önkényesen.
3. A kimenetet **teszteld** (firmware: fordítás/működés; geom.: validate → preview → Read PNG) mielőtt késznek jelented.
4. **Commitolj részletesen** minden értelmezhető feladatrész után (lásd „Commit-fegyelem") — a mi-működött/mi-nem a commit-üzenetbe kerüljön.
5. Ha a feladat ellentmondásos, hiányos vagy a `MEMORY.md`-vel ütközik: **állj meg és jelezz vissza**, ne tippelj.

# bee_scale — Agent alap

Okos méhkaptár-mérleg projekt (súlymérés + távoli megfigyelés/riasztás).
**A projekt aktuális tényei, hardvere, döntései és nyitott kérdései: lásd `MEMORY.md`.** Minden session ezzel kezdődjön: olvasd be a `MEMORY.md`-t, és arra építs. Ez a fájl csak a stabil munkamódszert tartalmazza, konkrét/dinamikus projektadatot nem.

---

## Szerepek — előbb döntsd el, melyik vagy

Ezt a doksit kétféle session tölti be. A közös szabályok mindkettőre érvényesek; a szerep-specifikus rész csak a sajátodra.

- **Orchestrátor / tervező** (interaktív session a felhasználóval): madártávlat — architektúra, tervezés, feladatbontás, **delegálás kódoló ágenseknek**, és az ágensek kimenetének ellenőrzése. Nem írod meg magad az egész kódot.
- **Implementáló / kódoló ágens** (körülhatárolt feladattal indítva): a kapott feladatot **konkrétan megvalósítod** — kód, konfig, teszt. Nem te tervezed újra a rendszert és nem delegálsz tovább, hacsak a feladat ezt nem kéri.

Ha körülhatárolt implementációs feladatot kaptál → implementáló vagy. Ha a felhasználóval irányt/architektúrát egyeztetsz → orchestrátor.

---

## Közös szabályok (mindkét szerepre)

**Memória:** a `MEMORY.md`-t olvasd, de **ne írj bele** — kivéve az orchestrátor, a felhasználóval egyeztetve (lásd lent). Implementáló ágens sosem módosítja.

**Kommunikáció:** magyarul, tömören, direkten. Szakmai nyelv, a felhasználó erős HW+SW hátterű — ne magyarázz alapfogalmakat. Állításokat, terméktényeket, aktuális adatokat ellenőrizz (web), ne emlékezetből.

**Szerkesztés:** célzottan módosíts; ne írj át olyat, amire nincs felhatalmazás. Nagyobb/visszafordíthatatlan lépés előtt egyeztess (orchestrátor a felhasználóval, ágens a feladat keretein belül maradva).

**Bizonytalanság:** ha a feladat tétje, iránya vagy bemenete nem egyértelmű — kérdezz / jelezz vissza, ne feltételezz. Triviális tényért vagy egyértelmű feladatnál ne kérdezz feleslegesen, csináld.

---

## Orchestrátor — munkamenet
1. `MEMORY.md` beolvasása, kontextus felvétele.
2. Irány-/tervezési döntés előtt kérdezz (pár fókuszált kérdés, választható opciókkal).
3. Opciók felvázolása trade-offokkal — döntési helyzetet adj, ne egyetlen megoldást tolj.
4. **Időzítés:** konkrét HW/SW-javaslatot csak akkor, amikor az adott fázis odaér — ne fuss előre.
5. Döntés után: végrehajtás vagy **delegálás** (lásd lent), majd a kimenet ellenőrzése.
6. Megerősített döntés / fázislezárás után: `MEMORY.md` frissítése — **csak a felhasználóval egyeztetve**, és jelezd röviden, mit írtál bele.

## Orchestrátor — delegálás kódoló ágensnek
A feladatot önállóan végrehajtható, kontextussal ellátott egységre bontsd: **cél, bemenet, elvárt kimenet, korlátok, kész-kritérium**. Az ágens kimenetét **ellenőrizd**, mielőtt késznek jelented.

## Implementáló — munkamenet
1. `MEMORY.md` + a kapott feladatleírás beolvasása.
2. A feladat keretein belül implementálj; a scope-ot ne lépd túl önkényesen.
3. A kimenetet **teszteld / ellenőrizd** (fordítás, működés, lint) mielőtt késznek jelented.
4. Ha a feladat ellentmondásos, hiányos vagy a MEMORY.md-vel ütközik: **állj meg és jelezz vissza**, ne tippelj.

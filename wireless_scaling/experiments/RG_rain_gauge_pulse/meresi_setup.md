# RG — Mérési setup (esőmérő reed-impulzus)

A **konkrét elrendezés**: bekötés, mit mérj, milyen módban, mit csinálj bounce/veszteség
esetén. Firmware: `main/rg_rain_gauge_pulse.c` (paraméterek a fájl tetején). Eredmények:
`mereseredmenyek.csv`. Cél/küszöbök: `README.md` pass/fail tábla.

## 1. Bekötés
- RJ11 **két középső** ere → egyik **GPIO4** (`RG_PIN`, belső pull-up), másik **GND**. Polaritás mindegy.
- A többi RJ11-ér ezen a szenzoron nincs bekötve.
- Kézi teszt kábel nélkül is: a két középső érintkezőt egy vezetékkel rövidre zárva = 1 „billenés".

## 2. MODE 0 — padi kalibráló / bounce (`RG_MODE 0`)
1. `pio run -t upload -t monitor`.
2. **Bounce-teszt:** billentsd a vödröt kézzel egyesével → minden billentésnél PONTOSAN 1 `TIP #n`
   log jöjjön (ne több — az bounce). Ha több: emeld a `RG_DEBOUNCE_MS`-t vagy tegyél RC-t a pinre.
3. **Térfogat-kalibráció:** mérj ki ismert vízmennyiséget (pl. fecskendővel X ml), öntsd lassan a
   tölcsérbe, olvasd a billenésszámot. Ellenőrizd: `szamolt_mm ≈ kiontott_terfogat / tolcser_terulet`.
   A gyári 0,2794 mm/tip ehhez képest ±? — jegyezd a csv-be.

## 3. MODE 1 — deep-sleep wake-on-tip (`RG_MODE 1`)
1. Állítsd `RG_MODE 1`-re, `pio run -t upload`. Flasheléshez alvó boardnál **BOOT+reset** (H1).
2. **Veszteség/dupla teszt:** billents N-szer (lassan, >0,5 s között) → a következő ébredéskori log
   `össz. tip=N` legyen. Eltérés = elveszett (túl gyors egymásután / lockout túl hosszú) vagy dupla (bounce).
3. **Ébredés-költség:** multiméter a node tápágába (H1-setup), billentgess adott rátán → átlagáram.
   Vesd össze a H1/H3 mérleggel: elfogadható-e az (A) út a várható eső-rátán, vagy kell a (B) külső IC.

## 4. Ha baj van
- **Dupla count / bounce:** `RG_DEBOUNCE_MS` ↑ (150 → 250), és/vagy soros 100 Ω + 100 nF RC a pinre.
- **Elveszett billenés (MODE 1):** `RG_TIP_WINDOW_MS` túl hosszú a lockouthoz — csökkentsd; vagy a
  billentés túl gyors volt (valós esőnél nem gond).
- **Azonnali ál-ébresztés alvás után:** a pin nem HIGH alvásban → ellenőrizd a pull-up + `gpio_hold_en`-t,
  és hogy a reed tényleg nyit (nem ragadt).

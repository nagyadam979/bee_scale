# Hőkalibráció — eredmények és recept (4 cella)

**Dátum:** 2026-07-13 · **Cellák:** node1, node2, node3, master
**Mérés:** HX711 `raw` count vs cella-hő (DS18B20 `scale_c`; master-nél `ds_a_c` = mérleg-hőmérő).
**Adat:** `data/*.csv` = **1 kg terhelt** (VÉGLEGES); `data/no_load/*.csv` = terheletlen (archív, elvetve — lásd lentebb).

---

## 1. A fő tanulság — TERHELVE kell hőkalibrálni

- **Terheletlenül** (no_load) a node-ok `raw(T)` görbéje **kaotikus hiszterézis-hurok**: a vízálló gumi membrán dominál (hidegben megkeményedik, beragad → elenged), a görbe kétértékű és illeszthetetlen.
- **1 kg előterheléssel** a membrán a **monoton munkapontjára feszül**, és előjön alóla a load cell tiszta **logaritmikus** karaktere.
- **Bizonyíték:** a terhelt mérés `R²≈0.99` tiszta log (node1/node3), a terheletlen ugyanott hurok/plató. A terhelt **annak ellenére tisztább**, hogy ~15×-gyorsabb hőrámpával készült (1.3 °C/perc vs 0.1 °C/perc) → **nem a rámpa-sebesség a domináns, hanem a terhelés**.
- **Következmény:** a végső hőkalibrációs számok a **terhelt (1 kg)** mérésből valók. A `no_load/` adat elvetve (archívnak megtartva).

> Korábbi munkahipotézis („a gyors rámpa rontja a terhelt mérést") **megdőlt** — az overlay-ábrák (no_load vs 1 kg) egyértelműen a terhelt javára döntöttek.

---

## 2. Cellánkénti karakter és végső számok

**MINDEN paraméter cellánként EGYEDI — soha nem összevonva!**

| cella | modell | paraméterek | R² | meredekség 0 °C → 30 °C | megjegyzés |
|---|---|---|---|---|---|
| **node1** | log | `a = −8370`, `c = 5.04` | 0.99 | −1662 → −239 c/°C | tiszta ✅ |
| **node2** | log | `a = −5281`, `c = 2.85` | 0.82 | −1853 → −161 c/°C | kis púp 18–27 °C (mozgatás-maradvány) |
| **node3** | log | `a = −11034`, `c = 5.18` | 0.99 | −2130 → −314 c/°C | tiszta ✅ |
| **master** | **lineáris** | `tempCoef = +318 c/°C` | — | +318 (konstans) | **ellentétes előjel!**, lapos; = avg(no_load +372, 1 kg +264) |

- A `master` **pozitív** meredekségű (melegre `raw` NŐ), a node-ok negatívak (melegre csökken) — egyedi cella-karakter.
- A master-nek **nincs érdemi membrán-shuntja**: csak a cella + HX711 saját, kicsi, közel lineáris hő-együtthatója marad → ezért lineáris, nem log. (Lehetséges, hogy a log-könyök a mért tartomány **alatt**, hidegebben van — nem tesztelt, a mélyhűtő ~−4 °C-ig ért.)

---

## 3. A kalibrációs recept (felhő-oldali)

A `README.md` alapmodellje: `kg = (raw − zero_count − tempCorr) / count_per_kg`.
Csak a **`tempCorr` tag** változik cellatípus szerint:

**Node-ok (log):**
```
tempCorr = a · ln( (scale_c + c) / (refTemp + c) )
kg = ( raw − zero_count − a·ln((scale_c + c)/(refTemp + c)) ) / count_per_kg
```

**Master (lineáris — a meglévő modell változatlanul):**
```
tempCorr = tempCoef · (scale_c − refTemp)
kg = ( raw − zero_count − tempCoef·(scale_c − refTemp) ) / count_per_kg
```

Kulcsok:
- `a`, `c`, `tempCoef`: a **2. pont táblázata**, cellánként külön.
- `refTemp`: a **súly-kalibráció** bench-hőmérséklete (`scale_c`). A hőkorrekció itt definíció szerint **0** (`ln(1)=0`, ill. `T−refTemp=0`).
- A log-fit **`b` abszolút tagja NEM kell** — a refTemp-re normálás elnyeli; a szintet a `zero_count` adja.
- Előjel: node `a<0` → melegben `tempCorr<0` → `raw − tempCorr` visszaadja a nagyobb, refTemp-beli count-ot. ✅ Konzisztens a lineáris modellel.

---

## 4. Adatfeldolgozási megjegyzések (mit kellett kivenni, és miért)

- **~10–13 °C-os lépcső (step):** a hajszárító bekapcsolása + a súlyok meglökése egy időben — lépcső-taggal illesztve / kivágva. **Nem valós jel.**
- **Lökés-tüskék** (node1 ~22–24 °C, node3 hideg vég, master ~30 °C): mechanikai bolygatás, sigma-clippel (2.5σ) kidobva.
- **Lag-hurok / púp 18–30 °C-nál:** a gyors rámpa miatt a mérőtest lemarad a szenzortól. A **lapos jelű masternél** relatíve nagy → gyenge R², de az irány és nagyságrend stabil (a két terhelési szint egyezik).

---

## 5. Nyitott pontok / korlátok

- **`count_per_kg` és `zero_count` MÉG NINCS** — a node súly-kalibráció most készül. Amíg nincs, a drift **kg-ban nem számszerűsíthető** (pl. a master +318 c/°C jelentősége csak a `count_per_kg` ismeretében dől el — lehet elhanyagolható is).
- **master `tempCoef` „egyelőre" lineáris +318** — döntés szerint nincs újabb hőkalibráció. Lassú, terhelt újramérés (opcionális) pontosíthatná a teljes tartományra; a jelenlegi érték a hideg szakasz + a két terhelés átlagából van.
- **node2 log R²=0.82** — a maradék púp mozgatás-műtermék; ha a gyakorlatban zavar, node2 terhelt újramérés lassan, mozgatás nélkül.
- **Hidegebb-könyök hipotézis (master):** nem tesztelt (mélyhűtő nem elég hideg).

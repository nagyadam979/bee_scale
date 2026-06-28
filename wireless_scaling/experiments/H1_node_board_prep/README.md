# H1 — Node board előkészítés (deep-sleep áram)

**Típus:** Hardver chunk · **Setup:** 0 (1 node, multiméter, rádió nélkül) · **Irány:** (A) elektromos/energetikai

## Cél
A node nyers nyugalmi (deep-sleep) áramának kimérése és minimalizálása. Ez a teljes
node-energiamérleg **alapszáma** — minden későbbi élettartam-becslés erre épül. A gyári LED
rejtett fogyasztó; le kell szedni és mérni kell a hatását.

## Függőségek
- **Előfeltétel:** nincs (a validáció első lépése, rádió nélkül).
- **Mit blokkol:** gyakorlatilag mindent — a node HW-konfiguráció (pl. LDO maradjon-e) az
  összes többi energiamérést befolyásolja. Ennek **zártnak kell lennie, mielőtt bármilyen
  rádiós kód indul.** A kritikus adatút első eleme: H1 → H2 → Sz0 → …

## Bemenet / előfeltételek
- 1 db ESP32-C3 SuperMini node board.
- Asztali multiméter (µA-tartomány) vagy precíz árammérő.
- Stabil tápforrás (a végleges tápúttól külön is mérhető a board maga).

## Megvalósítás vázlata (NEM kód)
1. **Gyári állapot mérése:** board deep sleepben (`esp_deep_sleep`), LED-del, áram feljegyezve.
2. **LED leszedése**, újramérés.
3. **LDO alvariánsok:** mérés a board LDO-jával együtt, illetve nélküle/megkerülve — két alvariáns.
4. Minden mérés a Setup 0 jegyzőkönyvbe (állapot → µA).
- A firmware itt minimális: csak `esp_deep_sleep` + a HX711 GPIO power-down (lásd H2),
  semmi rádió. Szoftver-bizonytalanság nincs.

## Kimenet / deliverable
- Deep-sleep áram mérve **gyári vs. módosított** (LED nélkül) állapotban, LDO le/fent alvariánssal.
- Mérési jegyzőkönyv-bejegyzés számszerű küszöbökkel.

## Pass / fail (Setup 0 táblázat)
| Mérés | Jó | Elfogadható | Problémás |
|---|---|---|---|
| Teljes node deep sleep, LED nélkül, HX711 power-downban | **< 50 µA** | < 100–200 µA | **> 500 µA** → board / HX711 / tápút hibakeresés |

## Nyitott kérdések / kockázatok
- LDO megtartása vs. megkerülése — trade-off (egyszerűség vs. nyugalmi fogyasztás); a mérés dönt.
- A „teljes node" küszöb a HX711-gyel együtt értendő → a végső pass H2-vel közös.

## Hivatkozások
- `../../MEMORY.md` (7. szakasz, master/node boardok), `../../docs/new_base_components.md` (Slave BOM).
- Társ-chunk: [`../H2_hx711_power_path/`](../H2_hx711_power_path/) (párhuzamosítható).

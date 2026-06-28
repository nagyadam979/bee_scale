# Sz1 — Node mérés + ring buffer

**Típus:** Szoftver chunk · **Setup:** 0 (rádió nélkül) · **Irány:** (B) rádiós protokoll alapja

## Cél
Mérés → ring buffer → deep sleep, **rádió nélkül**. Ébredés után korrekt, `seq`-elt mintasor
álljon a bufferben. A **kritikus út része**: stabil puffer nélkül az ACK utáni törlés, az
újraküldés és a batch-logika sem tesztelhető korrektül.

## Függőségek
- **Előfeltétel:** H1 + H2 (a node HW-konfiguráció és a HX711 power-down kész). Sz0 header párhuzamos.
- **Mit blokkol:** Sz2 → Sz3 (a rádiós réteg a stabil bufferre épül).
- Kritikus adatút: H1 → H2 → Sz0 → **Sz1** → Sz2 → Sz3.

## Bemenet / előfeltételek
- H1/H2-vel előkészített node (LED le, HX711 power-down OK).
- Sz0 közös header (a buffer rekord-formátuma ehhez igazodik).
- HX711 **LDO-ról szűrt tiszta 3.3V**-on (különben a `weight` az akku töltöttségével driftel).

## Megvalósítás vázlata (NEM kód)
1. Ébredés → HX711 power-up → mintavétel (weight, temp, vbat) → ring bufferbe `seq`-szel.
2. HX711 power-down → `esp_deep_sleep` a következő slotig.
3. Ring buffer reset-túlélése / felülírási politika (régi minta sorsa, ha tele).
4. Ébredés utáni ellenőrzés: a buffer korrekt, hézagmentes `seq`-sort tartalmaz.

## Kimenet / deliverable
- Korrekt `seq`-elt mintasor a ring bufferben ébredés után, rádió nélkül.

## Pass / fail
| Feltétel | Pass |
|---|---|
| `seq` folytonosság ébredések során | monoton, hézagmentes |
| Minta integritás (weight/temp/vbat) | minden ébredésnél érvényes rekord |
| Deep sleep ↔ mintavétel ciklus | stabil, nincs elveszett/duplázott minta |

## Nyitott kérdések / kockázatok
- Ring buffer mérete / felülírási stratégia ACK-késés esetén (Sz3 buffer-clear-rel közös döntés).
- `seq` túlcsordulás kezelése (mezőszélesség Sz0-ban).
- Mintaidő-jelölés: `sample_age_s` vs. `sample_period_s` (Sz0/Sz4 összefügg).

## Hivatkozások
- `../../docs/new_base_components.md` (payload mezők), `../../MEMORY.md` (7. nyitott kérdés #3 — HX711 táp).
- Függ: [`../H1_node_board_prep/`](../H1_node_board_prep/), [`../H2_hx711_power_path/`](../H2_hx711_power_path/), [`../Sz0_wire_protocol/`](../Sz0_wire_protocol/).

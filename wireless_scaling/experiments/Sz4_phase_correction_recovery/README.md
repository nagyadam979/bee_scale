# Sz4 — Fáziskorrekció + recovery

**Típus:** Szoftver chunk · **Setup:** 2 (1 node + master, hosszú futás) · **Irány:** (B) rádiós protokoll

## Cél
A node deep-sleep órájának driftjét kompenzáló slot-tartás: a master oldali
`next_slot_offset_s` / `clock_correction_s` számítás, és a node szélesebb keresése ACK-hiányra
(recovery). Cél, hogy a node injektált / mért drift mellett is a **slotjában maradjon N cikluson át**.

## Függőségek
- **Előfeltétel:** **Sz3** (működő ACK + slot) **+ a Setup 2 / M1-ben mért valós drift** (guard-window paraméter).
- Drift-ág: Sz3 → Setup 2 / M1 → **Sz4**.

## Bemenet / előfeltételek
- Sz3 kész (ACK-vezérelt buffer + slot-logika).
- M1 kimenete: tényleges slow-clock drift + minimális guard window.
- Sz0 ACK-mezők: `next_slot_offset_s`, `clock_correction_s`.

## Megvalósítás vázlata (NEM kód)
1. **Master korrekció-számítás:** várt vs. tényleges ébredésből `next_slot_offset_s` és `clock_correction_s`.
2. **Node alkalmazás:** a node a korrekciót a következő deep-sleep időzítőjébe építi.
3. **Recovery:** ACK-hiányra a node szélesebb keresési ablakot nyit (re-akvizíció).
4. **Drift-injektálás (determinisztikus teszt):** slot-offset manuális eltolása → a korrekciós matek
   determinisztikusan tesztelhető a hosszú futás kivárása nélkül is.
5. Validáció: N cikluson át a node a slotjában marad injektált és valós drift mellett.

## Kimenet / deliverable
- Injektált / mért drift mellett a node **a slotjában marad N cikluson át**; ACK-hiányból visszatalál.

## Pass / fail
| Feltétel | Pass |
|---|---|
| Slot-tartás N cikluson át | a node az ablakon belül ébred (guard window-on belül) |
| Injektált offset korrekció | determinisztikusan visszahúzza a slotba |
| ACK-hiány recovery | szélesebb keresés → re-akvizíció, nem esik ki véglegesen |

## Nyitott kérdések / kockázatok
- **N és a guard window konkrét értéke M1-től függ** — előtte csak előzetes paraméterek.
- Korrekció modell: egyszeri offset vs. folyamatos lineáris óra-korrekció (M1 driftjellege dönti el).
- Hőmérséklet-függés (a drift hővel változik) — M1 méri, Sz4 ennek a tartományra méretez.

## Hivatkozások
- `../../docs/new_base_components.md` (időbélyegzés), `../../MEMORY.md` (7. szakasz).
- Függ: [`../Sz3_rendezvous_app_ack/`](../Sz3_rendezvous_app_ack/), [`../M1_drift_guard_window/`](../M1_drift_guard_window/).

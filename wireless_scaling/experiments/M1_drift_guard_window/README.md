# M1 — Driftmérés / guard-window méretezés

**Típus:** Mérési chunk · **Setup:** 2 (1 node + master, hosszú futás) · **Irány:** (B) rádiós protokoll (mérési input)

## Cél
A node saját **deep-sleep időzítőjének tényleges driftjének** kimérése több órás/napos futás
alapján (master-logolt várt vs. tényleges ébredés), hőmérséklet-ingadozásban. Ez **táplálja az
Sz4 guard-window paramétert** — itt dől el a minimális guard window.

## Függőségek
- **Előfeltétel:** működő ACK + slot (Sz3), hogy a master logolni tudja a várt/tényleges ébredést.
- **Mit táplál:** **Sz4** (fáziskorrekció + recovery) guard-window paramétere.
- Drift-ág: Sz3 → **Setup 2 / M1** → Sz4.

## Bemenet / előfeltételek
- 1 node + master, hosszú (napos) futás, természetes hőmérséklet-ingadozás.
- Master nyers logolás (várt vs. tényleges ébredési idő, hőmérséklet).
- Python offline elemző (drift-illesztés a nyers logon).

## Megvalósítás vázlata (NEM kód)
1. A master **minden ébredésnél logolja a várt és a tényleges időt** (+ hőmérséklet).
2. Napokig futó mérés hő-ingadozásban (idő-, nem darabszám-igényes).
3. **Offline Python:** drift (ppm / s-per-ciklus) és annak hőfüggésének kiértékelése a nyers logból.
4. Ebből a **minimális guard window** + ajánlott korrekciós modell (offset vs. lineáris) → Sz4-nek.

## Kimenet / deliverable
- Tényleges slow-clock drift (érték + hőfüggés) és a **minimális guard window** számszerű ajánlás.

## Pass / fail
| Feltétel | Pass |
|---|---|
| Drift jellemzés | mért drift-érték + hőfüggés napos futásból |
| Guard-window ajánlás | konkrét szám, amivel Sz4 N cikluson át slotban tart |
| Reprodukálhatóság | offline Pythonból a nyers log újrajátszva ugyanazt adja |

## Nyitott kérdések / kockázatok
- A drift hőfüggése mekkora → guard window a teljes üzemi hőtartományra méretezendő.
- Korrekciós modell választás (offset vs. folyamatos lineáris) — a mért driftjelleg dönti el (Sz4-gyel közös).
- Futásidő: napos nagyságrend → ez a leghosszabb átfutású chunk, korán indítandó Sz3 után.

## Hivatkozások
- `../../docs/new_base_components.md` (időbélyegzés), `../../MEMORY.md` (7. szakasz).
- Függ: [`../Sz3_rendezvous_app_ack/`](../Sz3_rendezvous_app_ack/). Táplálja: [`../Sz4_phase_correction_recovery/`](../Sz4_phase_correction_recovery/).

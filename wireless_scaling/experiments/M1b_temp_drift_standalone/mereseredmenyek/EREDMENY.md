# M1b — STANDALONE off-grid hő-drift mérő · mérési eredmény

> Egyszeri éles futás (2026-06-30), ESP32-C3 SuperMini + ZS-042 (DS3231+AT24C32) + HX711 +
> mérőcella, nagy power bankról, off-grid. A részletes fejlesztési napló a commit-történet.
> **A modul ezzel a setuppal LEZÁRVA**; a teljesebb (erase-biztonság, prod-üzem) tesztek a
> végleges masterrel (a master immár ESP32-C3 SuperMini — 2026-07-02 pivot, lásd H4), külön környezetben jönnek.

## Setup
- Node deep-sleep, **T = 300 s** ébredési periódus, konstans cella-terhelés.
- Minden ébredéskor: DS3231 idő+hő (referencia), C3 die-hő, HX711 nyers 24-bit → AT24C32 EEPROM.
- I2C: SDA=GPIO4, SCL=GPIO3. HX711: PD_SCK=GPIO5, DOUT=GPIO6. Power bank + terhelő (bleeder)
  ellenállás az auto-shutoff ellen.

## Futás
- **7,35 óra**, **89 érvényes rekord** (cycle 1..89).
- DS3231 ambiens hő-tartomány: **11,75 → 36,00 °C (span 24,25 °C)** — valódi, tág sweep.

## Eredmények
| Mennyiség | Érték | R² | Megjegyzés |
|---|---|---|---|
| **Mérőcella+HX711 hő-drift** | **1031 counts/°C** | **0,974** | Tiszta, erős lineáris jel; hx-span ~27 300 counts a 24 °C-on. Ez az M1b fő célja. |
| **Óra-drift (RC slow-clock)** | **−2482 ppm** (−8,94 s/óra) | ~1,0 | awake-korrigált (avg_awake 1296 ms levonva); nyers +1838 ppm. |
| Óra-drift vs hő (ppm/°C) | ~48 ppm/°C | 0,02 | **Nem megbízható** a jelenlegi residual-derivált módszerből; a jel ott van (a fit residual 0,27 → 1,4 s-ra nőtt a hővel), de a tiszta ppm/°C jobb illesztést igényel. |

## Adatfájlok
- `field_recovery_raw.txt` — a teljes EEPROM nyers recovery-dumpja (a tényleges HW-adat forrása).
- `field_clean.txt` — szűrt, dedupolt idősor (89 rekord), ebből az elemzés.
- `field.csv` / `field.json` — az `m1b_analyze.py` per-rekord és összegző kimenete.
- `bench_awake_validation.txt` — padi awake_ms HW-validáció (T=10 s) referenciának.

Újraelemzés: `python3 ../m1b_analyze.py field_clean.txt --out-prefix field`.
(A `.txt` kiterjesztés szándékos: a repo-gyökér `.gitignore` a `*.log`-ot kizárja.)

## Tanulság (a következő setupra átviendő)
- A `M1B_ERASE_AND_START=1` build **minden cold boot-on automatikusan töröl**. Az éles log
  emiatt majdnem elveszett: a board power bankról a gépre dugva cold boot-olt → az erase
  lefutott, mielőtt a BOOT+reset megelőzhette volna. **A teljes adat visszanyerhető volt**, mert
  az erase csak a fejléc write_index-ét nullázza, a rekordok fizikailag az EEPROM-ban maradtak
  → a `M1B_DUMP_ALL` recovery-mód (teljes EEPROM-kiolvasás + Python-szűrés) mentette ki.
- A végleges (C3-master, 2026-07-02 pivot) setupban az erase legyen **dump-alapértelmezett + explicit,
  véletlenül nem kiváltható** erase-úttal.

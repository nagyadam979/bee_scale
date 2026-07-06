# experiments/ — Kísérleti validációs chunkok (index)

Ez a mappa a kaptármérleg-rendszer **kísérleti összeállításának validációs rendjét** projektesíti.
Minden almappa **egy chunk** = egy körülhatárolt feladat, **egyetlen mérhető kimenettel** és
számszerű (vagy egyértelmű logikai) pass/fail kritériummal.

> ⚠️ **Vegyes állapot:** több chunk már **kóddal + mért eredménnyel** kész (H1, H2, Sz2, M1, M1b),
> az RG folyamatban; a többi mappa még **README-spec** (mit kell implementálni + hogyan dől el). Az
> egyes README-k jelzik a saját státuszukat.

**Alapelv:** minden kockázatot a *legkisebb értelmes setupban* zárunk le, és csak akkor lépünk
feljebb, ha az alacsonyabb szint zárt.

Háttér / rendszerterv: `../MEMORY.md` (7. szakasz), `../docs/new_base_components.md`, BOM: `../docs/alkatreszek.csv`.

---

## Három bizonytalanságforrás → három validációs irány

- **(A) elektromos / energetikai mérések** — single-node bench (Setup 0)
- **(B) rádiós protokoll-logika** — node + master bench (Setup 1–2)
- **(C) flotta-szintű viselkedés** — több node + master, terepközeli (Setup 2.5–3)

---

## Setup-szintek

| Szint | Összeállítás | Mit zár le |
|---|---|---|
| **Setup 0** | 1 node, multiméter, rádió nélkül | Teljes node-energiamérleg (deep sleep, LED, HX711 power-down, LDO/tápút) |
| **Setup 1** | 1 node + 1 master, padon | Protokoll-mag: `seq`-dup-szűrés, buffer-clear, egyidejű WiFi/ESP-NOW üzem (D1) |
| **Setup 2** | 1 node + master, hosszú futás | Folyamatos üzem: csomagvesztés/kézbesítés always-on RX-nél (Sz2b), stabilitás |
| **Setup 2.5** | 3 node + master (opcionális) | Dup-kezelés + broadcast-ütközés valós forgalomban, kisebb káosz |
| **Setup 3** | több node + master, terepközeli | Skálázódás: broadcast-ütközés, csomagvesztés-arány, redundancia-méretezés (a végleges node-szám egyelőre nyitott) |

---

## Chunk-térkép

### Hardver
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| H1 | [`H1_node_board_prep/`](H1_node_board_prep/) | 0 | LED leszedve, deep-sleep áram mérve (gyári vs. módosított) |
| H2 | [`H2_hx711_power_path/`](H2_hx711_power_path/) | 0 | HX711 nyugalmi áram + excitation-lekapcsolás igazolva |
| H3 | [`H3_master_power_duty_cycle/`](H3_master_power_duty_cycle/) | — | **FELFÜGGESZTVE** (2026-07-03) — master hálózati táp/always-on, nincs akku-pack méretezés |
| H4 | [`H4_master_deep_sleep/`](H4_master_deep_sleep/) | 0 | **TÁRGYTALAN** (2026-07-03) — master hálózati táp/always-on, nem alszik (a mérés: S3 mA-t húzna) |

### Szoftver
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| Sz0 | [`Sz0_wire_protocol/`](Sz0_wire_protocol/) | Setup 1 előtt | Verziózott közös packet/header (node ↔ master szerződés) |
| Sz1 | [`Sz1_node_sampling_ring_buffer/`](Sz1_node_sampling_ring_buffer/) | 0 | Korrekt `seq`-elt mintasor a ring bufferben, rádió nélkül |
| Sz2 | [`Sz2_espnow_low_level/`](Sz2_espnow_low_level/) | 1 | 1000 csomag veszteség/lefagyás nélkül padon |
| Sz3 | [`Sz3_rendezvous_app_ack/`](Sz3_rendezvous_app_ack/) | 1 | **Nagyrészt tárgytalan** always-on masterrel (tűzz-és-felejts); csak az app-ack marad opció, ha garantált kézbesítés kell |
| Sz4 | [`Sz4_phase_correction_recovery/`](Sz4_phase_correction_recovery/) | 2 | **Nagyrészt tárgytalan** — always-on master, nincs slot/rendezvous-ablak, amit tartani kéne |
| Sz2b | [`Sz2b_coexistence_packet_loss/`](Sz2b_coexistence_packet_loss/) | 1→2.5 | Csomagvesztés-kivizsgálás (M1-anomália) + WiFi STA/ESP-NOW egyidejű üzem (D1) + redundancia-méretezés |
| Sz5 | [`Sz5_master_time_uplink/`](Sz5_master_time_uplink/) | 2/3 | Rekonstruált abszolút timestamp ±1 perc, 4G uplink |

### Mérés
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| M1 | [`M1_drift_guard_window/`](M1_drift_guard_window/) | 2 | Tényleges deep-sleep timer drift → Sz4 guard-window paraméter (kimenet always-on masterrel kihasználatlan) |
| M1b | [`M1b_temp_drift_standalone/`](M1b_temp_drift_standalone/) | standalone | **KÉSZ** — off-grid hő-drift: cella+HX711 1031 counts/°C (R²=0,97) + óra-drift |

### Periféria (a H/Sz/M validációs rácson kívül)
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| RG | [`RG_rain_gauge_pulse/`](RG_rain_gauge_pulse/) | 0–1 | **FOLYAMATBAN** — esőmérő reed-impulzus (WH-SP-RG tipping bucket), C3-on |

---

> **2026-07-03 — hálózati tápú (always-on) master:** mivel a master sosem alszik és mindig figyel,
> a node deep-sleep drift nem érinti a vételt. Ezért **H3/H4 tárgytalan** (nincs master-akku), és az
> **Sz3 rendezvous / Sz4 drift-guard nagyrészt felesleges** (tűzz-és-felejts + `seq`-gap). Az M1
> guard-window kimenete kihasználatlan. A kritikus-út **fő ábrája már az always-on modell**; a régi
> duty-cycle-es ág historikusként alatta marad.

## Kritikus út és párhuzamos utak

```
Kritikus működő adatút (always-on master — 2026-07-03):
  H1 → H2 → Sz0 → Sz1 → Sz2 → Sz2b → (Sz5 uplink)

Master idő / uplink (párhuzamos):
  Sz5  (NTP folyamatosan él always-on + folyamatos neten; a ±1 perces rekonstrukció csak net-kimaradásra)

Flottaszint:
  Sz2b ──→ Setup 2.5 (opcionális) ──→ Setup 3

--- HISTORIKUS (korábbi duty-cycle-es master-modell) ---
  Kritikus adatút:       H1 → H2 → Sz0 → Sz1 → Sz2 → Sz3
  Drift / guard-window:  Sz3 ──→ Setup 2 / M1 ──→ Sz4
```

- **H1 és H2 párhuzamosítható** (függetlenek).
- **Sz0 minden rádiós chunk előfeltétele** — ha stabil a header, Sz2/Sz3/Sz4 egymástól függetlenül cserélhető.
- **Sz5 párhuzamosan fejleszthető** a rádiós réteggel.

---

## Iterálhatóság (a gyors javítási ciklusok)

1. **Egységes batch-keret elöl (Sz0).** A közös header a chunkok közti szerződés; stabil interfész mellett a node és a master külön javítható.
2. **Master mindent nyersen loggol, a Python külön réteg.** Timestamp-rekonstrukció (Sz5), drift-elemzés (M1), dup-statisztika offline Pythonból fut a nyers logon, valós eszközfutás nélkül újrajátszva — a leggyorsabb javítási ciklus, pont a két legbizonytalanabb pontra (guard window, lineáris óra-korrekció).
3. **Setup 0 mérési jegyzőkönyv számszerű küszöbökkel.** A HW-chunkok (H1/H2) kimenete egy-egy szám a táblázatban (µA / mA); a H3 felfüggesztve (always-on master, nincs akku-pack méretezés). A „javítás" mérhető: alkatrész cseréje → újramérés → a szám fel/le megy.

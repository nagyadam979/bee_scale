# experiments/ — Kísérleti validációs chunkok (index)

Ez a mappa a kaptármérleg-rendszer **kísérleti összeállításának validációs rendjét** projektesíti.
Minden almappa **egy chunk** = egy körülhatárolt feladat, **egyetlen mérhető kimenettel** és
számszerű (vagy egyértelmű logikai) pass/fail kritériummal.

> ⚠️ **Ezek a mappák jelenleg CSAK README-t tartalmaznak** (specifikáció / implementációs terv).
> Kód még nincs — minden almappa README-je leírja, _mit_ kell majd implementálni és _hogyan dől el_, hogy kész-e.

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
| **Setup 1** | 1 node + 1 master, padon | Protokoll-mag (ACK, dup-szűrés, buffer-clear), drift mesterségesen injektálható |
| **Setup 2** | 1 node + master, hosszú futás | Tényleges slow-clock drift → guard-window méretezés |
| **Setup 2.5** | 3 node + master (opcionális) | Slot-sorrend, recovery, dup-kezelés valós forgalomban, kisebb káosz |
| **Setup 3** | 12 node + master, terepközeli | Skálázódás: slot-ütközés, retry-torlódás, RX-ablak kihasználtság |

---

## Chunk-térkép

### Hardver
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| H1 | [`H1_node_board_prep/`](H1_node_board_prep/) | 0 | LED leszedve, deep-sleep áram mérve (gyári vs. módosított) |
| H2 | [`H2_hx711_power_path/`](H2_hx711_power_path/) | 0 | HX711 nyugalmi áram + excitation-lekapcsolás igazolva |
| H3 | [`H3_master_power_duty_cycle/`](H3_master_power_duty_cycle/) | 2/2.5/3 | Master 2h-ciklus átlagáram valós forgalommal |
| H4 | [`H4_master_deep_sleep/`](H4_master_deep_sleep/) | 0 | **S3-Zero master deep-sleep mA → board ELVETVE; a master is C3 SuperMini (pivot 2026-07-02)** |

### Szoftver
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| Sz0 | [`Sz0_wire_protocol/`](Sz0_wire_protocol/) | Setup 1 előtt | Verziózott közös packet/header (node ↔ master szerződés) |
| Sz1 | [`Sz1_node_sampling_ring_buffer/`](Sz1_node_sampling_ring_buffer/) | 0 | Korrekt `seq`-elt mintasor a ring bufferben, rádió nélkül |
| Sz2 | [`Sz2_espnow_low_level/`](Sz2_espnow_low_level/) | 1 | 1000 csomag veszteség/lefagyás nélkül padon |
| Sz3 | [`Sz3_rendezvous_app_ack/`](Sz3_rendezvous_app_ack/) | 1 | Buffer pontosan az ACK-elt mintákig ürül |
| Sz4 | [`Sz4_phase_correction_recovery/`](Sz4_phase_correction_recovery/) | 2 | Node a slotjában marad N cikluson át injektált/mért drift mellett |
| Sz5 | [`Sz5_master_time_uplink/`](Sz5_master_time_uplink/) | 2/3 | Rekonstruált abszolút timestamp ±1 perc, 4G uplink |

### Mérés
| Chunk | Mappa | Setup | Egymondatos kimenet |
|---|---|---|---|
| M1 | [`M1_drift_guard_window/`](M1_drift_guard_window/) | 2 | Tényleges deep-sleep timer drift → Sz4 guard-window paraméter |

---

## Kritikus út és párhuzamos utak

```
Kritikus működő adatút:
  H1 → H2 → Sz0 → Sz1 → Sz2 → Sz3

Drift / guard-window:
  Sz3 ──→ Setup 2 / M1 ──→ Sz4

Master idő / uplink (nagyrészt párhuzamos):
  Sz5  (nyers master log + szimulált kimaradások — nem blokkolja a rádiós réteget)

Flottaszint:
  Setup 2.5 (opcionális) ──→ Setup 3
```

- **H1 és H2 párhuzamosítható** (függetlenek).
- **Sz0 minden rádiós chunk előfeltétele** — ha stabil a header, Sz2/Sz3/Sz4 egymástól függetlenül cserélhető.
- **Sz5 párhuzamosan fejleszthető** a rádiós réteggel.

---

## Iterálhatóság (a gyors javítási ciklusok)

1. **Egységes batch-keret elöl (Sz0).** A közös header a chunkok közti szerződés; stabil interfész mellett a node és a master külön javítható.
2. **Master mindent nyersen loggol, a Python külön réteg.** Timestamp-rekonstrukció (Sz5), drift-elemzés (M1), dup-statisztika offline Pythonból fut a nyers logon, valós eszközfutás nélkül újrajátszva — a leggyorsabb javítási ciklus, pont a két legbizonytalanabb pontra (guard window, lineáris óra-korrekció).
3. **Setup 0 mérési jegyzőkönyv számszerű küszöbökkel.** A HW-chunkok (H1/H2/H3) kimenete egy-egy szám a táblázatban (µA / mA). A „javítás" mérhető: alkatrész cseréje → újramérés → a szám fel/le megy.

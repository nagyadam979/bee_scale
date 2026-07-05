# Sz6 — Ébredés → megszólítás → átadás protokoll (multi-node bench)

**Típus:** Szoftver chunk · **Setup:** 2 node + 1 master, padon, USB-ről · **Irány:** (B) rádiós protokoll

## Cél

A **megbízható wake→adatátadás protokoll** kidolgozása tisztán az ESP-NOW kommunikációra
fókuszálva (semmi periféria, semmi fogyasztás-optimalizálás, a mérés szimulált):
a node felébred, elküldi a "mérést", a master app-szinten visszaigazolja, a node alszik.
Több node-dal, felskálázható mintával (cél-flotta: ~12 node).

**RÖVIDÍTETT bench-ciklus: 12 s deep sleep** (±1 s jitter) — az éles (percek) ciklust
szimuláló bench a protokoll stabilizálása UTÁN jön.

## A protokoll

```
NODE (C3 SuperMini, deep sleep)                MASTER (S3-Zero, always-on)
──────────────────────────────                 ───────────────────────────
ébredés (timer, RTC-állapottal)
  │ pre-TX jitter (0..150 ms)                  recv-cb: csak queue-ba dob
  ├─ DATA ──────── broadcast ⟶                 consumer (core 1): CRC → NEW/DUP/gap
  │   (ha van tanult master: unicast)            │ osztályozás → statisztika
  │                                              └─ app-ACK unicast (DUP-ra IS!)
  │ ⟵──────────── ACK ─────────────┘
  │ ACK-ból master-MAC tanulás (RTC-be)
  │ nincs ACK? → retry (max 5, jitter,
  │   2 unicast-próba után broadcast-fallback)
  └─ "SZ6 CYCLE" log → deep sleep 12 s ±1 s
```

- **Megszólítás/átadás:** discovery = broadcast; a master ACK-jának feladójából a node
  megtanulja a master MAC-jét (RTC-ben őrzi, deep sleepet túléli) → utána **unicast**.
  Ha az unicast-próbák nem hoznak ACK-ot → broadcast-fallback + újratanulás (master-csere
  ellen). Az app-ACK a kézbesítés bizonyítéka (a MAC-ACK nem az).
- **seq/boot_id szemantika:** a `seq` RTC-ben, ciklusonként +1 (sikertelen ciklusnál IS)
  → a masternél a végleges seq-gap = elveszett mérés. A `boot_id` csak valódi
  power-on/brownout resetnél generálódik újra (`esp_reset_reason()` gating — az USB-reset
  és minden más reset túléli az RTC-t); boot_id-váltásból látja a master a node-resetet.
- **Ütközés-kezelés (skálázás):** pre-TX jitter + retry-jitter + alvásidő-jitter
  (fázis-dekorreláció) + app-retry. A retransmit-duplikátumot a master a seq-ből szűri,
  de ACK-olja (az első ACK elveszhetett).
- **Wire-formátum:** a befagyasztott `firmware/common/protocol.h` (`sz2_payload_t` /
  `sz2_ack_t`). Mező-újrahasznosítás az Sz6-ban: `packet_idx` = próba-sorszám (1..5),
  a master logolja, hányadik próba ment át.
- **Sz2-örökség:** callback-fegyelem (recv-cb csak queue-ba dob; TX szerializáció
  send-cb szemaforral; NO_MEM vár-újrapróbál), PHY-interop fix (explicit 11b/g/n LR
  nélkül + `esp_now_set_peer_rate_config` 11B/1M_L minden peerre), master `WIFI_PS_NONE`.

## Fájlok

```
node/     ESP32-C3 SuperMini firmware — env: node1 / node2 (-DSZ6_NODE_ID)
master/   ESP32-S3-Zero firmware — always-on RX + ACK + statisztika
tools/    serial_logger.py (resiliens háttér-logger)
          catch_bootloader.py + flash_catch.sh (alvó node flashelése)
logs/     futási logok (gitignored)
```

## USB-munkafolyamat (fejlesztés emberi beavatkozás nélkül)

- **Master:** always-on → stabil port, sima `pio run -t upload`; a log háttérből
  gyűjthető: `python tools/serial_logger.py /dev/cu.usbmodemXXXX logs/master.log`.
- **Node:** deep sleep alatt NEM enumerál → flash az awake-ablak elkapásával:
  `./tools/flash_catch.sh "$PWD/node" node1 /dev/cu.usbmodemXXXX` — a
  `catch_bootloader.py` a port felbukkanásakor ~20 ms-en belül beadja az esptool
  USBJTAGSerialReset szekvenciáját, a chip a ROM bootloaderben ragad, a flash nyugodtan
  lefut. (A sima pio-retry ~2-5 s hidegindítása lecsúszik az ~1-1,5 s-os ablakról.)
- **⚠️ USB_UART_CHIP_RESET:** a port MEGNYITÁSA a C3-on chip-resetet válthat ki
  (DTR/RTS átmenet) — ezért (1) a node az újrainitet `esp_reset_reason()`-nel gate-eli
  (az RTC-állapot túléli), (2) a node-oldali loggert csak indokolt esetben futtatjuk.

## Pass / fail (bench-kiértékelés a master "SZ6 STAT" soraiból)

| Feltétel | Pass |
|---|---|
| ≥50 ciklus / node | minden ciklus ACK-kal zárul (node: `result=OK`) |
| Adatvesztés | `miss=0` mindkét node-ra (nincs végleges seq-gap) |
| Duplikátum-kezelés | `dup` csak ACK-vesztésnél nő, a master szűri (nem új adat) |
| Ütközés-felépülés | egyidejű TX után retry-ból helyreáll (nincs elakadt node) |
| Master stabilitás | nincs lefagyás, `q_drop=0`, `ackfail` alacsony |

## Eredmények

*(kitöltés a futások után)*

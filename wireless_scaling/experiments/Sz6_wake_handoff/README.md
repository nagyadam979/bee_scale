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

## Eredmények (2026-07-05, padon) — ✅ PASS

**Zavartalan validációs ablak: ~1 h 54 m (06:35–08:29), 12 s-os ciklusokkal:**

| Metrika | node1 (C3 #1) | node2 (C3 #2) |
|---|---|---|
| Vett ciklus az ablakban | **480** | **482** |
| Végleges seq-gap (elveszett mérés) | **0** | **0** |
| Duplikátum / try>1 / out-of-order | 0 / 0 / 0 | 0 / 0 / 0 |
| RSSI a masternél | −27 dBm | −44 dBm |
| Kumulatív (reflash óta, ~2 h) | 494/495 (1 miss a tooling-átmenet percében) | **497/497** |

Master: ~2 h uptime, `q_drop=0`, `len_bad=0`, `ver_bad=0`, `ackfail=0` — a queue+consumer
pipeline terhelés alatt is veszteségmentes.

**Külön igazolt protokoll-utak:**
- **Discovery→átadás:** broadcast → ACK-ból master-MAC tanulás → unicast váltás (RTC-ben
  túléli az alvást); a ciklus rádiós munkája unicast üzemben **~110 ms/ébredés**.
- **ACK-vesztés/retransmit/dedup (élesben elkapva):** a node nem hallotta az ACK-ot →
  try=2,3,4 retransmit → a master `DUP`-ként szűrte és MINDET újra-ACK-olta → a node
  felépült, adat nem duplikálódott és nem veszett el.
- **Master-restart reziliencia:** master reboot (~35 s kiesés) alatt a node-ok saját logja
  korrekten `FAIL/dest=F`-et könyvelt (broadcast-fallback is lefutott), a master visszatérte
  után MINDKÉT node az ELSŐ próbára visszatalált (unicast, re-discovery nélkül); a kiesés
  alatti ciklusok seq-gapként detektálhatók (élesben az Sz1 ring buffer fedi majd).
- **Node-reset detektálás:** boot_id-váltás → a master seq-követése újraindul (`resets` számláló).

**A bench két nagy tanulsága (jövőbeli munkához KRITIKUS):**
1. **node2 RF-hiba → TX-power fix:** a 2. C3 SuperMini példány 21 dBm-en ~4%-os
   kézbesítést produkált (RX-e közben ép volt — sniffer-rel bizonyítva), mert az ismert
   SuperMini antenna-illesztési hiba miatt teljes poweren torzít a PA. **8,5 dBm-re
   (SZ6_TX_POWER=34) csökkentve 100%-os lett.** Példányfüggő! (node1 teljes poweren is
   hibátlan). Éles flottánál a TX-power példányonként kalibrálandó/ellenőrizendő.
2. **USB-Serial-JTAG chip-reset:** a port host-oldali megnyitása (DTR=0,RTS=1 átmenet)
   reseteli a C3/S3-at — firmware-oldali tiltás NINCS ezeken a chipeken. A reset-mentes
   nyitási recept a `tools/serial_logger.py`-ban (RTS-először elengedés); a node emellett
   `esp_reset_reason()`-nel gate-eli az állapot-nullázást (az RTC minden nem-power-on
   resetet túlél).

**Nyitott / következő lépések:**
- Éles-szimuláló bench (percek-órák ciklusidő, linger/log-ismétlés kikapcsolva) — a
  protokoll stabilizálva, ez már csak paraméter.
- Ütközés-stressz nagy flottával (12 node) — a jitter-dekorreláció 2 node-dal ütközésmentes
  volt; a retry-út külön igazolt, de a skálázási stressz-teszt még hátravan.
- A node1 egyszeri miss-e (seq=2, a flash-tooling percében) nem reprodukálódott — figyelendő.

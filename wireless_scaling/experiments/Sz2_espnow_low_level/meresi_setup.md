# Sz2 — Mérési setup (ESP-NOW alacsony szint, 2-board stressz-teszt)

Ez a doksi a **konkrét teszt-elrendezést** írja le: melyik board melyik szerep, hogyan
flasheld MINDKETTŐT, hogyan olvasd le az eredményt, és mi a PASS-feltétel. A firmware:
`node/main/sz2_node_tx.c` (TX) és `master/main/sz2_master_rx.c` (RX); a hangolható paraméterek
a fájlok tetején. Közös csomag-definíció: `common/sz2_protocol.h`. Eredmény:
`mereseredmenyek.csv`. Cél/pass-fail: `README.md` pass/fail tábla.

> **A teszt lényege:** a node (TX) elküld 1000 fix-méretű ESP-NOW broadcast csomagot, a
> master (RX) veszi, CRC-zi, és seq-statisztikát vezet. A PASS akkor áll, ha a master
> riportja: **received = 1000, missing = 0, dropped = 0, crc_error = 0, nincs lefagyás.**

---

## 1. Szerepek és boardok

| Szerep | Board | PlatformIO env | Konzol | Log háttérből |
|---|---|---|---|---|
| **Node (TX)** | ESP32-C3 SuperMini | `esp32-c3-supermini` | natív USB-Serial-JTAG | **NEM** (JTAG-CDC) — a user a saját termináljában nézi |
| **Master (RX)** | classic ESP-WROOM-32 | `esp32-wroom-32` | UART0 (USB-UART bridge) | **IGEN** — itt jelenik meg a `=== SZ2 RIPORT ===` |

> A node logja ezen a fejlesztőgépen háttérből nem olvasható (a C3 USB-Serial-JTAG-ja CDC,
> nem a klasszikus bridge). A node logot a felhasználó a saját termináljában nézi
> (`pio device monitor`). A teszt kiértékelése a **master** logján történik (az olvasható).

**Csatorna-egyezés (KRITIKUS):** mindkét firmware `SZ2_CHANNEL` paramétere a fájl tetején;
**alap 1**, és a kettőnek EGYEZNIE kell. Ha átírod, mindkettőt írd át és flashelj újra.

---

## 2. Flashelés — két külön USB-port

A két board két külön USB-porton lóg. A portot az `--upload-port`-tal add meg explicit
(különben a PIO az elsőként talált portot választaná). A portokat így listázd:

```
~/.platformio/penv/bin/pio device list
```

(macOS-en tipikusan `/dev/cu.usbserial-XXXX` vagy `/dev/cu.usbmodemXXXX` a master; a C3 node
a natív USB-n `/dev/cu.usbmodemXXXX`.)

### Master (RX) — előbb ezt, hogy várja a csomagokat

```
cd master
~/.platformio/penv/bin/pio run -t upload -t monitor --upload-port /dev/cu.<MASTER_PORT>
```

A monitor háttérből is olvasható. Indulás után a log:
`=== Sz2 MASTER (RX) ... ===` majd `Master kesz, varja a csomagokat...`.

### Node (TX) — utána

```
cd node
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.<NODE_PORT>
```

A node logját a felhasználó a SAJÁT termináljában nézheti:

```
~/.platformio/penv/bin/pio device monitor -p /dev/cu.<NODE_PORT> -b 115200
```

> **Ha a C3 node nem enumerál / nem flashelhető** (pl. egy korábbi firmware miatt): tartsd
> nyomva a **BOOT** gombot, nyomj **reset**-et (vagy USB ki/be), majd engedd el a BOOT-ot →
> download mód, és flashelj. (Ez a firmware ébren marad, nem alszik, így normál esetben végig
> enumerál — ez csak vész-eset.)

---

## 3. A teszt menete

1. **Master flash + monitor.** A master elindul, beáll a csatornára, power-save OFF
   (`WIFI_PS_NONE`), és várja a csomagokat (`Master kesz, varja a csomagokat...`).
2. **Node flash.** A node elindul, kiírja a saját MAC-jét és a `boot_id`-t, majd elkezdi
   küldeni a 1000 csomagot (`TX haladas: seq=...`). A node logot a user a saját termináljában
   láthatja (nem szükséges a kiértékeléshez).
3. **A node elküld 1000 csomagot** back-to-back (alap `SZ2_INTER_PACKET_DELAY_MS = 0`),
   egyesével szerializálva (send → send-cb → következő). NO_MEM esetén vár-újrapróbál.
4. **A master kiírja a végső összesítőt.** Amikor a master `received == total_packets`
   (1000), vagy `SZ2_IDLE_REPORT_MS` (2000 ms) ideig nincs új csomag → kinyomtatja a
   gép-grep-elhető riportot:

   ```
   === SZ2 RIPORT ===
   node_id=1 MAC=.. boot_id=0x........
   expected=1000 received=1000 missing=0 duplicate=0 out_of_order=0
   dropped_queue=0 crc_error=0
   elapsed_ms=... pkt_per_s=... rssi_min=.. rssi_avg=.. rssi_max=..
   PASS_FAIL=PASS
   === SZ2 RIPORT VEGE ===
   ```

5. **Olvasd le a master logból:** `received`, `missing`, `duplicate`, `out_of_order`,
   `dropped_queue`, `crc_error`, és a `PASS_FAIL` sort. Ezeket írd a `mereseredmenyek.csv`-be.

> A `PASS_FAIL` sort a firmware számolja (received == expected ÉS missing == 0 ÉS
> dropped == 0 ÉS crc_error == 0). A **lefagyás** (crash) hiányát abból látod, hogy a master
> végig logol és kiírja a riportot (nincs panic / reboot a logban).

---

## 4. PASS-feltétel (README pass/fail tábla)

| Feltétel | PASS |
|---|---|
| 1000 csomag padon | received = 1000, missing = 0 |
| Lefagyás | nincs (sem node, sem master nem panic-ol/reboot-ol) |
| Queue-túlcsordulás | dropped_queue = 0 |
| CRC | crc_error = 0 |
| RX callback | csak queue-ba dob (a kódban: `recv_cb` → `xQueueSend(...,0)`) |
| TX | nincs átfedő küldés (a kódban: `esp_now_send` → `xSemaphoreTake(s_tx_done)`) |
| `ESP_ERR_ESPNOW_NO_MEM` | kezelve (vár-újrapróbál, nem dob/pörög) |

---

## 5. Hibakeresés

1. **received = 0 (semmi nem jön).** → Csatorna-eltérés a leggyakoribb ok: ellenőrizd, hogy
   `SZ2_CHANNEL` MINDKÉT firmware-ben azonos. A node logból (user terminál) lásd, küld-e
   egyáltalán (`TX haladas`). Ellenőrizd, hogy a master `WIFI_PS_NONE`-ban van (a log indulásból).
2. **missing > 0, dropped = 0.** → Rádiós vesztés (MAC-szintű, broadcast nincs ACK). Tedd
   közelebb a boardokat, csökkentsd a forgalmat (`SZ2_INTER_PACKET_DELAY_MS` > 0 a node-on),
   vagy nézd az RSSI-t a riportban. A broadcast nem retransmittál — ez a tűzz-és-felejts
   korlátja (app-ack az Sz3).
3. **dropped_queue > 0.** → A consumer nem győzi üríteni a queue-t. Növeld `SZ2_RX_QUEUE_LEN`-t,
   vagy lassítsd a TX-et (`SZ2_INTER_PACKET_DELAY_MS`). A bench-en (1 node, core1 consumer)
   ennek 0-nak kell lennie.
4. **crc_error > 0.** → Bit-hiba a dróton, vagy struct-méret/offset eltérés a két target közt
   (nem várt — mindkettő little-endian, packed). Ellenőrizd, hogy mindkét firmware ugyanazt a
   `common/sz2_protocol.h`-t fordította (a build INCLUDE_DIRS-ből).
5. **node_id RESET sorok a logban.** → A node újraindult (új `boot_id`); ez nem hiba, csak a
   seq-tracking újraindul. Ha nem akartad, ne resetelgesd a node-ot a teszt alatt.

---

## 6. Hangolható paraméterek (mindkét firmware tetején)

**Node (`sz2_node_tx.c`):** `SZ2_CHANNEL`, `SZ2_NODE_ID`, `SZ2_TOTAL_PACKETS` (1000),
`SZ2_SEND_TIMEOUT_MS` (50), `SZ2_NO_MEM_RETRY_MS` (2), `SZ2_INTER_PACKET_DELAY_MS` (0),
`SZ2_PROGRESS_EVERY` (100), `SZ2_REPEAT` (false).

**Master (`sz2_master_rx.c`):** `SZ2_CHANNEL`, `SZ2_RX_QUEUE_LEN` (64), `SZ2_MAX_NODES` (4),
`SZ2_EXPECT_TOTAL` (1000), `SZ2_IDLE_REPORT_MS` (2000), `SZ2_PROGRESS_EVERY` (100),
`SZ2_MISSING_LIST_MAX` (32).

> Ha `SZ2_TOTAL_PACKETS`-t a node-on átírod, a master `SZ2_EXPECT_TOTAL`-t is igazítsd
> (a seq-bitmap felső korlátja); a master a payload `total_packets`-éből is tanulja a várt
> db-ot, de a bitmap-méret fordítási konstans.

---

## 7. Mérési eredmény (2026-06-28, padon) — PASS

ESP32-C3 SuperMini (node, `/dev/cu.usbmodem101`) + ESP-WROOM-32 (master, CP2102,
`/dev/cu.usbserial-0001`), ~30 cm, csatorna 1. Szennyezésmentes one-shot (friss master, a node
egyszer küld 1000-et):

| Metrika | Érték |
|---|---|
| received / expected | **1000 / 1000** |
| missing | **0** |
| duplicate | **0** |
| out_of_order | **0** |
| dropped_queue | **0** |
| crc_error | **0** |
| recv_cb / ok (DIAG) | 1000 / 1000 |
| elapsed / sebesség | 1138 ms / ~878 csomag/s |
| RSSI | min −39 / avg −36 / max −35 dBm |
| lefagyás | nincs |
| **PASS_FAIL** | **PASS** |

A node-log (a felhasználó termináljából): `elkuldve=1000, no_mem_retry=0, send_cb_timeout/egyeb=0,
tx_status_fail=0` — a TX-fegyelem hibátlan (egy in-flight küldés, NO_MEM nem fordult elő).

### A néma-bug és a fix (FONTOS tanulság)

**Tünet:** a master egészségesen elindult, de **0 csomagot vett**, miközben a node hibátlanul
küldött (a node-log szerint). A promiscuous sniffer (lásd lent) igazolta, hogy a node ESP-NOW
frame-jei **fizikailag megérkeztek** a masterhez (~880/s, −35 dBm), de a normál ESP-NOW RX-réteg
nem adta át őket a `recv_cb`-nek.

**Gyökérok:** **PHY-ráta / protokoll-mód mismatch a C3 (RISC-V) és a classic ESP32 (Xtensa) között.**
A két chip eltérő default PHY-módja miatt a vett frame-eket az ESP-NOW nem dolgozta fel (a frame
fizikailag jött, de nem a „közös nyelven").

**Fix (mindkét firmware `wifi_espnow_init`-jében, EGYEZŐEN):**
- `esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B|11G|11N)` — a LR (long-range) bit explicit
  kizárása (az LR-moduláció nem interoperábilis a sima 11b/g/n-nel → néma vétel).
- `esp_now_set_peer_rate_config(broadcast, {phymode=WIFI_PHY_MODE_11B, rate=WIFI_PHY_RATE_1M_L})`
  az `esp_now_add_peer` után — fix, legrobusztusabb közös ráta.

### Diagnosztikai eszközök (a final firmware-ben benne maradtak)

- **DIAG-számlálók a riportban:** `recv_cb` (hányszor adott át az esp_now), `ok` (feldolgozva),
  `len_bad / ver_bad / crc_bad` — a néma eldobási utak láthatóvá tétele. Egészséges one-shotban
  `recv_cb == ok == 1000`, a többi 0. (A számlálók GLOBÁLISAK → a tesztet **friss masterrel** indítsd.)
- **Promiscuous sniffer (opcionális build):** a master `SZ2_SNIFFER` flaggel promiscuous módban
  számolja az 1-es csatornán beérkező összes 802.11 frame-et, és külön az ESP-NOW vendor-action
  frame-eket (OUI 18:fe:34) + a src MAC-et és RSSI-t. Ezzel dönthető el, hogy a frame **fizikailag
  megérkezik-e** (radio/csatorna kontra ESP-NOW RX-réteg). Build:
  `PLATFORMIO_BUILD_FLAGS=-DSZ2_SNIFFER pio run -t upload --upload-port <MASTER_PORT>`.

### Eszköz-/környezeti korlát (rögzítve a jövőre)

- A **C3 node USB-Serial-JTAG logja a fejlesztőgépen háttérből NEM olvasható megbízhatóan**
  (reseteléskor a USB újra-enumerál; pyserial `dtr=False`-szal download módba is viheti). A node
  logot a felhasználó a **saját termináljában** nézi (`pio device monitor`). A master (CP2102,
  külső bridge) logja viszont háttérből is olvasható — a teszt kiértékelése ezen alapul.
- A `pio device monitor` headless (átirányított stdout) elhasal (`termios … Operation not
  supported`); a master automatizált olvasásához **pyserial** kell (a master CP2102-jén tiszta).

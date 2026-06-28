# Sz2 — ESP-NOW alacsony szint

**Típus:** Szoftver chunk · **Setup:** 1 (1 node + 1 master, padon) · **Irány:** (B) rádiós protokoll

## Cél
ESP-NOW send/recv **callback-fegyelem** kialakítása: a réteg, amire a teljes rádiós protokoll épül.
Cél a veszteség-/lefagyásmentes átvitel padon, a flotta zaja nélkül.

## Függőségek
- **Előfeltétel:** Sz0 (közös header) + Sz1 (stabil node buffer).
- **Mit blokkol:** Sz3 és minden további rádiós chunk előfeltétele.
- Kritikus adatút: … → Sz1 → **Sz2** → Sz3.

## Bemenet / előfeltételek
- 2 db ESP32 (szerep szerint node + master), bench, kontrollált rádiós környezet.
- Sz0 header. WiFi/ESP-NOW **azonos csatorna** beállítva (master STA + ESP-NOW egy csatornán — kész/tesztelt elv).

## Megvalósítás vázlata (NEM kód)
- **RX-callback fegyelem:** a receive callbackben **csak queue-ba dob** (`xQueueSendFromISR`), semmi más.
- **TX-fegyelem:** két küldés közt **ne induljon új küldés a send-callback visszatérése előtt**.
- **Flow control:** `ESP_ERR_ESPNOW_NO_MEM` → várni-újrapróbálni (nem eldobni, nem pörgetni).
- Stressz-teszt: 1000 csomag folyamatos küldése/vétele padon.

## Kimenet / deliverable
- 1000 csomag **veszteség / lefagyás nélkül** padon, fegyelmezett callback-kezeléssel.

## Pass / fail
| Feltétel | Pass |
|---|---|
| 1000 csomag padon | 0 elveszett, 0 lefagyás |
| RX callback | csak queue-ba dob, nincs nehéz munka ISR-kontextusban |
| TX | nincs átfedő küldés a send-callback előtt |
| `ESP_ERR_ESPNOW_NO_MEM` | kezelve (vár-újrapróbál) |

## Nyitott kérdések / kockázatok
- Queue mélység (vételi burst pufferelése) — méretezés a forgalomhoz.
- Csatorna-koegzisztencia a 4G stick 2.4 GHz hotspotjával (feltöltés alatt ESP-NOW szünetel — Sz5).
- Titkosítás (PMK/LMK AES-128) opcionális — most kihagyható, később bekapcsolható.

## Hivatkozások
- `../../docs/new_base_components.md` (Node→Master ESP-NOW), `../../MEMORY.md` (7. — vételi pipeline).
- Függ: [`../Sz0_wire_protocol/`](../Sz0_wire_protocol/), [`../Sz1_node_sampling_ring_buffer/`](../Sz1_node_sampling_ring_buffer/).

---

## Implementáció

Két külön ESP-IDF projekt (PlatformIO-val fordítva, mint a H2), egy közös headerrel:

```
common/sz2_protocol.h     közös packed wire-formátum (sz2_payload_t, sz2_ack_t, CRC16) — mindkét firmware includolja
node/                     TX projekt — ESP32-C3 SuperMini (board esp32-c3-devkitm-1), konzol USB-Serial-JTAG
  main/sz2_node_tx.c      a node: 1000 broadcast csomagot küld, szerializált send-fegyelemmel
master/                   RX projekt — classic ESP-WROOM-32 (board esp32dev), konzol UART0 (háttérből olvasható)
  main/sz2_master_rx.c    a master: veszi+CRC-zi a csomagokat, seq-statisztikát vezet, "=== SZ2 RIPORT ===" összesítőt nyomtat
meresi_setup.md           2-board flashelés + a teszt menete + PASS-feltétel
mereseredmenyek.csv       eredmény-skeleton (a user tölti a teszt napján)
```

**Közös header (`common/sz2_protocol.h`):** fixed-size, `__attribute__((packed))`
`sz2_payload_t` (node→master, 41 byte) a docs adat-mezőivel (node_id/weight/temp/vbat) +
stressz-mezők (boot_id, seq, packet_idx, total_packets, send_timestamp_ms) + CRC16-CCITT.
Az `sz2_ack_t` (20 byte) az Sz0-csíra a későbbi app-ACK-hez (Sz3) — definiálva, de az Sz2-ben
NEM használt. Mindkét cél little-endian, azonos int-szélesség → bit-azonos a dróton.

**Callback-fegyelem (a chunk lényege) a kódban:**
- **RX (recv) — csak queue-ba dob:** `sz2_master_rx.c : recv_cb()` → fix struct kimásolás
  (payload érték szerint + src MAC + RSSI, mind csak a cb-ben érvényes) → `xQueueSend(q,&item,0)`
  (non-blocking, **NEM** `*FromISR` — a cb WiFi-task kontextusban fut). A nehéz munka (CRC,
  statisztika, log) a `consumer_task()`-ban (core 1-re pinnelve) megy.
- **TX szerializáció — egy in-flight küldés:** `sz2_node_tx.c : tx_task()` → `esp_now_send()`
  majd `xSemaphoreTake(s_tx_done, ...)`; a `send_cb()` csak `xSemaphoreGive(s_tx_done)`.
  A send-cb **verzió-guarddal** (`ESP_IDF_VERSION >= 5.4.0` → `esp_now_send_info_t`).
- **NO_MEM kezelés:** `tx_task()` belső `for(;;)` — `ESP_ERR_ESPNOW_NO_MEM` → `vTaskDelay` +
  retry ugyanarra a seq-re (nem dob, nem pörög).
- **Master power-save OFF:** `wifi_espnow_init()` → `esp_wifi_set_ps(WIFI_PS_NONE)` (kritikus a
  folyamatos vételhez).

## Teszt menete (összefoglaló)

Részletesen: `meresi_setup.md`. Röviden: 1) master flash+monitor (várja a csomagokat,
log háttérből olvasható) → 2) node flash → 3) a node elküld 1000 csomagot →
4) a master kiírja a `=== SZ2 RIPORT ===` blokkot (received/missing/duplicate/dropped/crc_error
+ `PASS_FAIL` sor). **PASS:** received = 1000, missing = 0, dropped = 0, crc_error = 0, nincs
lefagyás. A node és master `SZ2_CHANNEL`-jének EGYEZNIE kell (alap 1).

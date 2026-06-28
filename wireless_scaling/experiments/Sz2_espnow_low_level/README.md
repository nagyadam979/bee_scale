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

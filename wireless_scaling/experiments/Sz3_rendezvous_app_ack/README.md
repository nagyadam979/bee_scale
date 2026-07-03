# Sz3 — Rendezvous + app-ACK

**Típus:** Szoftver chunk · **Setup:** 1 (1 node + 1 master, padon) · **Irány:** (B) rádiós protokoll

## ⚠️ NAGYRÉSZT TÁRGYTALAN (2026-07-03)
A master **hálózati tápon always-on** → mindig figyel, így a **rendezvous/slot-logika felesleges**
(a node tűzz-és-felejts broadcastja mindig megérkezik; a hiánydetektálás `seq`-gapből megy). Ami
esetleg megmaradhat: az **app-szintű ACK** csak akkor, ha garantált kézbesítés kell — de kutatási
idősornál a `seq`-gap kezelhető. Az alábbi leírás a korábbi duty-cycle-es master-modellhez készült.

## Cél
A protokoll-mag: **slot-logika, HELLO/batch, app-szintű ACK, `seq`-duplikátumszűrés, ACK-vezérelt
buffer-törlés** (`clear_buffer_until_seq`). A MAC-szintű ACK ≠ alkalmazás-szintű kézbesítés — itt
épül a megbízható átvitel.

## Függőségek
- **Előfeltétel:** Sz2 (fegyelmezett ESP-NOW réteg).
- **Mit blokkol:** Sz4 (fáziskorrekció), H3 (a master duty-cycle a működő RX-ablakból jön).
- Kritikus adatút utolsó eleme: … → Sz2 → **Sz3**.

## Bemenet / előfeltételek
- Sz2 kész (1000 csomag padon OK), Sz0 header (ACK-mezők: `ack_last_seq`, `clear_buffer_until_seq`).
- Sz1 node ring buffer (a törlés ezt célozza).

## Megvalósítás vázlata (NEM kód)
1. **Rendezvous / slot:** a node az ablakában ébred, HELLO → batch küldés.
2. **App-ACK:** a master visszaigazol (`ack_last_seq`); a node timeoutra retransmittál.
3. **Dup-szűrés:** a master a `seq` alapján kiszűri a duplikátumot és detektálja a hiányt.
4. **Buffer-clear:** a node a bufferjéből **pontosan az ACK-elt `seq`-ig** töröl (`clear_buffer_until_seq`).
5. Negatív teszt: ACK kiesésnél semmi se vesszen el (a nem-ACK-elt minta marad a bufferben).

## Kimenet / deliverable
- A node bufferje **pontosan az ACK-elt mintákig ürül**; ACK-hiánynál semmi se vész el.

## Pass / fail
| Feltétel | Pass |
|---|---|
| Buffer-törlés | pontosan `clear_buffer_until_seq`-ig, se több, se kevesebb |
| ACK-hiány | nem-ACK-elt minta megmarad, retransmit után átmegy |
| Duplikátum | master kiszűri (`seq`), nincs dupla felvétel |
| Hiány-detektálás | `seq`-gap észlelve |

## Nyitott kérdések / kockázatok
- **ESP-NOW kézbesítés stratégia:** app-ack (több ébrenlét, garantáltabb) vs. tűzz-és-felejts +
  `seq`-gap (kevesebb node-fogyasztás). Kutatási idősornál a gap kezelhető → eldöntendő (MEMORY.md #4).
- Slot/timeout paraméterek előzetesek; a végleges drift-tűrés Sz4 + M1 után jön.

## Hivatkozások
- `../../docs/new_base_components.md` (ESP-NOW app-ack, MQTT batch), `../../MEMORY.md` (7. nyitott kérdés #4).
- Függ: [`../Sz2_espnow_low_level/`](../Sz2_espnow_low_level/). Blokkolja: [`../Sz4_phase_correction_recovery/`](../Sz4_phase_correction_recovery/), [`../H3_master_power_duty_cycle/`](../H3_master_power_duty_cycle/).

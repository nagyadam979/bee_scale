# Sz0 — Közös wire protocol / header

**Típus:** Szoftver chunk · **Setup:** Setup 1 előtt (bench, eszközfüggetlen is tesztelhető) · **Irány:** (B) rádiós protokoll

## Cél
Verziózott, **közös packet/header formátum**, amit a node és a master is használ. Ez a **chunkok
közti szerződés** — ha stabil, Sz2/Sz3/Sz4 egymástól függetlenül cserélhető/javítható.

## Függőségek
- **Előfeltétel:** Sz1-gyel párhuzamosan készülhet, de **minden rádiós chunk (Sz2/Sz3/Sz4/Sz5) előfeltétele.**
- Kritikus adatút: H1 → H2 → **Sz0** → Sz1 → Sz2 → Sz3.

## Bemenet / előfeltételek
- A rendszerterv adatformátuma (`payload_t`, `packed`) — `../../docs/new_base_components.md`.
- Nincs HW-előfeltétel; encode/decode dummy payloadon, akár hoston is tesztelhető.

## Megvalósítás vázlata (NEM kód)
- Fixed-size, `__attribute__((packed))` struktúra; nincs parse/string. Node és master ugyanazt.
- **Mezők — payload (node → master):**
  `node_id`, `packet_type`, `protocol_version`, `seq`, `sample_age_s` (vagy `sample_period_s`), + payload mezők (`weight`, `temp`, `vbat`).
- **Mezők — ACK (master → node):**
  `ack_last_seq`, `clear_buffer_until_seq`, `next_slot_offset_s`, `clock_correction_s`, opcionálisan `retry_policy`.
- **Verziózás:** `protocol_version` kötelező → előre/visszafelé kompatibilitás kezelhető.
- Encode/decode teszt: node és master **ugyanazt a struktúrát értelmezi** dummy payloadon.

## Kimenet / deliverable
- Stabil, verziózott közös header + encode/decode oda-vissza teszt (round-trip).

## Pass / fail
| Feltétel | Pass |
|---|---|
| Encode → decode dummy payloadon | bit-azonos round-trip |
| Node és master értelmezés | ugyanaz a struktúra mindkét oldalon |
| Méret | fixed-size, `packed`, nincs string/parse |

## Nyitott kérdések / kockázatok
- `sample_age_s` vs. `sample_period_s` — melyik metódus a mintaidő-jelölésre (mérés dönt Sz4/M1-nél).
- `retry_policy` mezőtartalom — Sz3 ack-stratégiától függ (app-ack vs. tűzz-és-felejts).
- Endianness / aligálás két ESP közt (azonos arch → várhatóan nem gond, de tesztelni kell).

## Hivatkozások
- `../../docs/new_base_components.md` (Adatformátum tábla, protokollok), `../../MEMORY.md` (7. szakasz).
- Fogyasztói: [`../Sz2_espnow_low_level/`](../Sz2_espnow_low_level/), [`../Sz3_rendezvous_app_ack/`](../Sz3_rendezvous_app_ack/), [`../Sz4_phase_correction_recovery/`](../Sz4_phase_correction_recovery/).

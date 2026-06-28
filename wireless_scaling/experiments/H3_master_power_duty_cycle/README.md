# H3 — Master táp-pack + duty-cycle HW

**Típus:** Hardver chunk · **Setup:** 2 / 2.5 / 3 (valós forgalom kell hozzá) · **Irány:** (A) elektromos/energetikai

## Cél
A master 18650 táp-packjének kimérése és a **2 órás ciklusra számolt átlagáramának**
meghatározása **valós forgalommal**. A ~16–20 mA csak az RX duty-cycle ideális komponense
(24–30 perc aktív / 120 perc, ~80 mA RX); erre rájön a boot, Wi-Fi init, ESP-NOW init, logolás,
DC/DC veszteség, esetleges 4G uplink és retry-k.

## Függőségek
- **Előfeltétel:** működő RX-ablak (**Sz3**), mert a duty-cycle a tényleges vételi ablakhosszból jön.
  A 4G uplink (Sz5) torzíthatja az átlagot → külön is mérendő.
- Setup 2/2.5/3 alatt fut (valós forgalom kell, nem bench-szimuláció).

## Bemenet / előfeltételek
- ESP32-S3-Zero master + 18650 táp-pack + DC/DC (buck-boost) a master tápútján.
- Áram-/energiamérő a teljes ciklusra (átlagáram-integrálás).
- Működő RX-ablak-firmware (Sz3) és opcionálisan a 4G uplink (Sz5).

## Megvalósítás vázlata (NEM kód)
1. Teljes 2h ciklus átlagáramának mérése valós forgalommal (RX + minden overhead).
2. **4G uplink külön mérése** (ki/be), hogy az átlagra gyakorolt hatása elkülönüljön.
3. Komponensek bontása: RX duty-cycle ideális rész vs. boot / Wi-Fi / ESP-NOW init / logolás / DC-DC.
4. Pack-méretezés / élettartam visszaszámolása az átlagáramból.

## Kimenet / deliverable
- 18650 pack mérve; 2h-ciklus **átlagáram valós forgalommal**, a 4G uplink hozzájárulása külön.

## Pass / fail
| Master 2h ciklus átlagáram (RX + overhead) | Jó | Elfogadható | Újranézni |
|---|---|---|---|
| Teljes | **< 15 mA** | < 25 mA | **> 30 mA** → RX-ablak hossz, táp és/vagy uplink |

## Nyitott kérdések / kockázatok
- **Master ESP nyugalmi fogyasztása dominálja a táp-mérleget** akkus esetben (mennyit alszik vs.
  mennyit figyel ESP-NOW-ra) — végigszámolandó (MEMORY.md 7. nyitott kérdés #2).
- MF79U 4G stick boot-tüskéi és tápzaja (külön táprail / 1000 µF puffer) — lásd Sz5 / rendszerterv.
- Master táp **két üzemmód** (folyamatos vs. akku/napelem) — a HW a szigorúbbra (akkus) tervezve.

## Hivatkozások
- `../../docs/new_base_components.md` (Master BOM, uplink, master táp kétféle üzemmód).
- Függ: [`../Sz3_rendezvous_app_ack/`](../Sz3_rendezvous_app_ack/), kapcsolódik: [`../Sz5_master_time_uplink/`](../Sz5_master_time_uplink/).

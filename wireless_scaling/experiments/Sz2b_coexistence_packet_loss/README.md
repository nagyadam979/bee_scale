# Sz2b — Koegzisztencia + csomagvesztés-kivizsgálás

**Típus:** Szoftver chunk · **Setup:** 1 → 2.5 (1 node + master padon, majd 3 node) · **Irány:** (B) rádiós protokoll

## Cél
Két, az Sz2 után maradt kockázat lezárása a **végleges hardveren** (ESP32-S3-Zero master + ESP32-C3 node):
1. **A csomagvesztés kivizsgálása** — az M1 futásában látott **87,9%-os veszteség** (88/726 cycle, erős
   −30 dBm RSSI mellett, többperces RF/ESP-NOW-RX szintű blackout-ok) gyökérokának felderítése. Az M1 az
   ideiglenes, worst-case setupban futott (C3 ↔ classic-ESP32, padra kötött pin, másik szoba) — itt a
   véglegesen kell reprodukálni/cáfolni.
2. **A D1 egyidejű üzem validálása** — a master WiFi STA-ként a 4G stick hotspotjára kapcsolódva **közben
   ESP-NOW-t is vesz** (azonos csatorna). Kérdés: stabil-e a folyamatos vétel a STA-kapcsolat + esetleges
   feltöltés-forgalom alatt, vagy csomagokat ejt.
3. **A redundancia méretezése** — az M1 fix 3× broadcast/ébredés helyett a **3×→N×** ismétlés hatásának
   mérése a tényleges kézbesítési arányra.

## Függőségek
- **Előfeltétel:** Sz2 (fegyelmezett ESP-NOW réteg — kész, 1000/1000 padon).
- **Végleges HW** kell hozzá (S3-Zero master + C3 node) — az M1 anomália nem az M1 scope-ja volt.
- **Kapcsolódik:** Sz5 (a stick-forgalom a koegzisztencia egyik terhelése), MEMORY.md 7. (D1, D2).

## Bemenet / előfeltételek
- ESP32-S3-Zero master (always-on, PS_NONE) + ESP32-C3 node (deep-sleep beacon, mint az M1-ben).
- Sz0 header + az Sz2 PHY-fix (`set_protocol` LR nélkül + `esp_now_set_peer_rate_config` 11b/1M_L).
- 4G stick (ZTE MF79U) a STA-kapcsolathoz — a koegzisztencia-terhelés méréséhez.
- RSSI/counter-logolás a masteren (vett/hiányzó cycle# a `seq`-ből, mint az M1 abszolút cycle#-je).

## Megvalósítás vázlata (NEM kód)
- **Baseline:** node tűzz-és-felejts broadcast (30 s, abszolút cycle#), master folyamatos RX — kézbesítési
  arány és a blackout-ok jellemzése a végleges HW-n, RSSI mellett (reprodukálja-e az M1 87,9%-át?).
- **Változó izolálása:** külön mérés (a) csak ESP-NOW (STA le), (b) STA fel, tétlen stick, (c) STA + aktív
  feltöltés → melyik lépés ejt csomagot. Ez dönti el a D1-et (egyidejű üzem áll-e, vagy fallback ablakos).
- **Redundancia-sweep:** 1× / 3× / N× ismétlés/ébredés → kézbesítési arány mint N függvénye; a legkisebb N,
  ami a célt hozza.
- **Gyanúsítottak:** csatorna-eltérés/-vándorlás, STA power-save, RX-queue telítődés, 2.4 GHz önzavar a stick
  hotspotjától, antenna/RF — a diagnosztika (DIAG-számlálók, promiscuous sniffer) az Sz2-ből átvéve.

## Kimenet / deliverable
- A csomagvesztés **gyökéroka** (vagy igazolt cáfolat, hogy a végleges HW-n nem jelentkezik).
- **D1 döntés lezárva:** egyidejű WiFi STA + ESP-NOW üzem tartható-e, vagy ablakos fallback kell.
- A **redundancia (N×)** méretezett értéke a célzott kézbesítési arányhoz.

## Pass / fail *(javaslat — a mérés napján véglegesítendő)*
| Feltétel | Pass (javasolt) |
|---|---|
| Kézbesítési arány padon, 3× redundanciával | **≥ 99%** |
| D1 egyidejű üzem | folyamatos RX csomagvesztés-emelkedés nélkül STA + feltöltés alatt (különben: fallback ablakos) |
| Csomagvesztés gyökéroka | azonosítva vagy a végleges HW-n cáfolva |

## Nyitott kérdések / kockázatok
- Nagy flotta (12 node) egyidejű broadcast-**ütközése** — Setup 3-ban skálázandó (jitter/redundancia).
- Ha a D1 elbukik: az ablakos fallback késleltetést visz (a `seq`+PSRAM elnyeli, adatvesztés nélkül).
- A stick 2.4 GHz hotspotjának önzavara külső antennával (TS-9) csökkenthető — lásd `new_base_components.md`.

## Hivatkozások
- `../../MEMORY.md` (7. — D1/D2, ESP-NOW kézbesítés + koegzisztencia; 8./M1 — a 87,9% anomália).
- Épít: [`../Sz2_espnow_low_level/`](../Sz2_espnow_low_level/) (PHY-fix, callback-fegyelem, diagnosztika).
- Kapcsolódik: [`../Sz5_master_time_uplink/`](../Sz5_master_time_uplink/) (a stick-forgalom mint koegzisztencia-terhelés).

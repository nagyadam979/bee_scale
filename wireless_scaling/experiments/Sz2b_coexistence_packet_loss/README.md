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
- Nagyobb flotta (több node) egyidejű broadcast-**ütközése** — Setup 3-ban skálázandó (jitter/redundancia).
- Ha a D1 elbukik: az ablakos fallback késleltetést visz (a `seq`+PSRAM elnyeli, adatvesztés nélkül).
- A stick 2.4 GHz hotspotjának önzavara külső antennával (TS-9) csökkenthető — lásd `new_base_components.md`.

## Hivatkozások
- `../../MEMORY.md` (7. — D1/D2, ESP-NOW kézbesítés + koegzisztencia; 8./M1 — a 87,9% anomália).
- Épít: [`../Sz2_espnow_low_level/`](../Sz2_espnow_low_level/) (PHY-fix, callback-fegyelem, diagnosztika).
- Kapcsolódik: [`../Sz5_master_time_uplink/`](../Sz5_master_time_uplink/) (a stick-forgalom mint koegzisztencia-terhelés).

---

# EREDMÉNYEK (2026-07-05, padon) — ✅ D1 VALIDÁLVA

> Az eredeti spec a 4G stick hotspotjával számolt; a bench **teszt-AP-val** futott (dlink,
> WPA2, csatornája előre nem ismert — pont ez adta a chunk kulcsproblémáját). A stick-kel
> való megismétlés az Sz5 scope-ja. A kód az **Sz6 PASS-forrás adaptációja** (protokoll
> változatlan), az új réteg a node-oldali **csatorna-scan discovery** + a master **STA-réteg**.

## Setup

- **Master:** ESP32-S3-Zero (always-on, USB-tápon), STA a dlink-EDA4 AP-n + ESP-NOW
  egyszerre; consumer core 1-en, WiFi/recv-cb core 0-n. Env-ek: `p1-espnow-only` /
  `p2-sta-idle` / `p3-sta-traffic` (SZ2B_WIFI_MODE=0/1/2). Hitelesítők: `creds.ini`
  (gitignored, minta: `creds.ini.example`).
- **Node-ok:** 2× ESP32-C3 SuperMini, 12 s deep-sleep ciklus (Sz6 bench-profil),
  **mindkettő SZ2B_TX_POWER=34 (8,5 dBm)** — lásd a 2. tanulságot.
- **Az AP a 8-as csatornára tette a mastert** — a node-ok scan-nel találták meg.
- Kiértékelés: `tools/analyze_sz2b.py` a master logján (ground truth).

## A csatorna-követés (az implementált kulcsmegoldás)

- Master STA-üzemben **nincs `esp_wifi_set_channel`** (törné az asszociációt); minden
  ESP-NOW peer `channel=0` (aktuális csatorna). A tényleges csatornát a WIFI-események
  és az MSTAT logolják.
- Node: a talált csatorna **RTC-cache-ben** (deep sleepet túléli); ha nincs cache, VAGY
  2 egymást követő ciklus ACK nélkül zárul → **1..13 scan** még ugyanabban a ciklusban
  (csatornánként broadcast DATA + 80 ms ACK-várás, max 3 sweep). A sweep az első ACK-nál
  NEM áll meg: a **legerősebb ACK-RSSI-jű csatornát** cache-eli (bleed-védelem, 3. tanulság).

## Fázisok és mért számok

| Fázis | Setup | Eredmény |
|---|---|---|
| 1. Baseline (15 p) | ESP-NOW only, fix ch1 | ✅ node1: 63, node2: 64 ciklus — 0 gap, 0 dup, 100% try=1 |
| 2. STA idle (15 p) | STA-connect + ESP-NOW | ✅ átállás ch8-ra scan-nel; node2 discovery: `seq=1 try=8` (ch1..7 üres, ch8 talált); élesben elkapott WiFi-kiesés + 2,2 s reconnect alatt is 0 gap |
| 3. Koegzisztencia (66 p soak) | STA + 5 s HTTP GET | WiFi/forgalom ✅ (0 disconnect, 748/748 GET, q_drop=0); node1 ⚠️ dup=344, try>1=22% → gyökérok-vadászat (2. tanulság), node2 referencia 0 hibával |
| 4. Master-reboot | usb_reset a masterre | ✅ ~4 s reboot, STA +1,1 s / IP +4,5 s; mindkét node ELSŐ próbára vissza (cache-elt ch8+MAC); 0 ciklusveszteség (reboot < 12 s alvásperiódus) |
| 5. ZÁRÓ SOAK (66 p, javított fw) | STA + forgalom | ✅ **node1: 276, node2: 278 ciklus — 0 gap, 0 dup, 100% try=1**; WiFi 0 disconnect; GET 791/796 = 99,4%; q_drop=0 — node1 átlag **−84 dBm-en**! |
| + Kiterjesztett futás (4,6 h, estig) | ugyanaz | ✅ node1: 1162, node2: 1172 ciklus — **0 gap, 0 dup**; 0 disconnect; GET 3284/3293 = 99,7% |

## PASS/FAIL (a záró soak + kiterjesztett futás alapján)

| Kritérium | Cél | Mért | Ítélet |
|---|---|---|---|
| Végleges seq-gap (mindkét node) | 0 | 0 (5,7 h alatt összesen) | ✅ |
| try>1 arány | < 5% | 0,0% | ✅ |
| q_drop | 0 | 0 | ✅ |
| WiFi asszociáció | folyamatos, reconnectek logolva | 0 disconnect a záró futásban; a P2-beli kiesés 2,2 s alatt felépült | ✅ |
| WiFi-forgalom sikerarány | > 95% | 99,4–99,7% | ✅ |

**D1 DÖNTÉS: az egyidejű WiFi STA + ESP-NOW üzem STABIL — nincs szükség ablakos fallbackre.**
Az M1-ben látott 87,9%-os veszteség a végleges HW-n + fegyelmezett rétegen nem reprodukálódott;
a bench során talált veszteségek gyökéroka RF-szintű volt (lásd lent), nem koegzisztencia.

## Tanulságok (jövőbeli munkához KRITIKUS)

1. **Csatorna-scan discovery működik és kell.** Az AP csatornája (itt: 8) előre nem
   ismert és nem is stabil garantáltan → a fix csatornás node-minta STA-master mellett
   halott. A scan+RTC-cache ára: egyszeri ~1 s discovery power-on után; utána a ciklus
   rádiós munkája **21–143 ms** (unicast, try=1).
2. **SuperMini TX-power TÍPUSHIBA, nem példányhiba.** Az Sz6-ban a node2, itt a node1
   bukott meg teljes poweren (21 dBm): ~1 m-ről, valós (nem 10 cm-es) távolságon a master
   a ch8-as küldéseit nem vette, miközben MAC-szinten ACK-olta (!). 8,5 dBm-re véve
   (SZ2B_TX_POWER=34) azonnal 100%-os lett, és −84..−90 dBm-ig hibátlan maradt.
   **Alapértelmezés mostantól: 8,5 dBm minden SuperMini node-on**; emelés csak mérés után.
3. **Szomszéd-csatorna bleed félretanítja a naiv scant.** Erős jelnél a master ch8-as
   ACK-ja a ch7/ch9-en is dekódolható → az első-ACK-ra megálló scan rossz csatornát
   cache-elt (élesben elkapva: „scan OK: csatorna=7") → oszcilláció (dup-vihar / minden
   2. ciklus elveszett). Fix: a teljes sweep kiértékelése, **max-RSSI csatorna** nyer.
4. **S3-Zero antenna: a link-budget gyanú MEGDŐLT (2026-07-06, `rssi_probe`).** A gyanút
   (~30–40 dB deficit, külső antennás master-board szükségessége) egy egyszerűbb, a két-ESP32
   bizonytalanságot kizáró méréssel cáfoltuk: mindkét board (S3-Zero és C3 SuperMini) STA-ként
   UGYANAHHOZ a referencia-routerhez (dlink-EDA4) csatlakozott, és az AP-RSSI-t a
   távolság függvényében mértük (`rssi_probe` firmware, telnet/UDP-riport). Eredmény:
   - **S3-Zero:** a routeren (0 m) −66, 2–4 m-en **−44…−45**, 5–6 m-en −53 dBm; disc=0 végig.
   - **C3 SuperMini:** a routeron (0 m) −74, közeltéri csúcs **−31…−34**, 5–6 m-en −55…−65; disc=0.
   - Mindkét board ugyanabban a nagyságrendben mozog; **nincs 30–40 dB deficit sehol**, a
     C3 csúcsértéke még erősebb is. A pozíció/orientáció/multipath dominál, nem az antenna.
   - Kulcsmegfigyelés: **0 m-en (a router házára téve) MINDKÉT board gyengébb**, mint 2–4 m-en
     — közeli-tér árnyékolás/elhangolás, nem antenna-hiba. Egy valóban rossz antenna monoton
     mélyen gyenge lenne, nem javulna 20 dB-t pár méter távolodásra.
   - **Következmény:** a P3-ban a munkaasztalon látott −82 dBm a router NAGY TÁVOLSÁGA + falak
     miatt volt, nem az S3 antennájáé. A `rssi_probe` a board VÉTELÉT méri (AP-beacon → RX-lánc),
     ami ugyanaz a lánc, amivel az ESP-NOW-ot venné → ha ez egészséges, a P3-as node→master
     gyengeségnek sem az S3 RX-antennája az oka. A valódi ludas a **SuperMini node TX-torzítás**
     volt teljes poweren (lásd 2. pont, `TX_POWER=34` fixálta). **Külső antennás master-board
     RF-okból NEM indokolt** — a döntés más szempontokra (fizikai elrendezés, ház) hárul.
5. **node2 USB-enumeráció instabil (kontakt).** Rádión hibátlan, de az USB-portja kétszer
   is eltűnt (replug elsőre megjavította). Fejlesztésnél számolni kell vele; a boardot
   érdemes lehet cserélni/újraforrasztani.
6. **A master STA-boot átmenete önjavító.** Boot után a master a default ch1-en ül, amíg
   az asszociáció (1–5 s) át nem viszi az AP csatornájára — az ebben az ablakban ACK-ot
   kapó node átmenetileg rossz csatornát tanulhat, de a streak→rescan út magától
   helyrehozza (P2-ben megfigyelve, beavatkozás nélkül felépült).

## Nyitott / következő lépések

- ~~C3↔C3 sniffer-kontrollmérés az S3-Zero antenna számszerűsítésére~~ → **ELVÉGEZVE**
  másképp: `rssi_probe` (board↔referencia-router RSSI-görbe), lásd a 4. tanulságot. Az
  antenna-gyanú megdőlt, külső antennás master RF-okból nem indokolt.
- Megismétlés a **ZTE MF79U stick hotspotjával** (Sz5): DHCP/NAT + a stick 2,4 GHz
  önzavara az igazi célkörnyezet.
- **Redundancia-sweep (1×/3×/N×)** — az eredeti spec 3. célja; az app-ACK+retry mellett
  a tűzz-és-felejts redundancia-méretezés kérdése maradt nyitva (élesben az ACK-os út megy).
- Nagyobb flotta ütközés-stressz (a végleges node-szám egyelőre nyitott) — Sz6 óta változatlanul hátravan.
- AP-csatornaváltás menet közbeni tesztje (a scan-út elvben fedi; explicit teszt nem volt).

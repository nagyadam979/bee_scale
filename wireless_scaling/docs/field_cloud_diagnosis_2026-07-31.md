# Field Cloud Diagnosis — 2026-07-31

> **Utóirat (2026-10-06):** ez a pillanatkép elavult. A hive2 master 2026-08-01 07:46-kor újraindult és helyreállt (a node_1 adatai a lyuk alatt is befutottak); a hive1 is visszajött, majd 2026-09-20-án ismét leállt. Az aktuális terepi állapot: `MEMORY.md` 11. szakasz.

## Mit ellenoriztem

- A live Firebase RTDB olvasasat anonim auth-tal.
- A dedikalt device-user (`esp32@beescale.local`) Firebase Auth belepeset.
- A repo szerinti hive1/hive2 path-okat es a wireless master cloud schemajat.

## Tenyek

- A Firebase projekt elerheto, az RTDB olvashato, az anonim webes read mukodik.
- A device-user auth jelenleg mukodik, tehat nincs altalanos Firebase jelszo/auth kieses.
- A wireless master terepi buildje hive2 schema-ba ir:
  - `hives/hive2/boards/<board>/readings`
  - `hives/hive2/env/readings`
- Ez egyezik a dashboard configgal es a dokumentalt rules-szal.

## Live RTDB utolso adatok

### Prototype / hive1

- `hive1/rods/w1/readings` utolso rekord: `2026-07-24T06:58:51Z`
- `hive1/rods/w2/readings` utolso rekord: `2026-07-24T06:58:44Z`
- `hive1/env/readings` utolso rekord: `2026-07-24T06:58:44Z`
- `hive1/temp/readings` utolso rekord: `2026-07-24T06:58:51Z`

Kovetkeztetes: a prototype teljesen, egyszerre allt meg 2026-07-24-en. Ez nem dashboard-path hiba.

### Wireless / hive2

- `hive2/boards/master/readings` utolso rekord: `2026-07-30T09:14:39Z`
- `hive2/env/readings` utolso rekord: `2026-07-30T09:14:39Z`
- `hive2/boards/node_1/readings` utolso rekord: `2026-07-30T09:06:59Z`
- `hive2/boards/node_2/readings` utolso rekord: `2026-07-30T09:07:19Z`
- `hive2/boards/node_3/readings` utolso rekord: `2026-07-30T09:07:39Z`

Kovetkeztetes: a wireless rendszeren a node-ok utoljara kb. 7-8 perccel a master leallasa elott jelentkeztek. Ez arra utal, hogy:

1. a cloud oldal 2026-07-30 09:14 UTC-ig biztosan mukodott,
2. a node-radio link meg 09:07 UTC korul meg elt,
3. utana maga a master vagy az uplink/tap allt meg.

## Mellekszalak, amik NEM tunnek fo oknak

- Firebase auth kieses: NEM, most is sikeres a device-user login.
- RTDB rules/schema mismatch: NEM, a live hive2 rekordok jo helyre es jo szerkezetben mentek be.
- Dashboard config/path problema: NEM, a dashboard a helyes hive1/hive2 agakat nezi.

## Erdemes helyszinen ezt nezni eloszor

1. Prototype: tap/WiFi/router oldali ok, mert minden hive1 stream egyszerre allt le 2026-07-24-en.
2. Wireless master: kap-e tapot, el-e a helyi statuszoldal, latszik-e `WIFI GOT_IP` / `LOCAL_STATUS open http://...` boot utan.
3. Wireless uplink: a hotspot/router tenyleg ch6-on van-e, mert a master production defaultja ch6.
4. Wireless master log: van-e `CHANNEL_MISMATCH`, `cloud PUT failed`, `Firebase repeated 401`, `backup ring full`, vagy folyamatos reconnect.
5. Ha a master el, de node nincs: ESP-NOW csatornaeltérés vagy a master radio nem maradt fent.

## Relevans helyi fajlok

- `wireless_scaling/firmware/master/platformio.ini`
- `wireless_scaling/firmware/master/main/master.c`
- `wireless_scaling/firmware/master/firebase_hive2_rules.md`
- `prototype/firebase/database.rules.json`

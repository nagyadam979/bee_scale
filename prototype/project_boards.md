# Board azonosítók — ESP32-C3 mérőegységek

> Projekt-memóriafájl. A `MEMORY.md` hivatkozik rá. A két fizikai mérőboard
> azonosítása, bekötése és RTDB-ágai. Titkot NEM tartalmaz (lásd `secrets.h`).
> Utolsó frissítés: 2026-06-11.

## A névadás eredete

A "21" és "12" nevek a HX711 **eredeti** DOUT/SCK lábsorrendjéből származnak:
- "21" = DOUT=GPIO2, SCK=GPIO1
- "12" = DOUT=GPIO1, SCK=GPIO2 (eredetileg)

**FIGYELEM:** a board_12 HW-cseréje után (2026-06-08) az új panel bekötése
**DOUT=2 / SCK=1** lett — azaz ma már **mindkét board azonos mappinget használ**
(2/1). A "12" név történeti azonosító maradt, a tényleges pinout-ra ne ebből
következtess, hanem az alábbi táblából / a `board_*.ino` fejlécéből.

## board_21

| Tulajdonság | Érték |
|---|---|
| MAC (vége) | …89:10 |
| Szerep | bal/első mérőrúd → **w1** |
| HX711 | DOUT=GPIO2, SCK=GPIO1 |
| Kaptár-hőmérő | **DS18B20** OneWire, **GPIO3** (külső 4.7 kΩ felhúzó a DQ↔VCC közé, ha a modulon nincs) |
| RTDB súly-ág | `/hives/hive1/rods/w1/readings` → `{ts, w}` |
| RTDB hőm-ág | `/hives/hive1/temp/readings` → `{ts, t}` (kaptáron belüli szonda) |

## board_12

| Tulajdonság | Érték |
|---|---|
| MAC (vége) | …31:50 |
| Szerep | jobb/hátsó mérőrúd → **w2** |
| HX711 | DOUT=GPIO2, SCK=GPIO1 (HW-csere után, = board_21 mapping) |
| Env-szenzor | **AHT20** I2C, SDA=GPIO6 / SCL=GPIO5 (hőm + pára) |
| BMP280 | **NINCS a buszon** (i2c-scan: nem válaszol) → nyomás (`p`) opcionális/elhagyva |
| RTDB súly-ág | `/hives/hive1/rods/w2/readings` → `{ts, w}` |
| RTDB env-ág | `/hives/hive1/env/readings` → `{ts, t, h}` (doboz-/környezeti) |

## HW-történet (board_12)

A régi board_12 HX711-e hibás volt: először raw ~−1000 random zaj, a HX711-modul
cseréje után raw=0 stuck. Végül **teljes csere**: új ESP32-C3 + új HX711 → stabil
raw ~−233k. Innen az új DOUT=2/SCK=1 bekötés.

## Közös firmware-jellemzők (mindkét board)

- Tara/kalibráció: 64 mintás átlag, NVS-be (`Preferences`) mentve.
- Feltöltött rekord súlya: 10 minta átlaga (`WEIGHT_AVG_SAMPLES`).
- Küldési ütem: **5 perc**, nem-blokkoló (`millis()`).
- Offline RAM-FIFO: 64 minta, megtelve a legrégebbi esik ki (reboot=adatvesztés, elfogadott).
- Retry: exponenciális backoff 2 s → max 60 s.
- Idő: NTP, UTC epoch (TZ-t a dashboard kezeli).
- WiFi + ArduinoOTA + Telnet-menü (port 23, stream/menu mód).
- Auth: dedikált eszköz-user (`esp32@beescale.local`), a Rules ezzel engedi az írást.

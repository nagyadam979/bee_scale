# bee_scale — ESP32 → Firebase RTDB feltöltő (Feladat 01)

Az ESP32 firmware **kommunikációs modulja**: 5 percenként feltölt egy súlymérési
rekordot a Firebase Realtime Database-be, hitelesítve és hibatűrően.
A HX711-olvasás **nincs** benne (mock mögött), riasztás **nincs** (Firebase Spark-kompatibilis, adat-only).

## Fájlok

```
firmware/bee_scale_rtdb/
  bee_scale_rtdb.ino     fő sketch: WiFi, NTP, 5 perces mintavétel, orchestráció
  config.h               NEM titkos konfig (HIVE_ID, intervallum, buffer, backoff…)
  secrets.h.example      titok-sablon → másold secrets.h-ra (gitignore-olt)
  weight_source.h        súlyforrás interfész: getWeights() → {w_total, w1, w2}
  weight_source_mock.cpp szintetikus mock (HX711 helyett, teszteléshez)
  rtdb_writer.h/.cpp     RTDB writer: auth + async push + retry/backoff + FIFO buffer
firebase/
  database.rules.json    RTDB Security Rules (eszköz ír, web app csak olvas)
```

> **Megjegyzés a `HIVE_ID`-ról:** a deliverable szerint minden konfig (intervallum,
> `HIVE_ID`) egy helyen, jól láthatóan — ezért a `HIVE_ID` a `config.h`-ban van
> (nem titok). A `secrets.h` csak valódi titkokat tartalmaz.

## Adatséma

RTDB push a `/hives/<HIVE_ID>/readings` ág alá, generált pushId-val:

```json
{ "ts": 1717160400, "w_total": 42.13, "w1": 21.02, "w2": 21.11 }
```

`ts` = UTC epoch **másodperc** (NTP-sync), `w_*` = kg (float, 2 tizedes).

## Hibatűrés (dióhéjban)

- **WiFi**: `setAutoReconnect(true)` + nem-blokkoló manuális reconnect.
- **Token-refresh**: a FirebaseClient lib auto-refresh; a hibaág logolva, az
  állapotgép `app.ready()` alapján vár.
- **Írás retry**: exponenciális backoff (`RETRY_BACKOFF_BASE_MS` →
  `RETRY_BACKOFF_MAX_MS`), push-timeout-tal.
- **Offline buffer**: RAM-only FIFO (`BUFFER_CAPACITY`), reconnectkor kiürül.
  Megtelve a **legrégebbi** minta esik ki. **Reboot/áramszünet esetén a buffer
  elvész — ebben a fázisban elfogadott** (nincs NVS-perzisztencia).

---

## Firebase-oldali előfeltételek (egyszeri)

1. **Projekt + RTDB**: Firebase Console → új projekt (Spark/no-cost) → Build →
   Realtime Database → Create Database. Jegyezd fel a **DB URL**-t.
2. **Web API key**: Project settings → General → *Web API Key*.
3. **Eszköz-user**: Authentication → Sign-in method → **Email/Password** engedélyezése
   → Users → Add user (pl. `device@example.com` + erős jelszó). Ez az eszköz írója.
4. **Security Rules feltöltése**: a `firebase/database.rules.json` tartalma → Realtime
   Database → Rules → Publish. **Cseréld ki** a `device@example.com` emailt a 3.
   lépésben létrehozott eszköz-user emailre.
   - Feltölthető CLI-vel is: `firebase deploy --only database`
     (`firebase.json`-ben: `{"database": {"rules": "firebase/database.rules.json"}}`).
5. (Web app olvasáshoz) hozz létre egy **viewer** usert vagy engedélyezz Anonymous
   auth-ot — a rules `auth != null`-t vár az olvasáshoz. A viewer **nem ír** (email nem egyezik).

## Build / Flash

### Arduino IDE
1. Boards Manager → **esp32** csomag (Espressif) telepítése.
2. Library Manager → **FirebaseClient** (mobizt) telepítése.
3. Nyisd meg a `firmware/bee_scale_rtdb/bee_scale_rtdb.ino`-t.
4. Másold a `secrets.h.example`-t **`secrets.h`**-ra ugyanabba a mappába, töltsd ki.
5. Válaszd ki a board-od (pl. *ESP32 Dev Module*), Upload.

### arduino-cli
```bash
# egyszeri setup
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install FirebaseClient

# secrets
cp firmware/bee_scale_rtdb/secrets.h.example firmware/bee_scale_rtdb/secrets.h
# töltsd ki a secrets.h-t

# fordítás (ESP32 Dev Module)
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/bee_scale_rtdb

# flash (cseréld a portot)
arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/cu.SLAB_USBtoUART firmware/bee_scale_rtdb
arduino-cli monitor -p /dev/cu.SLAB_USBtoUART -c baudrate=115200
```

## Kész-kritérium ellenőrzés

- **Alap**: boot után WiFi-re csatlakozik, hitelesít, és 5 percenként új rekord
  jelenik meg a konzolban a `/hives/<HIVE_ID>/readings` alatt a fenti sémával.
  (Az első mintát az NTP-sync után rögtön elküldi, nem vár 5 percet.)
- **Reconnect + buffer**: kapcsold ki a WiFi-t pár percre. A kiesés alatt vett
  minták RAM-ban gyűlnek (`pending` nő a soros logban); visszakapcsolás után a
  buffer kiürül, a rekordok megjelennek a DB-ben.
- **Rules negatív teszt**: nem hitelesített kliens nem ír/olvas; viewer user csak olvas.
  ```bash
  # írás token nélkül → PERMISSION_DENIED
  curl -X POST -d '{"ts":1,"w_total":1,"w1":1,"w2":1}' \
    "https://<DB_URL>/hives/hive1/readings.json"
  # olvasás token nélkül → PERMISSION_DENIED
  curl "https://<DB_URL>/hives/hive1/readings.json"
  ```

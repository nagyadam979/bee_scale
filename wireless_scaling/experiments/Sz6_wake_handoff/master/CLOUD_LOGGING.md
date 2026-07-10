# Sz6 master cloud logging

Cel: a master minden valid ESP-NOW DATA csomagot Firebase Realtime Database REST API-val
tovabbit a prod RTDB-be.
A soros log tovabbra is megmarad, de az overnight meres nem fugg tole.

## Firmware konfiguracio

1. Masold a mintat:

```sh
cp main/cloud_config.example.h main/cloud_config.h
```

2. Toltsd ki:

```c
#define SZ6_WIFI_SSID "..."
#define SZ6_WIFI_PASS "..."
#define SZ6_FIREBASE_API_KEY "..."
#define SZ6_FIREBASE_DATABASE_URL "https://..."
#define SZ6_FIREBASE_EMAIL "..."
#define SZ6_FIREBASE_PASSWORD "..."
#define SZ6_FIREBASE_ROOT_PATH "experiments/sz6_overnight"
```

3. Build:

```sh
pio run -e esp32-s3-zero-cloud
```

Az `esp32-s3-zero` env cloud nelkuli baseline marad.

## Fontos radio feltetel

Cloud modban a master WiFi STA-kent csatlakozik, ezert a radio csatornat az AP/hotspot diktalja.
Az ESP-NOW node-oknak ugyanazon a csatornan kell adniuk, kulonben a master nem fogja venni oket.
Overnight teszthez praktikus egy fix 2.4 GHz hotspot/AP csatorna, es ahhoz allitott node firmware.

## RTDB utvonal

```text
/experiments/sz6_overnight/nodes/node_<node_id>/readings/{pushId}
```

Ez szandekosan nem a `/hives/hive1/...` prod ag alatt van, tehat a meglevo dashboard
es prod meresi strukturak nem latjak. A helyi rules forrasban csak erre a kulon
experiment agra kerult irasi engedely: `../prototype/firebase/database.rules.json`.

## POST payload

Minden valid csomag egy JSON objektum:

```json
{
  "ts": 1717160400,
  "rx_ms": 123456,
  "node_id": 1,
  "seq": 42,
  "boot_id": "0x1234ABCD",
  "packet_idx": 0,
  "vbat_mv": 4070,
  "weight": -175123.0,
  "temp_c": 23.50,
  "rssi": -48,
  "kind": "NEW",
  "gap": 0,
  "src_mac": "AA:BB:CC:DD:EE:FF"
}
```

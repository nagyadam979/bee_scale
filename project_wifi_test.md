# Teszt WiFi — fejlesztői hálózat

> Projekt-memóriafájl. A `MEMORY.md` hivatkozik rá. A fejlesztés/bench alatt
> használt ideiglenes hálózat. **Jelszót NEM tartalmaz** — a credentialök a
> gitignore-olt `secrets.h`-ban élnek (struktúra: `secrets.h.example`).
> Utolsó frissítés: 2026-06-11.

## Hálózat

| Tulajdonság | Érték |
|---|---|
| SSID | `AMS-TP-2.4GHz` |
| Sáv | 2.4 GHz (az ESP32-C3 csak 2.4 GHz-et tud) |
| Jelleg | **ideiglenes, kísérleti** — bench/fejlesztői háló, nem a telephelyi |
| Jelszó / OTA-jelszó | `secrets.h` (`WIFI_PASSWORD`, `OTA_PASSWORD`) — repóban nincs |

## Megjegyzés a kitelepítéshez

A firmware jelenleg **erre a teszt-hálózatra** van fordítva (`secrets.h`).
A telephelyi kitelepítéskor a helyszíni 2.4 GHz-es SSID/jelszóra kell átírni a
`secrets.h`-t, majd újraflashelni (OTA-val, ha azonos hálón vagy; különben USB).
A telephelyi WiFi kiépítése a `MEMORY.md` szerint a kitelepítés fő nyitott pontja.

## secrets.h szerkezete (példa, érték nélkül)

```c
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."
#define OTA_PASSWORD  "..."
#define FIREBASE_API_KEY     "..."
#define DATABASE_URL         "https://terrarium1-default-rtdb.europe-west1.firebasedatabase.app"
#define DEVICE_USER_EMAIL    "esp32@beescale.local"
#define DEVICE_USER_PASSWORD "..."
```

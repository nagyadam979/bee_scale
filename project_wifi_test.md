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

**2026-06-11:** a `secrets.h` (mindkét board) átállítva a **telephelyi hálózatra:
`TP-Link_4448`** (2.4 GHz). A jelszó a gitignore-olt `secrets.h`-ban. A teszt-háló
(`AMS-TP-2.4GHz`) már nincs használatban — ez a doksi historikus referencia.

A változás életbe lépéséhez **újra kell flashelni** mindkét boardot (OTA, ha azonos
hálón vagy; különben USB), mert a WiFi credential fordítási időben kerül a firmware-be.

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

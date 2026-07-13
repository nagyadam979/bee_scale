# Firebase RTDB rules snippet for hive2

The final master firmware writes dashboard-facing records under `/hives/hive2`.
Merge the following shape into `firebase/database.rules.json` before running the
prod cloud build against RTDB. This is intentionally separate from `hive1`.

**Séma (v2 — tiszta board-modell):**
- `boards/<id>/readings` — minden mérőrúd (node_1..3 ÉS master) a SAJÁT rekordjában
  viszi a súlyt (`weight_raw`) és a saját kalibrációs cella-hőjét (`temp_c`).
  A master `temp_c`-je a mérleg-cella DS18B20-ja (a régi `scale_c`).
- `env/readings` — telepítés-szintű KÖRNYEZETI adat: eső (`rain_mm`, intervallum-delta),
  levegő (`air_t_c`/`air_rh`), nyomás (`bmp_t_c`/`pressure_pa`) ÉS a kaptár-szonda
  (`hive_c`, a régi DS0).
- A korábbi külön `temp` ág MEGSZŰNT (a scale_c a boards-ba, a hive_c az env-be került).

```json
{
  "hives": {
    "hive2": {
      ".read": "auth != null",

      "boards": {
        "$board": {
          "readings": {
            ".write": "auth != null && auth.token.email === 'esp32@beescale.local'",
            ".indexOn": ["ts"],
            "$record": {
              ".validate": "newData.hasChildren(['ts','weight_raw'])",
              "ts": { ".validate": "newData.isNumber() && newData.val() > 1700000000" },
              "node_id": { ".validate": "newData.isNumber()" },
              "weight_raw": { ".validate": "newData.isNumber()" },
              "temp_c": { ".validate": "newData.isNumber()" },
              "vbat_mv": { ".validate": "newData.isNumber()" },
              "sensor_error_mask": { ".validate": "newData.isNumber()" },
              "$other": { ".validate": false }
            }
          }
        }
      },

      "env": {
        "readings": {
          ".write": "auth != null && auth.token.email === 'esp32@beescale.local'",
          ".indexOn": ["ts"],
          "$record": {
            ".validate": "newData.hasChildren(['ts','rain_mm'])",
            "ts": { ".validate": "newData.isNumber() && newData.val() > 1700000000" },
            "rain_mm": { ".validate": "newData.isNumber()" },
            "air_t_c": { ".validate": "newData.isNumber()" },
            "air_rh": { ".validate": "newData.isNumber()" },
            "bmp_t_c": { ".validate": "newData.isNumber()" },
            "pressure_pa": { ".validate": "newData.isNumber()" },
            "hive_c": { ".validate": "newData.isNumber()" },
            "sensor_error_mask": { ".validate": "newData.isNumber()" },
            "$other": { ".validate": false }
          }
        }
      }
    }
  }
}
```

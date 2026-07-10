# Firebase RTDB rules snippet for hive2

The final master firmware writes dashboard-facing records under `/hives/hive2`.
Merge the following shape into `firebase/database.rules.json` before running the
prod cloud build against RTDB. This is intentionally separate from `hive1`.

```json
{
  "hives": {
    "hive2": {
      ".read": "auth != null",

      "rods": {
        "$rod": {
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
            "$other": { ".validate": false }
          }
        }
      },

      "temp": {
        "readings": {
          ".write": "auth != null && auth.token.email === 'esp32@beescale.local'",
          ".indexOn": ["ts"],
          "$record": {
            ".validate": "newData.hasChild('ts') && (newData.hasChild('hive_c') || newData.hasChild('scale_c'))",
            "ts": { ".validate": "newData.isNumber() && newData.val() > 1700000000" },
            "hive_c": { ".validate": "newData.isNumber()" },
            "scale_c": { ".validate": "newData.isNumber()" },
            "$other": { ".validate": false }
          }
        }
      }
    }
  }
}
```

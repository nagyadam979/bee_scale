# H4 — Master board deep-sleep áram (ESP32-S3-Zero) — LEZÁRVA, a board ELVETVE

**Típus:** Hardver chunk · **Setup:** 0 (bench) · **Irány:** (A) elektromos/energetikai

## ⛔ Kimenet / döntés (2026-07-02)
Az ESP32-S3-Zero nyers deep-sleep árama **mA-nagyságrend** (éles darabon) — a WS2812 fedélzeti
LED SW-kioltása (fekete-frame + data-LOW/hold) és a 4 MB flash-kényszer / boot-loop-fix (lásd
commit-történet) UTÁN is. Ez **nagyságrenddel a node C3 ~185 µA (H1) fölött** → az S3-Zero
**alkalmatlan** az akkus / ultralow-power masterhez.

**Pivot: a master is ESP32-C3 SuperMini** (a már bizonyított node-board; a H1 deep-sleep minta
újrahasznosítva). A master nyugalmi alapszáma ezzel a H1 C3-száma (~185 µA). A kétmagos + PSRAM
előny nem hiányzik: az **RX-ablak és az uplink szekvenciális** (a 4G stick amúgy is ~2-3 perc alatt
bootol az ESP-NOW vétel UTÁN), a napi puffert az **AT24C256 EEPROM** fedezi. Lásd `MEMORY.md` 7. + 8.

Az alábbi eredeti, S3-specifikus leírás a mérés dokumentációjaként marad meg (a firmware/build a
mappában szintén S3-célú — historikus artefakt).

## Cél
A **master** (ESP32-S3-Zero, Waveshare) nyers nyugalmi (deep-sleep) áramának kimérése —
a master-energiamérleg **alapszáma**. Ez a node-oldali **H1** (ESP32-C3 deep-sleep, ~185 µA)
master-párja. Szolgálja a MEMORY.md 7. nyitott kérdés #2-t (master nyugalmi fogyasztás) és
a **H3** duty-cycle mérés "nyugalmi" alsó korlátját.

A firmware MINIMÁLIS és a mérést szolgálja: nincs rádió (WiFi/ESP-NOW), nincs PSRAM-használat.

## Fedélzeti fogyasztók (webről ellenőrizve, 2026-07-02)
- **USB-Serial-JTAG pad** (~500–800 µA) — az S3-on is deep sleepben is ébren marad, az IDF csak
  light sleepnél kapcsolja le → a firmware **explicit lekapcsolja** (`sleep_console_usj_pad_backup_and_disable()`).
  Ez a H1 fő tanulsága a C3-on (731 µA → 185 µA).
- **WS2812 RGB LED (GPIO21)** — a chip a táprailen ül, nyugalmi árama HW-adottság (0…~1 mA, darabfüggő),
  SW-ből nem kapcsolható le. A data-lábat alvásra **LOW + hold**. Reset után a LED tipikusan sötét.
- **LDO ME6217C33M5G** — nyugalmi ~15 µA (adatlap-tartomány), HW-adottság.
- Várható chip RTC deep-sleep: ~7–10 µA.

## Fordítás / flash
```
pio run                              # fordítás
pio run -t upload -t monitor         # flash + monitor (natív USB-Serial-JTAG)
```
Tartós deep sleepben a board **nem enumerál** USB-n → flasheléshez **BOOT nyomva + reset**
(bootloader), majd `pio run -t upload`. `idf.py`-vel: `idf.py set-target esp32s3 && idf.py build`.

## Hangolható paraméter (`main/h4_master_deep_sleep.c` teteje)
- `DEEP_SLEEP_WAKEUP_SEC` — `0` = tartós (örök) deep sleep, minden wakeup tiltva (**tiszta mérés**);
  `>0` = timer-wakeup ennyi mp után (ciklikus szimuláció).

## Mérés
3.3 V a `3V3` lábra (vagy USB 5 V az LDO-n át), multiméter/DMM soros az áramkörben. A boot-log
lefut, majd a USB-pad lekapcsol és a board deep sleepbe megy → a DMM a nyugalmi áramot mutatja.

**A legtisztább méréshez** (ha a WS2812 / LDO hozzájárulását le akarod választani) a fedélzeti
WS2812 fizikailag leszedhető, ahogy a H1-ben a C3 power-LED-je. Ha boot után a RGB LED **világít**
(glitch), jelezd → akkor fekete-frame kell (led_strip); tapasztalat szerint reset után sötét.

## Pass / fail (tájékoztató, mérésen finomítandó)
| Master deep-sleep áram (WS2812 rajta) | Jó | Elfogadható | Újranézni |
|---|---|---|---|
| Teljes | **< 50 µA** | < 200 µA | **> 500 µA** → USB-pad? WS2812? LDO? |

## Hivatkozások
- Node-párja: [`../H1_node_board_prep/`](../H1_node_board_prep/) (deep-sleep minta: USB-JTAG pad + GPIO hold).
- Ráépül: [`../H3_master_power_duty_cycle/`](../H3_master_power_duty_cycle/) (valós forgalommal, duty-cycle).
- Forrás: waveshare.com/wiki/ESP32-S3-Zero, espboards.dev/esp32/esp32-s3-zero/.

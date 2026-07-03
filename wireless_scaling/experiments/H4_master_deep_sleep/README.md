# H4 — Master board deep-sleep áram (ESP32-S3-Zero) — TÁRGYTALAN

**Típus:** Hardver chunk · **Setup:** 0 (bench) · **Irány:** (A) elektromos/energetikai

## ⛔ TÁRGYTALAN (2026-07-03)
A master **hálózati tápot** kap → **always-on**, sosem alszik → a deep-sleep áram irreleváns.
A mérés kimutatta: az S3-Zero deep sleepben **mA-t húz** (a WS2812 SW-kioltása + 4 MB flash-fix
UTÁN is), ezért egy ideig felmerült a C3-ra váltás — de a hálózati táp okafogyottá tette, **az
S3-Zero marad** (dual-core + PSRAM az always-on RX/uplinkhez). Az alábbi leírás historikus.

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

# RG — Esőmérő reed-impulzus (WH-SP-RG tipping bucket)

**Típus:** Periféria-szenzor kísérlet (nem a H/Sz/M validációs rácsból) · **Setup:** 0–1 (1 node, kézi billentés / vizes kalibráció) · **Irány:** (A) elektromos + firmware

## Cél
A **WH-SP-RG** (MS-WH-SP-RG) önkiürülő **billenővödör** esőmérő reed-impulzusainak
kiolvasása és megbízható **számlálása a node-on**, a projekt deep-sleep / ESP-NOW
architektúrájába illesztve. A kimenet: hány billenés → mennyi csapadék, és **hogyan
számoljunk alvás közben** anélkül, hogy az energiamérleget szétverné.

## A szenzor tényei (webről ellenőrizve, 2026-07-02 — források a végén)
- **Típus:** önkiürülő billenővödör (tipping bucket), **reed-kapcsoló** impulzus-kimenet.
- **Felbontás:** **1 impulzus = 0,2794 mm** csapadék (= 0,011"). Minden billenéskor **pillanatnyi
  kontaktus-zárás** (reed short), amit digitális számláló / µC érzékel.
- **Csatlakozó:** **RJ11**, a reed a **két KÖZÉPSŐ érintkezőre** van kötve. **Polaritás nem
  számít** — egyszerű kapcsoló (nincs aktív elektronika, nincs táp).
- **Következmény:** a node oldalán ez egy egyszerű **nyitott kontaktus GND felé**: egyik középső
  ér → GPIO (belső pull-up), másik → GND. Nyugalomban a pin HIGH, billenéskor rövid LOW.

## A KÍSÉRLET LÉNYEGI KÉRDÉSE — impulzusszámlálás deep sleepben (C3)
A node az architektúra szerint az idő nagy részét **deep sleepben** tölti; az eső viszont
**aszinkron, bursztös**. A C3 három korlátja miatt ez nem triviális:
- **Nincs ULP** koprocesszor (az S3/klasszikus ESP32-vel szemben) → nincs mivel „a háttérben" számolni.
- A **PCNT** hardveres impulzusszámláló **deep sleepben nem jár**.
- A **deep-sleep GPIO-ébresztés SZINT-triggerelt** (nem él), és egy ébredés ~**192 ms**.

### Két járható út (ezt méri ki a kísérlet)
| Út | Hogyan | Előny | Ár / kockázat |
|---|---|---|---|
| **(A) Wake-on-tip** | Minden billenésre ébredés (level-LOW GPIO wakeup), `RTC_DATA_ATTR` számláló++, debounce/lockout, vissza alvásba | **Nincs extra HW**; a meglévő node-ra ráültethető | ~192 ms/ébredés → **nagy esőben ébredés-vihar**; szint-trigger + bounce kezelése kényes |
| **(B) Külső számláló IC** | Ultra-low-power ripple counter (pl. **CD4520 / 74HC4040**, ~µA nyugalmi) számol alvás alatt is; a node a **saját** (pl. 30 s) ébredésekor olvassa ki + nullázza | Nincs per-tip ébredés; a fogyasztás független az eső intenzitásától | **+1 alkatrész** + kiolvasó vonalak (GPIO/SPI/I²C-expander), reset-kezelés |

**Kiinduló javaslat:** először **(A)-t** mérni — nincs BOM-hatás, és a való életben az esemény-ráta
alacsony (50 mm/h zápor is csak ~1 billenés / 20 s). Ha az ébredés-költség a **H1/H3 energiamérleget**
érdemben rontja (worst-case tartós heves eső), akkor **(B)** a fallback. A döntés a mért ébredés-költségen áll.

## Bekötés
| Funkció | ESP32-C3 SuperMini | Megjegyzés |
|---|---|---|
| Reed egyik ere | **GPIO4** (`RG_PIN`) | Belső pull-up be; kerüli a HX711-et (H1: SCK=GPIO5, DT=GPIO6). Deep-sleep GPIO-ébresztésre alkalmas. |
| Reed másik ere | **GND** | Polaritás mindegy. |
| (RJ11) | két **középső** érintkező | A többi ér ezen a szenzoron nincs bekötve. |

> **Reed-védelem:** hosszú kábelnél / kültéren érdemes soros ~100 Ω + a GPIO-ra kis RC
> (pl. 100 nF a GND felé) a bounce/tranziens tompítására — HW-oldali kiegészítés, a firmware
> SW-debounce-a (150 ms) enélkül is elnyeli a bounce-ot. Mérésnél döntjük el, kell-e.

## Firmware — két mód (`RG_MODE` a forrás tetején)
`main/rg_rain_gauge_pulse.c`, a hangolható paraméterek a fájl tetején
(`RG_PIN`, `RG_DEBOUNCE_MS=150`, `RG_MM_PER_TIP=0.2794`, `RG_TIP_WINDOW_MS`):
- **MODE 0 — padi kalibráló / bounce:** ESP ébren, GPIO-**él**-ISR + SW-debounce, **élő log**
  (`TIP #n -> mm`). Erre kalibrálod a tényleges **ml/billenés** térfogatot (ismert vízmennyiség
  kimérve → billenésszám → mm/tip ellenőrzés) és figyeled a reed-bounce viselkedést.
- **MODE 1 — deep-sleep wake-on-tip (A út):** `RTC_DATA_ATTR` számláló túléli az alvást; minden
  billenésre ébred, számol, bevárja a kontaktus-nyitást + lockout, majd `gpio_wakeup` LOW-szintre
  + `gpio_hold_en` (C3, nincs RTCIO) és vissza deep sleepbe. **H1-minta:** USB-Serial-JTAG pad
  lekapcsolás alvás előtt (nyugalmi áram), a boot-log USB-n megmarad.

## Fordítás / flash
```
pio run                        # vagy: idf.py set-target esp32c3 && idf.py build
pio run -t upload -t monitor   # SuperMini natív USB-Serial-JTAG
```
Deep-sleepes MODE 1-nél a board alvásban nem enumerál USB-n → flasheléshez **BOOT+reset** (H1-tanulság).

## Pass / fail
| Mérés | Pass |
|---|---|
| **Kalibráció (MODE 0):** ismert vízmennyiség → billenésszám → mm | a számolt mm a kiöntött mennyiség ±5%-án belül, **0 elveszett és 0 dupla** billenés (bounce nem számol többször) |
| **Deep-sleep számlálás (MODE 1):** N kézi billentés alvás közben | az `RTC_DATA_ATTR` számláló pontosan **N** (nincs veszteség/dupla), a node minden billenés után visszaalszik |
| **Ébredés-költség (MODE 1):** átlagáram tartós billentgetésnél | az (A) út **energiamérlege** elfogadható a várható eső-rátán; ha nem → (B) IC |

## Nyitott kérdések / kockázatok
1. **(A) vs. (B) döntés** — a mért ébredés-költségen áll (lásd fenti tábla). Worst-case: tartós heves eső.
2. **Reed-bounce a szint-triggeres wake-nál** — a MODE 1 a kontaktus-nyitást bevárja + lockout; ellenőrizni, hogy tényleg 1 billenés = 1 count.
3. **Kültéri kábelvédelem / RC-szűrő** — kell-e HW-debounce a SW mellé (hosszú kábel, tranziensek).
4. **Adatút a payloadba** — az eső egy **kumulatív számláló** (nem pillanatérték); hogyan illik a `payload_t`-be (új mező vs. külön beacon) → Sz0 header-döntés, ha az esőmérő bekerül a rendszerbe.
5. **Kalibráció-ellenőrzés** — a 0,2794 mm/tip a gyári érték; a konkrét darab tölcsér-átmérőjével ellenőrizhető (kiöntött térfogat / billenésszám).

## Hivatkozások
- `../../MEMORY.md` (7. rendszerarchitektúra, `payload_t`), `../README.md` (chunk-index), H1 (deep-sleep minta), Sz0 (header-szerződés, ha bekerül).
- WH-SP-RG spec / pinout: [hestore.eu MS-WH-SP-RG](https://www.hestore.eu/en/prod_10042313.html), [MISOL manual (manuals.plus)](https://manuals.plus/asin/B00QDMBXUA), [SparkFun Weather Meter Hookup Guide](https://learn.sparkfun.com/tutorials/weather-meter-hookup-guide/all).
- C3 deep-sleep számlálás / külső counter megközelítés: [garrysblog — ESP32 rain gauge deep sleep](https://garrysblog.com/2025/05/30/rebuilding-a-rain-gauge-using-esp32-with-live-updates/), [ESPHome pulse_counter](https://esphome.io/components/sensor/pulse_counter/).

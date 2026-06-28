# M1 — Mérési setup (deep-sleep drift / guard-window, felügyelet nélküli éjszakai futás)

Ez a doksi a **konkrét mérés-elrendezést** írja le: a két board flashelése, a host-oldali
robusztus logger indítása, az egész éjszakás (felügyelet nélküli) futás lépései, majd az
offline elemzés. A node a saját deep-sleep slow-clockjának (belső RC) driftjét méri: T=30 s-onként
ébred, egy ESP-NOW beacont küld a masternek; a master minden vételre egy gép-grep-elhető sort ír
a soros portjára, amit a host-logger HOST-időbélyeggel fájlba ment. Eredmény-skeleton:
`mereseredmenyek.csv`. Cél/pass-fail: `README.md`.

> **A mérés lényege:** több órás/napos futás természetes hőmérséklet-ingadozásban. A node ébredési
> periódusa a nominális 30 s-tól ELDRIFTEL (a kutatás szerint reálisan 0,5–5%, ~1000 ppm/°C
> hőfüggéssel, kompenzálatlanul). A drift **SZÁZALÉKOS** nagyságrendű, nem ppm-es. Ebből
> méretezzük az Sz4 **guard-window-t** (mekkora vételi ablak tartja a node-ot a slotban N cikluson
> át korrekció nélkül).

---

## 1. Szerepek és boardok

| Szerep | Board | PlatformIO env | Port (tipikus) | Konzol |
|---|---|---|---|---|
| **Node** | ESP32-C3 SuperMini | `esp32-c3-supermini` | `/dev/cu.usbmodem101` | natív USB-Serial-JTAG |
| **Master** | classic ESP-WROOM-32 | `esp32-wroom-32` | `/dev/cu.usbserial-0001` | UART0 (CP2102 bridge) |

> A master logja (CP2102, külső bridge) háttérből / pyserialből **megbízhatóan olvasható** — a
> drift kiértékelése EZEN alapul. A node a saját USB-jén csak ébredéskor logol, és deep sleep alatt
> **leválik a USB-ről** (nem enumerál) — ez NORMÁLIS, a node logja a méréshez nem kell.

**Csatorna-egyezés (KRITIKUS):** ahogy az Sz2-nél, a node és a master ESP-NOW csatornájának EGYEZNIE
kell (a node-/master-firmware tetején, alap 1). Ha átírod, mindkettőt flasheld újra. (A PHY-ráta/
protokoll-fix — `set_protocol` LR nélkül + `esp_now_set_peer_rate_config(11b,1M_L)` — a Sz2-ből
öröklődik; e nélkül a C3↔classic-ESP32 vétel néma.)

---

## 2. A T=30 s periódus átírása (node firmware)

A node ébredési periódusa egyetlen `#define` a node firmware tetején:

```
// node/main/m1_node_beacon.c
#define M1_WAKE_PERIOD_SEC 30      // <-- ezt írd át, ha más periódust mérsz
```

Ennek meg KELL egyeznie az elemző `--nominal-period-s` paraméterével (alap 30). A master a beaconban
kapott `reqT_ms` mezőt is logolja (= 30000), így utólag ellenőrizhető.

---

## 3. Flashelés — két külön USB-port

A portokat listázd: `~/.platformio/penv/bin/pio device list`. A portot `--upload-port`-tal add meg
explicit (különben a PIO az elsőként talált portot választaná).

### Master — ELŐBB ezt (hogy fogadja a beaconokat)

```
cd master
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbserial-0001
```

> A master monitort NE a `pio device monitor`-ral nézd a méréshez — azt a **host-logger**
> (`log_master.py`) veszi át, mert csak az tesz HOST-időbélyeget a sorok elé és csatlakozik vissza
> USB-hibára. (Egy gyors sanity-check erejéig a `pio device monitor` jó, de a tényleges éjszakai
> rögzítés a logger.)

### Node — UTÁNA

```
cd node
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/cu.usbmodem101
```

> **Ha a C3 node nem flashelhető** (mert már alszik / korábbi deep-sleep firmware fut): tartsd
> nyomva a **BOOT** gombot, nyomj **reset**-et (vagy USB ki/be), engedd el a BOOT-ot → download mód,
> flashelj. (A H1/Sz2-ben rögzített eljárás.)

---

## 4. A felügyelet nélküli éjszakai futás — lépésről lépésre

1. **Master flash** (3. pont). A master elindul, beáll a csatornára, `WIFI_PS_NONE`, és várja a
   beaconokat. Kiír egy indító sort + időnként egy `M1HB` heartbeatet.

2. **Host-logger indítása** — EZ rögzít mindent HOST-időbélyeggel, USB-hibára újracsatlakozik:

   ```
   python3 log_master.py --port /dev/cu.usbserial-0001 --baud 115200
   ```

   (Alap port `/dev/cu.usbserial-0001`, alap baud 115200, alap kimenet
   `m1_capture_YYYYMMDD_HHMMSS.log` a futtatás könyvtárában. `--out FILE`-lal felülírható.)
   A logger a fájlba ÉS az stdout-ra ír (látod, hogy él), append módban flush + időnkénti `fsync`
   (áramszünet-védelem). USB-megszakadás / master-reset esetén `# RECONNECT <ts>` sort logol, vár
   ~2 s, és a végtelenségig újranyitja a portot — UGYANABBA a fájlba.

   > **A master innentől NE legyen resetelve / kihúzva.** Ha mégis (pl. áramszünet), a logger
   > visszacsatlakozik; a master `esp_timer`-e resetkor 0-ra ugorhat (az elemző ezt detektálja és
   > jelzi — reset esetén a HOST-óra az érvényes referencia). A node-reset új `boot_id`-t ad → új
   > szegmens, a `cycle` 0-ról újraindul (az elemző szegmensenként illeszt).

3. **Node flash + reset** (3. pont). A node elindul, kiírja a `boot_id`-t, küld egy első beacont,
   majd belép a 30 s-os deep-sleep ciklusba. Ettől kezdve ~30 s-onként ébred, beacont küld (a node
   ébredésenként **3×** küldi ugyanazt a `cycle`-t a megbízhatóságért — az elemző dedupol), és
   visszaalszik. A node USB-je alvás közben leválik — ez normális.

4. **Hagyd futni egész éjszaka** (napos nagyságrend ideális a hő-ingadozás miatt). A logger
   folyamatosan ír; reggel az stdout-on látod a beérkező `M1RX` sorokat és időnkénti `M1HB`-t.

5. **Reggel: Ctrl-C a loggeren.** Tiszta kilépés, a fájl lezárva (`# STOP <ts>` sor a végén).

6. **Offline elemzés:**

   ```
   python3 m1_drift_analyze.py m1_capture_YYYYMMDD_HHMMSS.log --csv out.csv --plot
   ```

   (Ha a node többször resetelt, több log-fájlt is megadhatsz egyszerre — az elemző egyesíti és
   `boot_id` szerint szegmentál.) A riport stdout-ra megy; a `--csv` per-cycle adatot ír (bárhol
   plotolható), a `--plot` opcionális PNG-t (matplotlib kell hozzá; ha nincs, csak figyelmeztet).

---

## 5. Mit ad az elemző (riport)

- **Futás hossza** (óra), beolvasott/M1RX/heartbeat/dedup/duplicate/crc-bad darabszámok, szegmensek.
- **Illesztett drift** szegmensenként, KÉT független referenciával:
  - **HOST-óra** (elsődleges, NTP-fegyelmezett fali óra): `T_meas` [s/cycle], drift ppm / % / s/óra.
  - **Master `esp_timer`** (precíz kereszt-ellenőrzés): ugyanaz; reset-detektálással.
- **AWAKE-korrigált drift:** a node `awake_ms` (precíz, 40 MHz xtal) levonva → a tiszta **alvás**-drift
  izolálva (a nyers drift = alvás-drift + awake/period; az awake-hozzájárulás külön kiírva).
- **Residual** (a lineáris fittől való eltérés) → az IDŐBEN VÁLTOZÓ (hő-)drift; szórása a CSV-ben is.
- **Hő-korreláció** (best-effort): ~ppm/°C a `die_c` ellen, ha a die-hő érdemben változik; különben
  „hőfüggés nem becsülhető ebből a futásból".
- **GUARD-WINDOW táblázat** (az M1 fő deliverable-je, Sz4-nek):
  `guard_s(N) = |drift| * N * nominal_period_s + margin`, konkrét N-ekre (alap 10 / 60 / 1440 ciklus).
  A `margin` alapból a residual-szórás 3-szorosa (`--margin-mode resid3`), vagy fix (`--margin-mode
  fixed --margin-fixed-s 2.0`).

> **Reprodukálhatóság (pass-feltétel):** ugyanaz a log → bitre ugyanaz a riport (determinisztikus
> stdlib fit). Az elemzőnek van `--selftest` módja, ami szintetikus logot generál ismert drifttel +
> hő-rámpával, lefuttatja magát, és ellenőrzi az egyezést.

---

## 6. PASS-feltétel (README pass/fail tábla)

| Feltétel | PASS |
|---|---|
| Drift jellemzés | mért drift-érték (ppm / % / s/óra) + hőfüggés napos futásból |
| Guard-window ajánlás | konkrét szám (s), amivel Sz4 N cikluson át slotban tart |
| Reprodukálhatóság | offline Pythonból a nyers log újrajátszva ugyanazt adja (+`--selftest` PASS) |

---

## 7. Hibakeresés

1. **Nem jön M1RX sor.** → Csatorna-eltérés (mint Sz2-ben): node/master `SZ2_CHANNEL` (a firmware tetején, alap 1) egyezzen.
   Ellenőrizd, hogy a node tényleg ébred (a `boot_id` + első beacon a master logban induláskor).
   PHY-ráta fix megvan-e mindkét oldalon (Sz2-örökség).
2. **`# RECONNECT` sorok a logban.** → USB-megszakadás vagy master-reset. Ha sok van, a master
   tápja/kábele instabil. A logger túléli; az elemzés a meglévő adatból fut. Master-reset esetén
   az `esp_timer` 0-ra ugorhat → az elemző jelez, a HOST-óra marad az érvényes referencia.
3. **Több szegmens a riportban.** → A node többször resetelt (új `boot_id`-k). Nem hiba; az elemző
   szegmensenként illeszt, a guard-window a legrosszabb szegmens alapján. Ha nem akartad, ne
   resetelgesd a node-ot.
4. **die_c mind „nan".** → A C3 die-hő szenzor nem olvasható / a firmware nem küldi. A drift-fit
   ettől még megy (host + master), csak a hő-korreláció marad el („nem becsülhető").
5. **Headless monitor.** → A `pio device monitor` átirányított stdout-tal elhasal; a méréshez
   eleve `log_master.py` (pyserial) a helyes eszköz.

---

## 8. Hangolható paraméterek

**Logger (`log_master.py`):** `--port`, `--baud`, `--out`. Belül: `RECONNECT_WAIT_S` (2 s),
`FSYNC_EVERY_S` (5 s), `READ_TIMEOUT_S` (1 s).

**Elemző (`m1_drift_analyze.py`):** `--nominal-period-s` (30), `--csv`, `--plot`, `--margin-mode`
(resid3|fixed), `--margin-fixed-s` (2.0), `--guard-n` ("10,60,1440"), `--selftest`.

**Node firmware:** `M1_WAKE_PERIOD_SEC` (30) — lásd 2. pont.

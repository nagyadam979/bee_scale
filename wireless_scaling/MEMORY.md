# bee_scale · wireless_scaling — Modul memória

> Tartós alapdokumentum a node-ház (2. fázis) almodulhoz. Ez orientálja a jövőbeli munkamenetet: cél, döntések, geometria-állapot, nyitott kérdések. Frissítsd, amikor egy döntés megszületik vagy egy geometriai fázis lezárul. A **részletes napló a commit-történet** (mit/miért/mi-működött/mi-nem).
> Utolsó frissítés: 2026-07-11 (**ÚJ DASHBOARD WEBAPP — 1. verzió preview channelen**: külön `dashboard/` frontend, ami egy helyen követi a hive1-et /régi, kalibrált kg/ és a hive2-t /új wireless, 4 egység, nyers count → „becsült kg" placeholder/. Vanilla + Firebase SDK 12.4.0 + ECharts 5.5.1, build nélkül; a PROD /`terrarium1.web.app`/ ÉRINTETLEN /csak Hosting preview channel, `firebase.json`-ban nincs `database` blokk/. Súly = telepítésenként EGY interpolált össz-vonal; hőm-chart = csak AHT20 + szonda /node `temp_c` és `scale_c` kalibrációs adat, nem rajzoljuk/; fotó-szerű navigáció /X-pinch-zoom, auto-Y, nincs csúszka/tooltip/ + a kritikus belső-canvas `touch-action:none` tanulság; interpolációs pont-sűrítés. Teljes doksi: **11. szakasz** + `dashboard/README.md`. Korábban ugyanaznap: **MASTER-FIRMWARE ÉLES + TELJES FLOTTA TEREPRE FLASHELVE**: master `esp32-s3-zero-local-status` env fut /node-vétel, saját mérés, flash-backlog, Firebase-feltöltés hive2 sémára, helyi HTTP státuszlap/; 3 node /`node1` VBAT-osztó nélkül, `node2`/`node3` osztóval/ csatlakozik ch6-on. A local host státuszlap 3 egymást követő bugja javítva /buffer-túllépés → `PRIu32` `%`-hiány crash → httpd LRU-purge beragadás/. Új üzemeltetési tudás: **10. szakasz** — deep-sleep node flash-workflow + gép-specifikus USB-eltűnés. Korábban 2026-07-04: **BENCH — ELSŐ fogyasztásmérés a TELJES node-elektronikán, akkuról**: áram-lépcsők kimérve; a 3,2 mA alvóáram gyökéroka a TPS63000-M kényszerített PWM-je volt → **PS-pad átforrasztás /PFM/ → 750 µA**; + HX711 power_up GPIO-hold fix — lásd 8./BENCH. Korábban 2026-07-03: **master HÁLÓZATI TÁP → always-on**, S3-Zero marad /dual-core+PSRAM/; napi döntések **D1**–**D4** /egyidejű WiFi STA+ESP-NOW → Sz2b; csomagvesztés-kivizsgálás → Sz2b; tiszta hálózati táp; hibrid mérő-master, DS3231 törölve/. 2026-06-29: M1 — node óra-drift; H1+H2+Sz2; geometria 2026-06-23)

> **Státusz:** a geometria **stabil és validált** — 2 nyomtatott part (`housing`, `bottom_lid`), 1-1 összefüggő solid. **Jelenleg senki nem dolgozik a modell geometriáján** — a következő geometriai munka KÜLÖN döntéssel indul. A modell forrása: `enclosure.py` (minden méret a fájl tetején, kommentált változó).

---

## 1. A modul egy mondatban

A méhkaptár-mérleg **vezeték nélküli node** teljes fejlesztése — elektronika (firmware), kísérletek és a **3D nyomtatott ház**. Jelenleg a 3D ház (parametrikus, ~IP65, ASA, build123d; mérőcella + ESP32 + 18650 akku befogadása) a kész/aktív rész; a **rendszerarchitektúra papíron eldöntve** (lásd 7. szakasz). Az `experiments/` már **kóddal + mért eredménnyel** kész chunkokat tartalmaz (H1, H2, Sz2, M1, M1b — lásd 8.; RG folyamatban); a `firmware/node` már áll (valódi HX711/vbat/ESP-NOW driverek, BENCH profillal kimérve — 8./BENCH), a **`firmware/master` pedig éles és tereptesztelt** (node-vétel + saját mérés + flash-backlog + Firebase-feltöltés + helyi státuszlap — lásd 7. és 10.).

## 2. Kontextus és skála

- **Cél:** saját, hobbi méhészet — 1 node prototípus háza. Prioritás: nyomtathatóság (ASA), megbízhatóság, egyszerűség.
- **Eszköz-stack:** **build123d + ocp-vscode** (élő 3D nézet), Python **3.11** (⚠️ a 3.14-hez nincs ocp wheel). A `.venv` ezen a gépen kész.
- **Munkamódszer + napi parancsok:** lásd `CLAUDE.md` (a kötelező validate → preview → Read-PNG hurok és a token-takarékos `enclosure-builder` delegálás).

## 3. Koordináták + befoglaló — megerősített tények

- **Tengelyek:** X = rövid él (`inner_x=60`), Y = hosszú él (`inner_y=78`, ide a 18650 holder ∥ Y), Z fel. Alj z=0, XY-origóra centrálva.
- **Külső bbox: 72.5 × 86 × 28.5 mm** (a +X fülek +4.5 mm-t nyújtanak).
- **2 nyomtatott part:** `housing`, `bottom_lid` (a korábbi akku-ajtó törölve).

## 4. Jelenlegi felépítés (mind validálva, 1-1 solid)

- **Falak:** `wall = long_wall_th = 4` (egyenletes), `top_th = 3.5`, `lid_th = 1.5`, `inner_z = 25`.
- **Tető-platform (cella-ágy):** besüllyesztett pocket (34.8) + **karima** (teherhordó talp, a felfekvésen kívül kifaragva, csak a boss-collarok maradtak) + **dent** (közép-üreg, ide hajlik a cella) + 2× **M2.5 önmetsző cella-fix boss** (x=0, y=±21.4). A felfekvő gyűrű/dent/karima átmeneteibe fillet (teherúti ív).
- **Y-oszlop (teherút):** álló „Y" a Y–Z síkban, x∈[−6, 6] (nem ér a +X holderhez); profil-sarkok lekerekítve; alján pilot + középső fedélcsavar.
- **Fedél-csavarok:** 5 **BELSŐ** (−X 2 sarok + −X/+Y/−Y közép) félkör (D) collarral; **+X 3 KÜLSŐ FÜL** (a seal-vonalon kívül → IP65 ok, a holder helyét felszabadítja), agresszív fül↔fal kónusszal.
- **bottom_lid:** sík lap + **megemelt belső dugó** (perem tömör + diagonális rács; kihagyások az oszlopnál és a bossoknál).
- **Lekerekítés/kónusz:** külső sarkok, felső perem, fülek, collarok, Y-oszlop, fedél-alj — a **kontakt felületek élesek**.

## 5. Megszületett döntések / tervezési elvek

- **2 part** (`housing` + `bottom_lid`), akku-ajtó nélkül.
- **Kontakt felületek ÉLESEK maradnak**, minden más kerekíthető (nyomtatás-szilárdság). Kontakt = USB-nyílások, furatok belseje, cella-ágy (pocket/dent/karima-felfekvés), ház↔fedél felület + gasket-horony, fül-alja (z=0). Mester kapcsoló: `print_round_enable`.
- **Fillet/chamfer technika** (OCCT korlát): a „minden élre egyszerre" elhasal → szűk, célzott él-csoportok; lyuk- és apró éleket kizárni; **2D-profil fillet az extrude ELŐTT robusztusabb**, mint 3D-élt vágni. (Részletes technika: `CLAUDE.md` / commit-történet.)
- **Statikát/merevítést a USER tervezi** — kérés nélkül NE tegyél vissza merevítést.

## 6. Nyitott kérdések (jövőbeli döntéspontok)

1. **Kábel-bevezetés** (cella-kábel, DS18B20): a −Y PG7 furatok törölve → **mód NYITOTT** (tető / fal / perem?).
2. **Cella-ágy teherúti ívek:** az 1.5 mm fillet csak first-pass — a user szerint **folytonos, nagy sugarú „tölcsér"** kell a felfekvéstől az oszlopig (érdemi profil-átépítés), + a 2 függőleges lightening-hasadék felülvizsgálata. *(A következő geometriai feladat fő tárgya.)*
3. **`batt_z0 = 3`** és a 18650-magasság feltételezés → **éles darabon mérendő** (~1.75 mm/vég a holdernél).
4. **Belső élek** még élesek (collar-tövek, nyitott-alj fal-élek) — kérésre célzottan kerekíthetők.
5. **18650 holder tartója:** a házban nincs fizikai tartó (a user oldja meg); a pozíció-referencia a commit-történetben (a törölt `layout.py` `STRIP_CX`-e).

---

## 7. Rendszerarchitektúra (firmware / elektronika) — papíron eldöntve

> Részletes doksi + BOM-megjegyzések: `docs/new_base_components.md`. BOM-tábla: `docs/alkatreszek.csv`. Ez a megegyezett rendszerterv. A `firmware/node` már fut (BENCH-hel kimérve — 8./BENCH); az `experiments/`-ben futó, mért chunkok vannak (H1/H2/Sz2/M1/M1b — 8.).

- **Topológia:** master–slave csillag (több slave node/kaptár → 1 master). A **master hibrid** (D4): a vétel/időbélyeg/felhőzés mellett **maga is mér** a saját kaptárán, saját `unit_id`-vel. Kommunikáció **ESP-NOW** (slave→master), a master WiFi STA (uplink) + ESP-NOW **azonos csatornán**. **D1: a WiFi STA és az ESP-NOW egyidejű üzeme a terv** (a master mindig kapcsolódva figyel is) — ezt az **Sz2b validálja**; ha nem stabil, **fallback: ablakos** (feltöltéskor az ESP-NOW rövid szünet).
- **Master vételi pipeline:** producer (ESP-NOW callback → `xQueueSendFromISR`, semmi más) / consumer (másik mag: időbélyegez, pufferel, felhőz). A master **hálózati tápon always-on** (nem alszik) → a két mag folyamatosan viszi az RX-et és a felhős I/O-t, egymás akadályozása nélkül. **Időbélyeg a masteré** (`rx_millis`+NTP); a slave-idő deep sleep alatt megbízhatatlan.
- **Adatformátum:** fixed-size `payload_t` (`packed`): `unit_id, seq, weight, temp, vbat`. A `seq` adja a hiány/duplikátum-detektálást.
- **Perzisztencia:** **SD ELHAGYVA.** Tömeg-puffer a master **PSRAM**-jában (~40 nap; always-on masterrel ez immár tisztán **hálózat-/felhő-kimaradás** elleni tartalék — a master maga mindig fut), reset-túlélést **AT24C256 EEPROM checkpoint** (32 KB = 256 kbit, slot-wear-leveling, node-onkénti utolsó `seq`+ts) + `seq`-gapdetektálás ad.
- **Felhőzés:** MQTT QoS 1 / HTTPS, batch-elve, sikeres feltöltés után EEPROM checkpoint.
- **Uplink:** **ZTE MF79U** 4G LTE WiFi stick (Yettel, B20/B8 vidéki sávok; opcionális TS-9 külső antenna). A master a stick **WiFi hotspotjára** csatlakozik STA-ként (nincs USB host gond). Load switch/MOSFET kapcsolja a stick 5V tápját (master GPIO).
- **Master táp — TISZTA HÁLÓZATI (D3, 2026-07-03):** **5V adapter → sima 5V→3.3V konverter** (LDO/buck); **akkus áthidalás/UPS NINCS**. Folyamatos táp → az uplink mindig mehet, a master **always-on** (nincs deep sleep). A korábbi akku/napelem üzemmód elesik; a ~40 napos PSRAM-puffer csak hálózat-/felhő-kimaradás elleni tartalék. Áramszünetkor a master leáll, a node-ok tovább pufferelnek (a `seq`-gap detektálja a kimaradást).
- **Boardok:** node = ESP32-C3 SuperMini (egymagos, deep sleep), master = **ESP32-S3-Zero** (2 MB PSRAM, kétmagos) — hálózati táp/always-on, ezért a deep-sleep áram tárgytalan (a H4 mérés szerint mA-t húzna, de nem alszik; lásd 8./H4).
- **Always-on következmény a timing-rétegre:** mivel a master folyamatosan figyel, a node deep-sleep drift **NEM érinti a vételt** (nincs rendezvous-ablak, amit el lehet véteni), az időbélyeg amúgy is a masteré. Ezért az **Sz3 rendezvous/slot és az Sz4 drift-guard-window nagyrészt felesleges**, az **M1 guard-window kimenete kihasználatlan**. Megmarad: `seq`-gap hiánydetektálás + broadcast-redundancia; nagyobb flottánál a broadcast-ütközés kezelése (jitter/redundancia) maradhat kérdés (a végleges node-szám egyelőre nyitott — konkrét cél-flotta-méret NINCS rögzítve).

**Nyitott architektúra-kérdések:**
1. **MF79U áramszünet utáni auto-recovery** (alacsonyabb prioritás — a stick tiszta hálózati tápon **mindig bekapcsolva** van): egy ritka hálózati áramszünet után magától visszaregisztrál-e a mobilnetre? Ha nem → HTTP POST a `192.168.x.1` admin végpontra. Éles darabon mérendő.
2. ~~Master ESP nyugalmi fogyasztása~~ — **TÁRGYTALAN** (2026-07-03): a master hálózati tápon always-on, nincs akkus táp-mérleg a masteren.
3. **HX711 táp:** tiszta, LDO-ról szűrt 3.3V kell (nem a süllyedő cellasín), különben a `weight` az akku töltöttségével driftel.
4. **ESP-NOW kézbesítés + koegzisztencia:** a node **tűzz-és-felejts** broadcastja (+ `seq`-gap) a fő irány. A **D1 egyidejű WiFi STA + ESP-NOW üzem validálása** és a **broadcast-ütközés kezelése** nagy flottánál az **Sz2b** scope-ja (D2). App-ack csak ha garantált kézbesítés kell.

## 8. Kísérleti eredmények / mérések (`experiments/`)

> A validációs feladatbontás (setup-szintek + chunkok H1–H3 / Sz0–Sz5 / M1) és minden chunk implementációs specifikációja: `experiments/README.md` + chunkonkénti `README.md`. Részletes napló: commit-történet. **Firmware-stack: ESP-IDF**, **PlatformIO**-val fordítva/flashelve (`framework=espidf`, `board=esp32-c3-devkitm-1`, natív `main/` layout `src_dir=main`-nel; `idf.py`-vel is fordul). A `pio` a `~/.platformio/penv/bin/`-ben.

**H1 — node deep-sleep áram (ESP32-C3 SuperMini) — a node-energiamérleg ALAPSZÁMA:**
- **Elért ~185 µA** (3.3V a 3V3 lábra, power-LED leszedve, HX711 modul nélkül). Státusz: *elfogadható* (<200 µA), de NEM *jó* (<50 µA).
- **A fő fogyasztó a USB-Serial-JTAG pad volt (~550 µA)** → a firmware deep sleep előtt lekapcsolja (`sleep_console_usj_pad_backup_and_disable()`); a boot-log USB-n megmarad. (731 µA → 185 µA.)
- **Maradék ~185 µA ≈ AMS1117 on-board LDO (~100 µA) + GPIO/RTC.** A *jó* (<50 µA) szint az LDO leszedését igényli, ami **fejlesztés alatt felejtős** (az LDO adja a 3.3V-ot az USB 5V-ból → leszedve nincs USB-flash). **Döntés: prodra halasztva** (~3 dedikált board beáldozva).
- **HX711 pin-terv (H1 firmware):** SCK=GPIO5, DT(DOUT)=GPIO6. A deep-sleep GPIO-hold a C3-on `gpio_hold_en` (nincs RTCIO). Flashelés tartós deep sleepnél: **BOOT+reset** (a board alvásban nem enumerál USB-n).

**H2 — HX711 power-path (excitation-lekapcsolás) — KÉSZ, PASS (2026-06-28):**
- **HX711 power-down (PD_SCK=HIGH) ténylegesen lekapcsol:** a modul+cella árama **~0-ra esik**, és a deep-sleep (nyugalmi) áram az overall node-fogyasztásban **nem okoz észlelhető növekedést → elhanyagolható** (< pár µA). A modulon nincs LED / szivárgó tápút (egybevág a webes kutatással: olcsó zöld/piros HX711 breakout-on nincs LED, nincs külön LDO; az E+ gerjesztést a chip belső AVDD-regulátora adja, ami power-downban leáll).
- **HX711+cella AKTÍV (powered-up) árama ~5 mA** (HX711 ~1.5 mA + load cell híd ~3.3 mA @ 3.3V) — ennyit kell a node-nak adnia a tényleges mérés idejére; deep sleepben ~0.
- A firmware **két üzemmódot** tud (`H2_MEASURE_MODE`): 0 = deep-sleep/excitation mérés (+ `EXCITATION_PROBE_MS` az „ébren" excitation V-kontrollhoz), 1 = **aktív-ciklus** (ESP ébren, HX711 ciklikus power-up/down az üzemi fogyasztás kiméréséhez). **A forrás default-ja jelenleg MODE 1** (10 s/10 s). A pin-terv és a USB-JTAG-fix/GPIO-hold a H1-ből.
- **Tanulság (zsákutca):** a 10 s/10 ms ciklusnál a power-down „nem látszott lekapcsolni" — a 0,1% duty miatt a DMM átlagolta; **szimmetrikus 10 s/10 s** ciklusban tiszta a leesés. Az excitation explicit V-mérése nem futott, a ~0 mA híd-áram közvetve igazolja.

**M1 — node deep-sleep óra-drift (ESP32-C3 belső RC slow-clock) — KÉSZ, mérve (2026-06-29):**
- **Nyers drift (master-referencia, ~6 órás szobahős futás): +0,60% (+6005 ppm, +21,6 s/óra), R²=1,000000.** A master `esp_timer` kereszt-ellenőrzés +0,603% — egyezik. **Tiszta RC alvás-drift** (a ~93 ms/ciklus awake/boot overhead = ~3085 ppm leválasztva): **+0,29% (+2920 ppm).** A kutatás jósolta 0,5–5% sáv alsó vége — szobahőn ez várható.
- **Hőfüggés NEM jellemezhető** ebből a futásból (a C3 belső die-hő csak 23,8–28,8 °C közt mozgott, R²=0,019; beltéri stabil hő). Tág hő-ingás (kaptár/kültér, nappal-éjjel) kell hozzá. A C3 SuperMini-n nincs külső 32k kristály → a drift kompenzálatlan, és **deep sleep alatt nincs RC-újrakalibráció** (a hő-drift menet közben kompenzálatlan; lásd a kutatás ~1000 ppm/°C becslését).
- **Guard-window (Sz4-nek):** `guard_s(N) = |drift|·N·T + margin` (margin = 3× residual-szórás ≈ 0,34 s). N=10 → ~2 s, N=60 → ~11 s, **N=1440 (12 h @ 30 s) → ~260 s.**
- **Setup:** tűzz-és-felejts beacon — **NINCS Sz3 ACK/slot** (a README azt írta elő, de a master-időbélyeg úgyis a referencia → kivágva). Node deep-sleepből 30 s-onként 1 ESP-NOW broadcast (RTC_DATA_ATTR-megőrzött **abszolút** cycle# + boot_id), ébredésenként 3× redundancia. **A Sz2 PHY-fix** (`set_protocol` LR-nélkül + `esp_now_set_peer_rate_config` 11b/1M_L mindkét oldalon + master PS_NONE) **és a H1 deep-sleep minta** (USB-JTAG-pad lekapcsolás, `gpio_hold`) **újrahasználva.** Offline Python host- ÉS master-referencián illeszt (`m1_drift_analyze.py` + `m1_to_json.py` → CSV + JSON; `log.txt` a nyers log).
- **ANOMÁLIA — 87,9% csomagvesztés (88/726 cycle):** erős RSSI (átlag −30 dBm) mellett sok többperces blackout (max ~34 perc; 0 logger-reconnect → RF/ESP-NOW-RX szintű, nem USB). **A driftet NEM rontja** (abszolút cycle# → a regresszió a meglévő pontokra illeszt, R²=1,0). Ez a **worst-case** setup (C3↔classic-ESP32, ideiglenes pad, másik szoba). **Az adatvesztés-kivizsgálás a VÉGLEGES hardverrel (S3-Zero master + C3 node) külön, az Sz2b alatt — NEM az M1 scope-ja.**

**H4 — master deep-sleep áram (ESP32-S3-Zero) — TÁRGYTALAN (2026-07-03):**
- A mérés kimutatta: az S3-Zero deep sleepben **mA-nagyságrendet** húz (a WS2812 fedélzeti LED SW-kioltása /fekete-frame + data-LOW/hold/ és a 4 MB flash-kényszer / boot-loop-fix UTÁN is). Emiatt egy ideig felmerült a **C3-ra váltás** (2026-07-02 pivot, majd visszavonva).
- **Végül a master HÁLÓZATI TÁPOT kap → always-on, sosem alszik** → a deep-sleep áram irreleváns. Az **S3-Zero marad** (dual-core + PSRAM az always-on RX/uplinkhez). A H4 firmware/mappa historikus artefakt; a chunk lezárva/tárgytalan.

**BENCH — ELSŐ fogyasztásmérés a TELJES node-elektronikán, akkuról (`firmware/node`, 2026-07-04):**
- **Setup:** teljes tápút — 18650 → TP4056 → **TPS63000-M** buck-boost → 3,3 V; **DMM az akku + ágában**, log nélkül (stopper + áram-lépcsők, a BENCH fázis-szekvencer 20 s-os fázisai). Először mértünk a teljes, összeszerelt elektronikán (eddig csak részegységeken: H1/H2).
- **Áram-lépcsők:** baseline (minden ki) **20 mA**; HX711 **24,6 mA** (+4,6 mA — egybevág a H2-es ~5 mA-rel); VBAT **21,3 mA**; DS18B20 (stub) **20,8 mA**; WiFi idle **89 mA**; ESP-NOW TX **90–100 mA**; deep sleep ELŐSZÖR **3,2 mA**.
- **A 3,2 mA alvóáram gyökéroka — MEGOLDVA:** a TPS63000-M modul gyárilag **kényszerített PWM**-ben járt (PS forrpont nyitva). A modulon 4 forrpont: 3 Vout-választó (5V / 4,2V / 3,3V — egyszerre csak EGY lehet zárva!) + 1 **PS pad**. A PS pad átforrasztása (PS/SYNC → GND) → **power-save/PFM mód** → alvóáram **3,2 mA → 750 µA**. Megjegyzés: a `docs/TPS63000-M_EN_10049565.pdf` valójában Hestore-fedlap + TI chip-adatlap; a **modul** forrpontjait a Hestore termékoldal (https://www.hestore.hu/prod_10049565.html) dokumentálja.
- **A 750 µA szétbontása (adatlap-elemzés):** a konverter „legálisan" **~310–450 µA** (PFM hatásfok kis terhelésen ~58–66%; a Vin 3,6–4,2 V buck-boost átmeneti zóna a legrosszabb; quiescent ~46 µA bemenet-oldalon) + SuperMini deep sleep **~185 µA** (H1 referencia; ezen a példányon power LED nincs — ellenőrizve) → **marad ~300–450 µA megmagyarázatlan.** Fő gyanúsított: a **vbat-osztó** (az NPN nem zár alvásban VAGY GPIO0 pad-dióda szivárgás teli akkunál; 3,7 V / 20 kΩ ≈ 185 µA); másodlagos: a TPS modul FB-osztója. **HX711 kizárva** (power-down ~0, H2-ben igazolt).
- **Firmware-fix (a BENCH találta):** a `hx711_power_down()` GPIO-holdja eddig csak `hx711_init()`-ben (reboot után) oldódott fel → reboot nélküli power_down→power_up ciklusnál (6. fázis, ESPNOW_TX) a PD_SCK HIGH-on ragadt, a HX711 power-downban maradt, a read timeout 0-t adott. Fix: `gpio_hold_dis()` + `gpio_deep_sleep_hold_dis()` a `hx711_power_up()` elején — flashelve, igazoltan működik.
- **NYITOTT — következő mérés:** alvás alatt feszültségmérés az osztó alsó 10k-ján (GPIO0–GND): ~0 mV → osztó ártatlan; ~1,8–2 V → NPN nyitva ragadt; köztes → pad-dióda szivárgás. Utána szükség esetén konverter-KIMENETI árammérés. Becsült terepi üzemidő 3,5 Ah-val: 750 µA → **~5–6 hónap**; osztó rendezve (~550 µA) → **~8 hónap**.

**BENCH follow-up — teljes node fogyasztás-debug (`firmware/node`, 2026-07-09):**
- **Aktuális padló:** normál firmware deep sleepben kb. **930–940 µA**; a `esp32-c3-sleep-floor` minimál profil (HX711 power-down + USB-JTAG pad off, minden más kihagyva) ugyanezt hozza. Ez alapján a 940 µA nagyrészt tápút / SuperMini / TPS63000-M kis terhelésű veszteség padlónak látszik, nem BENCH-fázis mellékhatásnak.
- **HX711 pin-mismatch:** a valós PD_SCK **GPIO5**, DOUT **GPIO6**. A korábbi GPIO4/GPIO3 konfig mellett a sleep-floor kb. **1,8 mA** lett, mert a HX711 nem ment power-downba. Javítva: `HX711_PD_SCK_GPIO=GPIO5`, `HX711_DOUT_GPIO=GPIO6`.
- **GPIO0 / VBAT:** a low-side kapcsolt 10k/10k osztó OFF állapotban a GPIO0 mérőcsomópontot VBAT-ra úsztatta; a hardverág le lett kötve. Firmware-ben `VBAT_MEASURE_ENABLED=0`, a payload/log vbat placeholder **0 mV**.
- **GPIO9 / DS18B20:** a hőmérő olvasás jelenleg tiltott (`DS18B20_ENABLED=0`), de deep sleep előtt GPIO9 explicit **HIGH + hold** (`DS18B20_HOLD_HIGH_IN_SLEEP=1`), hogy a 4,7 kΩ felhúzó ne tudjon LOW padon át ~700 µA-t elfűteni.
- **Rossz irány kizárva:** az „összes GPIO input/no-pull” próbálkozás kb. **1,7 mA**-re rontott; C3-on deep sleepben a nem holdolt GPIO-k alapból high-Z, ezért csak a külső fogyasztót vezérlő lábakat szabad célzottan kezelni.
- **Doksi:** részletes jegyzet: `docs/node_power_debug_2026-07-09.md`; GPIO9 boot/fogyasztás jegyzet: `docs/gpio9_ds18b20_boot_test.md`.

## 9. Mappastruktúra és modul-fájlok

Részletes mappa-leírás + munkamódszer: `CLAUDE.md`.

- **`enclosure/`** — 3D modellezés: `enclosure.py` (fő build123d modell, méretek a tetején) + `validate.py`/`preview.py`/`_zsection.py` (validáló + vizuális ellenőrző eszközök) + `datasheets/cell_SC902/` (mérőcella-adatlapok) + `export/` (generált STL/STEP/PNG) + `requirements.txt`.
- **`experiments/`** — node-kísérletek: a validációs chunk-struktúra (H1–H3 / Sz0–Sz5 / M1, lásd `experiments/README.md`). **H1 kész** (deep-sleep áram), **H2 kész** (HX711 power-path), **Sz2 kész** (ESP-NOW alacsony szint, 1000/1000 padon), **M1 kész** (node óra-drift mérve), a többi chunk README-spec.
- **`firmware/`** — a kiérlelt kód: `node/` (ESP-IDF/PlatformIO node-firmware — BENCH fázis-szekvencer + éles profil, valódi HX711/vbat/ESP-NOW driverek; env-ek: `node1`/`node2`/`node3`, `*-test`, `esp32-c3-sleep-floor`) + `master/` (éles master-firmware; prod env **`esp32-s3-zero-local-status`** = cloud + helyi HTTP státuszlap) + `common/` (megosztott protokoll). Flash-workflow és gép-specifikus USB: **10. szakasz**.
- **`docs/`** — `new_base_components.md` (alkatrész-méretek) + `alkatreszek.csv` (BOM: alkatrész / funkció / ár / node–master darab).
- **`agents/enclosure-builder.md`** — a geom. iterációt izolált kontextusban futtató al-ágens.
- A teljes `bee_scale` projekt-kontextusa (mérési lánc, backend, dashboard): a `prototype/` modul (1. fázis).

## 10. Firmware flash-workflow + gép-specifikus USB (üzemeltetési tudás)

> Ez a valódi tereplfash-eléshez felhalmozott gyakorlati tudás ezen a gépen (macOS, Apple Silicon). A firmware build/flash mindig `~/.platformio/penv/bin/pio`-val.

- **Deep-sleep node flashelése (a fő nehézség):** a FIELD-profilú node (`NODE_PROFILE_FIELD`, `wake=1200 s`) a legtöbb időt deep sleepben tölti, és az ESP32-C3 SuperMini **natív USB-Serial/JTAG-ja alvás alatt lekapcsol** → a `/dev/cu.usbmodem*` port eltűnik a gépről, csak a rövid ébrenléti ablakban (mérés+ESP-NOW küldés) villan fel. Emiatt **auto-detecttel gyakorlatilag nem flashelhető**. **Bevált workflow:** (1) a node-t **download/bootloader módba** tenni (BOOT/GPIO9 nyomva + RST, vagy BOOT nyomva bedugás) → a port stabilan enumerál; (2) a firmware-t **előre lefordítani** (`pio run -e nodeN`), hogy az upload már csak az esptool-írás (~7-8 mp) legyen; (3) ha az ébredési ablakot kell elkapni, egy **port-figyelő** (0,2 s poll `ls /dev/cu.usbmodem*`-re) a port megjelenésekor **azonnal** `pio run -e nodeN -t upload --upload-port <port>`-ot indít — modell-körfordulás nélkül belefér a ~20 mp-es ablakba. **Node-azonosítás:** flasheléskor mindig **csak egy** node legyen bedugva (a watcher az első portra ír); a `MAC:` sor az esptool-logban igazolja, melyik fizikai eszközre ment (a 3 node MAC-je eltér).
- **Gép-specifikus USB-eltűnés (visszatérő):** ezen a Mac-en több ESP (nem csak egy) esetén a USB soros port **eltűnik és nem jön vissza** semmilyen beavatkozásra (kábel-/port-/bootloader-váltás sem segít), **csak a gép újraindítása**. Ok: az ESP32-S3/C3 natív USB-Serial/JTAG „piszkos" újra-enumerációja + a macOS USB host-stack (IOUSBHostFamily) beragadása, amit súlyosbít, ha **két processz** (pl. egy futó `pio device monitor` ÉS egy flash) egyszerre nyitja a portot. **Következmény a gyakorlatra:** (1) flash/monitor közben **egyszerre csak egy** processz tartsa a portot — flashelés előtt a `pio device monitor`-t le kell állítani (különben `[Errno 35] Resource temporarily unavailable` / „port is busy"); (2) ha a port „örökre" eltűnt → **reboot** az egyetlen megbízható fix Apple Siliconon (nincs SMC-reset).

## 11. Dashboard / webapp (új frontend)

> Teljes doksi (fájlok, döntések, deploy, új telepítés hozzáadása, nyitott pontok): **`dashboard/README.md`**. A részletes napló a commit-történet (`31194c7 … 08ce881`).

- **Cél/hely:** külön `dashboard/` frontend (branch `dev_02`), ami **egy helyen** követi a két telepítést: **hive1** (régi prototípus, változatlan séma, kalibrált kg, ~5 perc) és **hive2** (új wireless, másik település, 4 egység: master + node_1..3, ~20 perc, jelenleg **nyers HX711 count**).
- **Stack:** vanilla HTML/CSS/ESM JS, build nélkül; Firebase JS SDK **12.4.0** (anonim auth + RTDB) + **ECharts 5.5.1** (CDN). A frontend **read-only**.
- **PROD-védelem (fontos):** a `terrarium1.web.app` **ÉRINTETLEN** — csak Hosting **preview channel** (`v2`) megy ki (`firebase hosting:channel:deploy v2`); a `firebase.json` **szándékosan nem tartalmaz `database` blokkot**, így a deploy sose írhatja felül a prod RTDB Rules-t.
- **Adatréteg = `config.js`** (`window.BEESCALE_CONFIG`): telepítés/egység/RTDB-útvonal/mezőnév/mit-hova-rajzolunk. Új telepítés/node = ide egy bejegyzés, kód nélkül.
- **Megjelenítési döntések:**
  - **Súly = telepítésenként EGY vonal** (össz-súly): az egységek async pontjait közös idővonalra interpoláljuk és összegezzük.
  - **hive2 „becsült kg" placeholder** (`kg = base + (raw − median)·kgPerCount`) — NEM valódi súly, a UI jelöli. A **valódi kalibráció + kg-konverzió + hőm-kompenzáció a felhőben** (Cloud Function) lesz, később (kiindulás ~1031 count/°C, M1b). Amint kész, a hive2 `weightUnit` "raw"→"kg" és a placeholder kivehető.
  - **Hőm-chart = csak valós környezeti hőm:** levegő (AHT20) + kaptár-szonda (DS18B20). A node-ok `temp_c`-je és a `scale_c` mérleg-hőm **kalibrációs/hőm-kompenzációs adat → nem rajzoljuk.**
  - **Fotó-szerű navigáció:** X-tengely (idő) pinch-zoom + pan, Y mindig a látható adathoz igazodik (a vonal nem csúszik ki); nincs alsó csúszka, nincs érték-tooltip. **Kritikus tanulság:** a kétujjas pinch csak akkor jut az ECharts-hoz, ha a **belső `<canvas>`** `touch-action:none` (nem elég a külső div), + iOS `gesture*` események JS-letiltása.
  - Interpolációs **pont-sűrítés** (`densify`, 5 perc): a ritka mérések közé beszúrt pontok, hogy nagyításnál is legyen mire ránagyítani.
- **Nyitott:** felhős kalibráció+hőm-kompenzáció (fő hátralévő); a **hive2 Firebase-séma nem végleges** (a felhős feldolgozáshoz átalakítható); link-minőség (rssi/gap) csak master-firmware feltöltéssel; a JS-runtime validáció felhasználói preview-visszajelzésen (nincs headless böngésző).

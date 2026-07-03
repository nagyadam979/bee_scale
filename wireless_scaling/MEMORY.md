# bee_scale · wireless_scaling — Modul memória

> Tartós alapdokumentum a node-ház (2. fázis) almodulhoz. Ez orientálja a jövőbeli munkamenetet: cél, döntések, geometria-állapot, nyitott kérdések. Frissítsd, amikor egy döntés megszületik vagy egy geometriai fázis lezárul. A **részletes napló a commit-történet** (mit/miért/mi-működött/mi-nem).
> Utolsó frissítés: 2026-07-03 (**master HÁLÓZATI TÁP → always-on**: a master ESP32-S3-Zero marad /dual-core + PSRAM/, mert a deep-sleep áram tárgytalan; ez egyszerűsíti a rendezvous/drift-réteget is — lásd 7. + 8./H4. Korábban 2026-06-29: M1 chunk — node óra-drift; H1 + H2 + Sz2; geometria a 2026-06-23-i állapot)

> **Státusz:** a geometria **stabil és validált** — 2 nyomtatott part (`housing`, `bottom_lid`), 1-1 összefüggő solid. **Jelenleg senki nem dolgozik a modell geometriáján** — a következő geometriai munka KÜLÖN döntéssel indul. A modell forrása: `enclosure.py` (minden méret a fájl tetején, kommentált változó).

---

## 1. A modul egy mondatban

A méhkaptár-mérleg **vezeték nélküli node** teljes fejlesztése — elektronika (firmware), kísérletek és a **3D nyomtatott ház**. Jelenleg a 3D ház (parametrikus, ~IP65, ASA, build123d; mérőcella + ESP32 + 18650 akku befogadása) a kész/aktív rész; a **rendszerarchitektúra papíron eldöntve** (lásd 7. szakasz), de a `firmware/` és `experiments/` kód még előttünk áll.

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

> Részletes doksi + BOM-megjegyzések: `docs/new_base_components.md`. BOM-tábla: `docs/alkatreszek.csv`. **Kód még nincs** — ez a megegyezett terv, nem implementáció.

- **Topológia:** master–slave csillag (több slave node/kaptár → 1 master). Kommunikáció **ESP-NOW** (slave→master), a master WiFi STA + ESP-NOW **azonos csatornán** (különben csomagvesztés; ez kész/tesztelt).
- **Master vételi pipeline:** producer (ESP-NOW callback → `xQueueSendFromISR`, semmi más) / consumer (másik mag: időbélyegez, pufferel, felhőz). A master **hálózati tápon always-on** (nem alszik) → a két mag folyamatosan viszi az RX-et és a felhős I/O-t, egymás akadályozása nélkül. **Időbélyeg a masteré** (`rx_millis`+NTP); a slave-idő deep sleep alatt megbízhatatlan.
- **Adatformátum:** fixed-size `payload_t` (`packed`): `unit_id, seq, weight, temp, vbat`. A `seq` adja a hiány/duplikátum-detektálást.
- **Perzisztencia:** **SD ELHAGYVA.** Tömeg-puffer a master **PSRAM**-jában (~40 nap; always-on masterrel ez immár tisztán **hálózat-/felhő-kimaradás** elleni tartalék — a master maga mindig fut), reset-túlélést **AT24C256 EEPROM checkpoint** (32 KB = 256 kbit, slot-wear-leveling, node-onkénti utolsó `seq`+ts) + `seq`-gapdetektálás ad.
- **Felhőzés:** MQTT QoS 1 / HTTPS, batch-elve, sikeres feltöltés után EEPROM checkpoint.
- **Uplink:** **ZTE MF79U** 4G LTE WiFi stick (Yettel, B20/B8 vidéki sávok; opcionális TS-9 külső antenna). A master a stick **WiFi hotspotjára** csatlakozik STA-ként (nincs USB host gond). Load switch/MOSFET kapcsolja a stick 5V tápját (master GPIO).
- **Master táp — HÁLÓZATI (eldöntve 2026-07-03):** folyamatos hálózati táp → az uplink mindig mehet, a master **always-on** (nincs deep sleep). A korábbi akku/napelem üzemmód (napi 1 ablak) elesik; a ~40 napos PSRAM-puffer így csak hálózat-/felhő-kimaradás elleni tartalék.
- **Boardok:** node = ESP32-C3 SuperMini (egymagos, deep sleep), master = **ESP32-S3-Zero** (2 MB PSRAM, kétmagos) — hálózati táp/always-on, ezért a deep-sleep áram tárgytalan (a H4 mérés szerint mA-t húzna, de nem alszik; lásd 8./H4).
- **Always-on következmény a timing-rétegre:** mivel a master folyamatosan figyel, a node deep-sleep drift **NEM érinti a vételt** (nincs rendezvous-ablak, amit el lehet véteni), az időbélyeg amúgy is a masteré. Ezért az **Sz3 rendezvous/slot és az Sz4 drift-guard-window nagyrészt felesleges**, az **M1 guard-window kimenete kihasználatlan**. Megmarad: `seq`-gap hiánydetektálás + broadcast-redundancia; nagy flottánál (12 node) a broadcast-ütközés kezelése (jitter/redundancia) maradhat kérdés.

**Nyitott architektúra-kérdések:**
1. **MF79U firmware** tápra-felregisztrálása: tápot adva jön-e net beavatkozás nélkül? (Ha nem → HTTP POST a `192.168.x.1` admin végpontra, vagy „mindig bekapcsolva" üzem.) — éles darabon mérendő.
2. ~~Master ESP nyugalmi fogyasztása~~ — **TÁRGYTALAN** (2026-07-03): a master hálózati tápon always-on, nincs akkus táp-mérleg a masteren.
3. **HX711 táp:** tiszta, LDO-ról szűrt 3.3V kell (nem a süllyedő cellasín), különben a `weight` az akku töltöttségével driftel.
4. **ESP-NOW kézbesítés:** az always-on master (mindig figyel) mellett a node **tűzz-és-felejts** broadcastja (+ `seq`-gap az elemzésben) a fő irány — nem kell a node-ébredést a master ablakához igazítani. App-ack csak ha garantált kézbesítés kell; nagy flottánál a broadcast-ütközés kezelése a nyitott pont.

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
- **ANOMÁLIA — 87,9% csomagvesztés (88/726 cycle):** erős RSSI (átlag −30 dBm) mellett sok többperces blackout (max ~34 perc; 0 logger-reconnect → RF/ESP-NOW-RX szintű, nem USB). **A driftet NEM rontja** (abszolút cycle# → a regresszió a meglévő pontokra illeszt, R²=1,0). Ez a **worst-case** setup (C3↔classic-ESP32, ideiglenes pad, másik szoba). **Az adatvesztés-kivizsgálás a VÉGLEGES hardverrel (ESP32-S3-Zero master) külön, Sz2/Sz3 alatt — NEM az M1 scope-ja.**

**H4 — master deep-sleep áram (ESP32-S3-Zero) — TÁRGYTALAN (2026-07-03):**
- A mérés kimutatta: az S3-Zero deep sleepben **mA-nagyságrendet** húz (a WS2812 fedélzeti LED SW-kioltása /fekete-frame + data-LOW/hold/ és a 4 MB flash-kényszer / boot-loop-fix UTÁN is). Emiatt egy ideig felmerült a **C3-ra váltás** (2026-07-02 pivot, majd visszavonva).
- **Végül a master HÁLÓZATI TÁPOT kap → always-on, sosem alszik** → a deep-sleep áram irreleváns. Az **S3-Zero marad** (dual-core + PSRAM az always-on RX/uplinkhez). A H4 firmware/mappa historikus artefakt; a chunk lezárva/tárgytalan.

## 9. Mappastruktúra és modul-fájlok

Részletes mappa-leírás + munkamódszer: `CLAUDE.md`.

- **`enclosure/`** — 3D modellezés: `enclosure.py` (fő build123d modell, méretek a tetején) + `validate.py`/`preview.py`/`_zsection.py` (validáló + vizuális ellenőrző eszközök) + `datasheets/cell_SC902/` (mérőcella-adatlapok) + `export/` (generált STL/STEP/PNG) + `requirements.txt`.
- **`experiments/`** — node-kísérletek: a validációs chunk-struktúra (H1–H3 / Sz0–Sz5 / M1, lásd `experiments/README.md`). **H1 kész** (deep-sleep áram), **H2 kész** (HX711 power-path), **Sz2 kész** (ESP-NOW alacsony szint, 1000/1000 padon), **M1 kész** (node óra-drift mérve), a többi chunk README-spec.
- **`firmware/`** — a node-firmware (jelenleg üres — ide a kiérlelt kód).
- **`docs/`** — `new_base_components.md` (alkatrész-méretek) + `alkatreszek.csv` (BOM: alkatrész / funkció / ár / node–master darab).
- **`agents/enclosure-builder.md`** — a geom. iterációt izolált kontextusban futtató al-ágens.
- A teljes `bee_scale` projekt-kontextusa (mérési lánc, backend, dashboard): a `prototype/` modul (1. fázis).

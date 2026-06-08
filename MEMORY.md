# bee_scale — Projekt memória

> Tartós alapdokumentum. Ez orientálja a jövőbeli munkamenetet: cél, döntések, hardver, nyitott kérdések. Frissítsd, amikor egy döntés megszületik vagy egy fázis lezárul.
> Utolsó frissítés: 2026-05-31 (web dashboard kész + élesben)

> **Státusz-frissítés (2026-05-31):** A mérési lánc (cella → HX711 → ESP32, kalibrációval) **bizonyítottan működött** egy korábbi kódbázisban — a mérlegek tökéletesen mérnek. A firmware kódja elveszett, **de megmaradt: a kalibrációs faktorok, a bekötés/pinout, és kódrészletek/jegyzetek.** Ezért a firmware újraépítése **rutinfeladat, alacsony kockázat — NEM ez a projekt fókusza.** A valódi tervezési munka a **távoli elérés stack: kommunikáció + backend + dashboard + riasztás.**

---

## 1. A projekt egy mondatban

Okos méhkaptár-mérleg: folyamatosan méri egy kaptár súlyát, és az adatot interneten keresztül, távolról elérhetővé teszi — megfigyelésre és riasztásra (hordás/fogyás trend, rajzás, lopás, méhcsalád állapota).

## 2. Kontextus és skála

- **Cél:** saját, hobbi méhészet. Prioritás: egyszerűség, megbízhatóság, alacsony költség — nem kereskedelmi termék.
- **Skála:** most **1 kaptár, prototípus**. (Bővítés nem cél, de a túlbonyolítást kerüljük.)
- **Helyszín:** van hálózati **áram**. WiFi még nincs kiépítve, de **tervezett** (ehhez igazítjuk a kommunikációt).
- **Távoli elérés szintje:** **megfigyelés + riasztás**. Read-only dashboard a trenddel + értesítések (pl. hirtelen súlyesés = rajzás/lopás, alacsony akku/hiba). Távoli vezérlés egyelőre nem cél.

## 3. Hardver — megerősített tények

| Elem | Állapot | Megjegyzés |
|---|---|---|
| Mérőcellák | **Megvan** | 2 db rúdszerű (rod) mérőegység, kaptáranként, **fél-hidas (half-bridge)** cellákkal. Két rúd megy egy kaptár alá (kábelezés/mechanika miatt külön egységként). |
| Mérőerősítő / ADC | **Megvan** | **2× HX711** (rudanként egy). Két fél-híd → összegezhető teljes súllyá. |
| Mikrokontroller | **Megvan** | **ESP32** (vagy változata) — beépített WiFi, ami illeszkedik a tervezett WiFi-s elérlhez. |

**Jegyzet a „két pár" megfogalmazáshoz:** a meglévő setup = 2 mérőrúd + 2 HX711. A pontos topológia (egy ESP32 olvassa-e mindkét HX711-et, vagy rudanként külön MCU) még **nyitott** — lásd 5. pont.

## 4. Megszületett döntések

- Hobbi léptékű, 1 kaptáras prototípussal kezdünk.
- ESP32 + 2× HX711 + 2 fél-hidas mérőrúd a kiindulási hardver.
- Adatkapcsolat iránya: WiFi (kiépítendő).
- Felhasználói oldal: read-only dashboard (trend). Riasztás egyelőre NINCS — később ráépíthető (lásd 5/#6).
- **Backend (eldöntve, 2026-05-31):** Firebase, **Spark (no-cost) csomag**. Adat-only architektúra:
  - ESP32 → **Realtime Database (RTDB)** közvetlen REST write, **Auth tokennel** hitelesítve, **Security Rules**-szal védve (ESP32 csak ír a saját ágába, web app csak olvas).
  - Megjelenítés: **Firebase Hosting** statikus web app (Chart.js), élőben olvassa az RTDB-t.
  - **Cloud Functions / Blaze NEM kell** (nincs szerveroldali logika) → marad ingyenes, bankkártya nélkül.
  - Kerülendő a Spark-on: Cloud Storage (2026-02-03 óta Blaze-hez kötött), Cloud Functions (deploy Blaze-t igényel).
- **Több eszköz (később):** UID-scope a Rules-ban — `/hives/{uid}/...`, ahol `auth.uid === uid` (eszközönként külön user, mindenki csak a saját ágába ír).
- **Adatmodell:** `/hives/hive1/readings/{pushId}: { ts, w_total, w1, w2 }` — timestamp + összsúly + rúdankénti részsúlyok (a 2 HX711 külön is látszik). A séma úgy készül, hogy a riasztás később rework nélkül ráépíthető.
- **Web dashboard (KÉSZ + élesben, 2026-05-31, Feladat 02):** statikus app a Firebase Hostingon — **élő URL: https://terrarium1.web.app** (`terrarium1` projekt). Vanilla JS, Firebase JS SDK (modular CDN) + Chart.js; read-only, indexelt `ts` query, realtime listener, 24h/7d/30d/összes ablak, w1/w2 toggle, helyi időzóna. Kód: `public/`, `firebase.json`, `.firebaserc`, `README.md`.
  - **Belépés: ANONIM Auth** (nincs login képernyő, auto-csatlakozás). A Rules `auth != null` változatlan; az anonim token kielégíti. **Egyszeri Console-toggle kell:** Authentication → Sign-in method → Anonymous → Enable (engedélyezve). Az írás-védelem érintetlen: csak az `esp32@beescale.local` device-user írhat. Trade-off: aki tudja az URL-t, látja a súly-adatot (alacsony érzékenység → elfogadott).

## 5. Nyitott kérdések (jövőbeli döntéspontok)

1. **MCU-topológia:** egy ESP32 / kaptár olvassa mindkét HX711-et (legvalószínűbb, elég GPIO van), vagy rudanként külön MCU? → eldöntendő a prototípus huzalozása előtt.
2. **Backend / tárolás / dashboard:** ✅ **ELDÖNTVE (2026-05-31): Firebase Spark** — RTDB (adat) + Hosting (web app, Chart.js) + Auth, adat-only, Cloud Functions/Blaze nélkül. Részletek a 4. szakaszban.
3. **Kalibráció:** kétpontos kalibráció (tara + ismert súly), hőmérséklet-kompenzáció kérdése (HX711 + cella drift). → később.
4. **Energiagazdálkodás:** van áram, de tartalék/áthidalás (áramszünet) kérdése. → később.
5. **Mintavételezés:** ✅ **ELDÖNTVE: 5 perc** küldési periódus (~288 minta/nap, ~105k/év — az 1 GB RTDB-limit évekig kényelmes).
6. **Riasztási logika:** **elhalasztva** — most adat-only + vizuális megfigyelés. Ha lesz igény, később ráépül (a séma erre felkészített). A csatorna/logika akkor dől el.

## 6. Az asszisztens szerepe és munkamódszere

- **Madártávlat:** magas szintű architektúra és tervezés, feladatok kidolgozása és **delegálása kódoló ágenseknek**. Nem én írom a teljes kódot — feladatokra bontok és átadok.
- **Időzítés:** konkrét szoftver-/hardverötleteket **csak akkor**, amikor az adott fázis odaér — nem előre.
- **Kommunikáció:** a felhasználó **erős HW + SW** háttérrel rendelkezik → tömör, technikai nyelv, magyarázat-túltengés nélkül. Magyarul.
- **Stílus:** tömör és direkt; döntéseket választási lehetőségként tárok fel, nem egyoldalúan.

## 7. Tervezett fázisok (nagyvonalú vázlat, nem véglegesített)

1. **Alapozás** — cél, hardver, architektúra rögzítése. *(itt tartunk)*
2. **Mérési lánc** — cella → HX711 → ESP32 olvasás, kalibráció, megbízható súlyérték. *(koncepció IGAZOLT korábban; firmware ÚJRAÉPÍTENDŐ, mert a kód elveszett)*
3. **Kommunikáció** — ESP32 → backend adatküldés (WiFi).
4. **Backend + tárolás** — adatbázis/idősoros tárolás.
5. **Dashboard + riasztás** — megjelenítés ✅ **KÉSZ** (https://terrarium1.web.app, anonim auth); riasztás elhalasztva (lásd 5/#6).
6. **Terepi megbízhatóság** — energia, hibakezelés, watchdog, OTA.

---

## 8. Projekt memóriafájlok (külső hivatkozások)

- [Teszt WiFi](project_wifi_test.md) — AMS-TP-2.4GHz, ideiglenes kísérleti hálózat

---

### Glosszárium
- **Fél-híd (half-bridge):** 2 ellenállásos nyúlásmérő ág; két fél-híd ad ki egy teljes Wheatstone-hidat.
- **HX711:** load-cell célú 24 bites ADC + erősítő.
- **Tara:** üres/nulla súly levonása (a kaptár saját tömege).

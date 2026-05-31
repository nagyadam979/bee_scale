# Feladat 02 — Web dashboard (Firebase Hosting + RTDB read)

> Delegálható implementációs feladat egy kódoló (implementáló) ágensnek.
> Olvasd be a `MEMORY.md`-t a projekt-kontextushoz, és maradj e feladat scope-jában.
> Készítette (orchestrátor): 2026-05-31. Az ESP32→RTDB feltöltő modul **kész és működik** (lásd `firmware/`).

## Cél
Statikus web app a **Firebase Hostingon**, ami a feltöltött méréseket **kirajzolja** (súly-trend), **Firebase Auth mögött**, read-only. Nincs riasztás, nincs írás, nincs Cloud Function.

## Kontextus — a backend KÉSZ, ez a ground truth
- Backend: **Firebase Spark (no-cost)**, projekt `terrarium1`.
- **Adat-útvonal:** `/hives/hive1/readings/{pushId}`
- **Rekord séma** (a firmware ténylegesen ezt írja):
  ```json
  { "ts": 1717160400, "w_total": 42.13, "w1": 21.02, "w2": 21.11 }
  ```
  - `ts` = **UTC epoch másodperc** (a megjelenítés kezeli a helyi időzónát).
  - `w_total`, `w1`, `w2` = kg (number, 2 tizedes). `w_total` az össz-súly, `w1`/`w2` a két mérőrúd.
- **Mintavétel:** 5 percenként egy rekord.
- **Security Rules (már fent vannak, `firebase/database.rules.json`):**
  - olvasás: `auth != null` (bármely hitelesített user olvashat),
  - írás: kizárólag a `esp32@beescale.local` eszköz-user,
  - `readings` `.indexOn: ["ts"]` — tehát `ts` szerinti query indexelt.
- **Cloud Functions / Blaze NEM használható.** Maradj Sparkon.

## Bemenet (a felhasználó biztosítja)
- **`firebaseConfig`** a web apphoz (apiKey, authDomain, databaseURL, projectId, storageBucket, messagingSenderId, appId). A web `apiKey` **nem titok** → bekerülhet a kódba; a védelmet a Rules + Hosting-domain adja.
- **Viewer user**: a Firebase Authentication → Users alatt kell egy **read-only belépő** (email/jelszó) a dashboardhoz. **A device-user hitelesítőt (`esp32@...`) NE használd a frontenden.** (Ha még nincs viewer user, a README írja le a létrehozását.)
- `HIVE_ID` = `hive1`.

## Elvárt kimenet (deliverable)
1. **Statikus web app** a `public/` mappában — `public/index.html`, lehetőleg **önállóan** (Firebase JS SDK **modular, CDN import** + **Chart.js CDN**), külön build-lépés nélkül. Vanilla JS elég, ne húzz be framework-toolchaint.
2. **Login gate**: email/jelszó bejelentkező képernyő (Firebase Auth). Bejelentkezés után tölti az adatot; kijelentkezés gomb.
3. **Adatlekérés RTDB-ből**:
   - Az utolsó **N** rekord ill. egy **időablak** a `/hives/hive1/readings`-ből — `ts` szerinti query-vel (`orderByChild('ts')`, `startAt(...)`), kihasználva az `.indexOn: ["ts"]`-t. **Ne** húzd le az egész `readings` ágat (most kicsi, de szokjon hozzá a skálázáshoz).
   - **Élő frissítés**: realtime listener (`onValue` / `onChildAdded`), hogy az új 5 perces minták maguktól megjelenjenek a grafikonon. Legyen egy **Reload** gomb is fallbacknek.
4. **Grafikon (Chart.js)**: vonaldiagram, X = idő (a `ts` UTC epoch sec → **helyi időzónára** konvertálva), Y = kg.
   - Alap sorozat: **`w_total`**.
   - **Toggle** a `w1` és `w2` külön sorozatok megjelenítésére.
5. **Időablak választó**: `24h / 7 nap / 30 nap / összes`. **Alapértelmezett: 7 nap.**
6. **Fejléc/UX**: a legutóbbi mért érték (`w_total`) + időbélyeg kiemelve; üres-állapot és "nincs friss adat" kezelése; egyszerű, tiszta megjelenés.
7. **Firebase Hosting deploy**: `firebase.json` + `.firebaserc` (public = `public/`, SPA rewrite nem kötelező). README a deploy lépésekkel (`firebase login`, `firebase init hosting` vagy kész config, `firebase deploy`). A Hosting Sparkon ingyenes.

## Korlátok
- **Csak olvasás** — a frontend SOHA ne írjon az RTDB-be.
- **Ne** használd a device-user hitelesítőt a frontenden (külön viewer user).
- **Ne** vezess be riasztást / Cloud Functiont / FCM-et / Blaze-függő szolgáltatást.
- A `firebaseConfig` mehet a kódba (web apiKey nem titok), de **service account kulcsot SOHA** ne tegyél a frontendbe/repóba.
- Ne módosítsd a `firmware/`-t és a `database.rules.json`-t (ha a dashboardhoz Rules-változás kellene, **jelezd**, ne csendben írd át).
- A scope-ot ne lépd túl; ha valami ütközik a `MEMORY.md`-vel vagy hiányos → **állj meg és kérdezz**.

## Kész-kritérium
- A Hostingra deployolt URL-en **belépés után megjelenik a súly-trend** a valós RTDB-adatból, helyes időzónával.
- **Új minta** (firmware push) ~automatikusan megjelenik (realtime listener), és a Reload gomb is működik.
- Az **időablak-váltás** és a **w1/w2 toggle** működik.
- **Nem hitelesített** látogató nem lát adatot (login gate + Rules — gyors negatív teszt: kijelentkezve nincs adat).
- A repo nem tartalmaz service-account kulcsot vagy más titkot.

## Mellékelt deliverable-ek
- `public/index.html` (+ esetleg `public/app.js`, `public/style.css`, ha nem egy fájl).
- `firebase.json`, `.firebaserc`.
- Rövid `README`: viewer user létrehozása, `firebaseConfig` behelyettesítése, `firebase deploy`.

## Nyitott / később (NE most)
- Riasztás (küszöb-súlyesés, comms-loss) — külön fázis, a séma erre felkészített.
- Downsampling/retenció az 1 GB RTDB-limit ellen — amíg kicsi az adat, nem sürget.

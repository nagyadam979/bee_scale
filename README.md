# bee_scale — Web dashboard (Firebase Hosting + RTDB read)

Statikus, read-only web app: a kaptármérleg súly-trendjét rajzolja ki, élőben az
RTDB-ből (Feladat 02). Backend: **Firebase Spark (no-cost)**, projekt `terrarium1`.
Nincs írás, nincs riasztás, nincs Cloud Function.

> **Belépés:** bejelentkezés nélküli — az app **anonim Firebase Auth**-tal csatlakozik
> automatikusan (a Rules `auth != null`-t várnak az olvasáshoz, ezt az anonim token
> kielégíti). Aki tudja az URL-t, látja a súly-adatot; írni továbbra is csak a
> device-user tud. (Korábban email/jelszós login gate volt — lásd a git-history-t.)

## Mit tud

- **Auto-csatlakozás** anonim Auth-tal — nincs bejelentkező képernyő.
- **Súly-trend** Chart.js vonaldiagramon: X = idő (a `ts` UTC epoch sec → **helyi
  időzóna**), Y = kg. Alap sorozat `w_total`, **toggle** a `w1`/`w2` rúdankénti sorozatra.
- **Időablak**: 24 óra / 7 nap / 30 nap / összes (alapértelmezett: **7 nap**).
- **Élő frissítés**: realtime listener (`onChildAdded`) — az új 5 perces minták maguktól
  felkerülnek. **Frissítés** gomb fallbacknek.
- **Fejléc**: legutóbbi `w_total` + helyi időbélyeg; üres-állapot és "nincs friss adat" banner.
- **Indexelt query**: `orderByChild('ts')` + `startAt(...)` / `limitToLast(...)` —
  sosem húzza le a teljes `readings` ágat (kihasználja a `.indexOn: ["ts"]`-t).

## Fájlok

```
public/index.html   markup + CDN (Chart.js, date-fns adapter)
public/app.js        firebase init, auth gate, query, chart, live listener (ES module, build nélkül)
public/style.css     stílus
firebase.json        Hosting konfig (public = public/)
.firebaserc          default projekt: terrarium1
```

A `firebaseConfig` már **be van írva** az `app.js`-be (a web `apiKey` **nem titok** — a
védelmet a Security Rules `auth != null` + a Hosting-domain adja). Service-account kulcs
**nincs** és soha ne is kerüljön a frontendbe/repóba.

## Egyszeri előfeltétel: Anonymous Auth engedélyezése

Az app anonim Auth-tal csatlakozik, ezért a Console-ban **egyszer** engedélyezni kell:

**Firebase Console → Authentication → Sign-in method → Anonymous → Enable.**

Ha nincs engedélyezve, a splash képernyő hibát ír ki (`operation-not-allowed`).

## Deploy (Firebase Hosting — Sparkon ingyenes)

```bash
firebase login                 # ha még nem vagy bejelentkezve
firebase deploy --only hosting # a .firebaserc miatt a terrarium1 projektre megy
```

A `firebase init hosting` **nem szükséges** — a `firebase.json` + `.firebaserc` készen van.
A deploy a végén kiírja a **Hosting URL**-t (`https://terrarium1.web.app`), azon nyílik a dashboard.

> Csak a Hostingot deployoljuk (`--only hosting`). Az RTDB Security Rules-t a firmware-task
> kezeli (`firebase/database.rules.json`); ezt a task **nem** módosítja és nem deployolja.

## Használat

1. Nyisd meg a Hosting URL-t → az app **automatikusan csatlakozik** (anonim Auth), nincs belépés.
2. Megjelenik a 7 napos súly-trend; váltogasd az időablakot, kapcsold be a `w1`/`w2` sorozatot.
3. Az új 5 perces minták maguktól felkerülnek; a **Frissítés** gomb azonnal újratölt.

## Korlátok (scope)

- Csak olvasás — a frontend soha nem ír az RTDB-be.
- Nincs riasztás / Cloud Function / FCM / Blaze-függő szolgáltatás (Spark-kompatibilis).
- "Összes" időablak biztonsági plafon: `ALL_LIMIT = 10000` rekord (a séma jelenleg messze alatta).

# bee_scale — Web dashboard (Firebase Hosting + RTDB read)

Statikus, read-only web app: a kaptármérleg súly-trendjét rajzolja ki **Firebase Auth
mögött**, élőben az RTDB-ből (Feladat 02). Backend: **Firebase Spark (no-cost)**,
projekt `terrarium1`. Nincs írás, nincs riasztás, nincs Cloud Function.

## Mit tud

- **Login gate** (Firebase Auth, email/jelszó) — kijelentkezve nincs adat.
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

## Egyszeri előfeltétel: viewer user

A dashboard a Rules `auth != null` olvasási feltétele miatt **bejelentkezést igényel**.
**A device-user (`esp32@beescale.local`) hitelesítőt NE használd a frontenden** — kell egy
külön, read-only **viewer** user. (Az írást a Rules csak a device-usernek engedi, a viewer
csak olvasni tud — emiatt biztonságos.)

Létrehozás (Firebase Console): **Authentication → Users → Add user** → e-mail + erős jelszó.

CLI/REST alternatíva (a web apiKey-vel, mert az Email/Password szolgáltató engedélyezve van):

```bash
API_KEY="AIzaSyC2wncwThhpj8GY4cIXPFjNPhaJv4OAAUM"
curl -s -X POST \
  "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=$API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"email":"viewer@beescale.local","password":"<erős-jelszó>","returnSecureToken":true}'
```

> **Aktuálisan használt teszt-viewer:** `viewer@beescale.local` (a jelszót éles
> használat előtt érdemes lecserélni a Console-ban vagy törölni, ha nem kell).

## Deploy (Firebase Hosting — Sparkon ingyenes)

```bash
firebase login                 # ha még nem vagy bejelentkezve
firebase deploy --only hosting # a .firebaserc miatt a terrarium1 projektre megy
```

A `firebase init hosting` **nem szükséges** — a `firebase.json` + `.firebaserc` készen van.
A deploy a végén kiírja a **Hosting URL**-t (`https://terrarium1.web.app`), azon nyílik a dashboard.

> Csak a Hostingot deployoljuk (`--only hosting`). Az RTDB Security Rules-t a firmware-task
> kezeli (`firebase/database.rules.json`); ezt a task **nem** módosítja és nem deployolja.

## Belépés és használat

1. Nyisd meg a Hosting URL-t → **belépés** a viewer userrel.
2. Megjelenik a 7 napos súly-trend; váltogasd az időablakot, kapcsold be a `w1`/`w2` sorozatot.
3. Az új 5 perces minták maguktól felkerülnek; a **Frissítés** gomb azonnal újratölt.
4. **Kilépés** → újra a login képernyő, adat nélkül.

## Korlátok (scope)

- Csak olvasás — a frontend soha nem ír az RTDB-be.
- Nincs riasztás / Cloud Function / FCM / Blaze-függő szolgáltatás (Spark-kompatibilis).
- "Összes" időablak biztonsági plafon: `ALL_LIMIT = 10000` rekord (a séma jelenleg messze alatta).

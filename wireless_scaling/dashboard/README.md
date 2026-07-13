# bee_scale · dashboard — új webapp

Új frontend a méhkaptár-mérleghez, ami **egy helyen** követi a két telepítést
(**hive1** = régi prototípus, **hive2** = új wireless setup). A meglévő prod
(`https://terrarium1.web.app/`) **érintetlen** marad — ez teljesen külön kód,
külön deploy-célra.

## Hol dolgoztunk

- **Mappa:** `wireless_scaling/dashboard/` (ezen a repón belül, külön a firmware-től).
- **Branch:** `dev_02`.
- **Deploy:** Firebase Hosting **preview channel** (`v2`), NEM a live.
  Aktuális URL: `https://terrarium1--v2-7aoelxy4.web.app` (a channel URL a
  `firebase hosting:channel:deploy v2` kimenetében jelenik meg, ~30 napig él).
- **Prod-védelem:** a `firebase.json` **szándékosan csak `hosting` blokkot**
  tartalmaz, `database` blokkot NEM — így egy deploy soha nem írhatja felül a
  prod RTDB Security Rules-t. A live release-t ez a projekt nem érinti (csak
  channel deploy megy).

## Stack

Build-lépés **nincs** (mint a régi appnál), tiszta statikus fájlok:

- **HTML/CSS/vanilla JS** (ESM), CDN-ről:
  - Firebase JS SDK **12.4.0** (modular ESM, gstatic) — app / auth / RTDB.
  - **ECharts 5.5.1** (UMD global, jsdelivr) — a chartok (natív touch-zoom).
- **Auth:** anonim belépés (a RTDB Rules `auth != null`-t vár olvasáshoz).
  A web `apiKey` NEM titok; a védelmet a Rules + a Hosting-domain adja.
- A frontend **read-only**: soha nem ír az RTDB-be.

## Fájlok

| Fájl | Szerep |
|------|--------|
| `public/index.html` | Váz: splash (auth) → app; topbar (telepítés-tabok + frissítés), időjárás-sáv, súly-chart, hőm-chart, állapot-kártyák. |
| `public/config.js` | **Adatréteg-konfiguráció** (`window.BEESCALE_CONFIG`). Itt van leírva minden telepítés/egység, RTDB-útvonal, mezőnév, mit hova rajzolunk. Új telepítés/node = ide egy bejegyzés, kód-módosítás nélkül. |
| `public/app.js` | Logika: auth, adatlekérés, súly-normalizálás, chartok, kártyák. |
| `public/style.css` | Sötét, mobil-first téma. |
| `firebase.json` | Csak `hosting` (public: `public`). **Nincs** `database` blokk (prod-védelem). |
| `.firebaserc` | Projekt: `terrarium1`. |

## Adatmodell (config-vezérelt)

Két telepítés a `hives` tömbben:

- **hive1** (`schema: "legacy"`, `weightUnit: "kg"`): a régi rendszer, változatlanul.
  Két mérőrúd (`w1`, `w2`), már **kalibrált kg** (`w` mező). ~5 perc.
- **hive2** (`schema: "wireless"`, `weightUnit: "raw"`): új setup, másik település.
  4 egység (`master` + `node_1..3`), egyenként saját HX711 → egyetlen `weight_raw`
  **nyers count**. ~20 perc. A `master` hálózati táp (`power: "mains"`), a `node_1`-nek
  nincs akkumérő HW-je (`noVbatHw: true`).

## Kulcs-döntések és megoldások (ebben a session-ben)

1. **Súly = telepítésenként EGYETLEN vonal** (össz-súly), nem több görbe.
   - Az egységek eltérő időben (async) érkező pontjait közös idővonalra
     **lineárisan interpoláljuk** (`interpAt`) és **összegezzük** (`totalKgSeries`),
     de csak ott, ahol MINDEN egység lefedi az időt → nincs ugrás.
   - hive1: `w1 + w2`. hive2: a 4 egység összege.
2. **hive2 valódi kalibrált kg (a frontenden):** a nyers count-ból boardonként
   `kg = (raw − zero_count − tempCorr) / count_per_kg` (config `boards[].calib`;
   node=log, master=lineáris hőkorrekció, a cella-hő a board saját `temp_c`-je).
   Ha egy board `calib`-ja hiányos, arra `placeholderKg`-fallback (median-alap) van.
   Nincs Cloud Function — a számítás kliens-oldali (l. `boardWeightKg`).
3. **„Környezet" chart = legördülőből EGY szenzor idősora** (config `env.sensors`):
   levegő (`air_t_c`), pára (`air_rh`), nyomás (`pressure_pa`) és kaptár-szonda
   (`hive_c`) — mind az `env/readings` ágból. Az **eső** külön: a felső sávban
   **24h csapadék-összeg csempe** (`env.rainTile`, a `rain_mm` intervallum-delták
   összege). A board-ok kalibrációs cella-hője (`temp_c`) a KÁRTYÁN látszik, nem chart.
4. **Fotó-szerű chart-navigáció, mobilon is:**
   - Kétujjas **pinch = idő-zoom**, egyujjas húzás = idő-pan (ECharts `inside`
     dataZoom az xAxis-on).
   - **A függőleges NEM szabad-zoomolható:** mindig a látható szakasz adatához
     igazodik (`filterMode:'filter'` + `yAxis.scale`), így a vonal **soha nem
     csúszik ki** a képből (nem lehet üres semmibe nagyítani).
   - **Nincs alsó csúszka**, **nincs érték-kiíró tooltip** (se kereszt, se tizedes).
   - **Kritikus tanulság:** a kétujjas pinch csak akkor jut el az ECharts-hoz, ha a
     **belső `<canvas>`** `touch-action: none` (nem elég a külső divre!) — különben a
     böngésző (iOS Safari) az oldalt zoomolja. Emellett iOS-en a `gesturestart/change/end`
     eseményeket JS-ből letiltjuk (`hardenTouch`).
5. **Látható pontok + interpolációs sűrítés:** `showSymbol:true` (nincs lttb-sampling),
   és a ritka mérések KÖZÉ `densify()` 5 percenként interpolált pontot szúr
   (gap-enként max 6), hogy nagyításnál a szakaszon is legyenek pontok.
6. **Nagyobb chartok:** súly 460px, környezet 340px; mobilon vh-alapú, a panel majdnem
   a képernyő széléig ér.

## Deploy

```bash
cd dashboard
firebase hosting:channel:deploy v2 --expires 30d   # preview channel (NEM live)
```

A live-ot ez a projekt szándékosan **nem** deployolja. (Ha valaha élesre menne:
`firebase deploy --only hosting` — de előtte tudatos döntés kell.)

## Új telepítés / node hozzáadása

Csak `config.js`: új elem a `hives` tömbbe (id, label, `schema`, `weightUnit`,
`boards[]` a RTDB-útvonalakkal/mezőnevekkel + boardonkénti `calib`, és `env`
= `{ rainTile, sensors[] }` a környezeti szenzorokhoz). Kód nem kell.

## Nyitott pontok / hátralévő

- **RTDB séma-tisztítás firmware-oldalon (KÉSZ, deploy hátra):** a `rods`→`boards`
  átnevezés, a master cella-hője (`scale_c`→`temp_c`) a board-rekordba, a kaptár-szonda
  (`hive_c`) az env-be. A master firmware-t **újra kell flashelni** (l. modul MEMORY 10.),
  a régi `hives/hive2/` teszt-adat törölhető (nem migrálunk), és az RTDB Rules-t
  (`firmware/master/firebase_hive2_rules.md`) deploy-olni kell az új sémára.
- A **súly-kalibráció ~50 kg-ig extrapolációja** validálatlan (mérés max ~10 kg) —
  a „4 cellára állás" adja a magas terhelési keresztellenőrzést.
- **Link-minőség** (rssi/gap) a hive2-höz: a master firmware ismeri, de jelenleg
  nem tölti fel; ha kell a nézetbe, előbb firmware-oldali feltöltés kell.
- A JS-runtime böngészős/eszközös validációja a felhasználó preview-visszajelzésén
  (headless böngésző nélkül nem tesztelhető közvetlenül).

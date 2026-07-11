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
2. **hive2 „becsült kg" (placeholder):** a nyers count-ból egységenként
   `kg = base + (raw − median(raw)) * kgPerCount` (config `placeholderKg`).
   **EZ NEM VALÓDI SÚLY** — a UI „⚠ becsült kg"-ként jelöli. A valódi kalibráció +
   hőm-kompenzáció **később, a felhőben** (Cloud Function) lesz, nem az MCU-n.
3. **Hőm-chart = csak valós környezeti hőmérséklet** (config `tempPlot`):
   Levegő (AHT20: hive2 `air_t_c` / hive1 `t`) + Kaptár-szonda (DS18B20: hive2
   `hive_c` / hive1 `t`). A node-ok `temp_c`-je és a `scale_c` mérleg-hőm
   **kalibrációs/hőm-kompenzációs adat → NEM rajzoljuk.**
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
6. **Nagyobb chartok:** súly 460px, hőm 340px; mobilon vh-alapú, a panel majdnem
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
`units[]` a RTDB-útvonalakkal és mezőnevekkel, `weather`, `tempPlot`). Kód nem kell.

## Nyitott pontok / hátralévő

- **Valódi kalibráció + kg-konverzió + hőm-kompenzáció a felhőben** (Cloud Function):
  a hive2 nyers count → valódi kg. Kiindulási együttható ~1031 count/°C (M1b).
  Amint kész, a `config.js` hive2 `weightUnit` "raw"→"kg" és a placeholder-blokk kivehető.
- A hive2 Firebase-szerkezet **nem végleges** — a felhős feldolgozáshoz szabadon
  átalakítható (pl. külön kalibrált ág).
- **Link-minőség** (rssi/gap) a hive2-höz: a master firmware ismeri, de jelenleg
  nem tölti fel; ha kell a nézetbe, előbb firmware-oldali feltöltés kell.
- A JS-runtime böngészős/eszközös validációja a felhasználó preview-visszajelzésén
  (headless böngésző nélkül nem tesztelhető közvetlenül).

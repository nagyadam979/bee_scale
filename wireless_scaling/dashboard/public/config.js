// bee_scale dashboard — konfiguráció (adatréteg-vezérlő)
// -----------------------------------------------------------------------------
// Ez a fájl írja le, MILYEN telepítések (hive-ok) és azon belül milyen
// mérőrudak (boards) léteznek, és melyik RTDB-ágból, milyen mezőkkel olvasandók.
// Új telepítés / node hozzáadása = ITT egy bejegyzés, kód-módosítás nélkül
// (multi-telepítésre felkészítve). A frontend read-only: soha nem ír az RTDB-be.
//
// A web apiKey NEM titok — a védelmet a Security Rules (auth != null olvasáshoz)
// + a Hosting-domain adja. Projekt: terrarium1 (Spark).
window.BEESCALE_CONFIG = {
  firebase: {
    apiKey: "AIzaSyC2wncwThhpj8GY4cIXPFjNPhaJv4OAAUM",
    authDomain: "terrarium1.firebaseapp.com",
    databaseURL: "https://terrarium1-default-rtdb.europe-west1.firebasedatabase.app",
    projectId: "terrarium1",
    storageBucket: "terrarium1.appspot.com",
    messagingSenderId: "379636506503",
  },

  // Egységenként ennyi legutóbbi rekordot töltünk (ts-indexelt query, limitToLast).
  // Nagy érték → az "egész timeline" böngészhető a chart alsó slider-ével.
  loadLimit: 5000,
  // Auto-frissítés (ms). hive2: 20 perc/egység, hive1: 5 perc — 2 perc bőven elég.
  refreshMs: 120000,
  // "Friss-e" küszöb: ennél régebbi utolsó minta → figyelmeztető jelzés (perc).
  staleMinutes: 45,

  hives: [
    {
      id: "hive2",
      label: "Wireless",
      sublabel: "master + 3 node · 20 perc",
      schema: "wireless",
      weightUnit: "raw", // az RTDB nyers HX711 count-ot tárol; a kg-ot a frontend
      // számolja rúdanként a boards[].calib-ból (kg = (raw - zero_count - tempCorr)
      // / count_per_kg). A weightField továbbra is weight_raw → a "raw" ág aktiválja
      // az app.js unitWeightKg() kalibrációs számítását (NEM azt jelenti, hogy raw-ot
      // jelenítünk meg). A placeholderKg (base/median-alap) csak fallback marad, ha
      // egy rúd calib-ja hiányos lenne.
      // Súly-kalibráció mind a 4 rúdra kész (calibration.json, R²>0.9998) → valódi kg.
      weightEstimated: false,
      // Mérőrudak (boards) a kaptáron. Mindegyik saját HX711 → egyetlen súly.
      boards: [
        {
          id: "master", label: "Master", path: "hives/hive2/boards/master/readings",
          weightField: "weight_raw", placeholderKg: { base: 34, kgPerCount: 0.0008 },
          // A master mérleg-cella hője (temp_c) mostantól a SAJÁT board-rekordjában
          // van (mint a node-oké) — nincs külön temp ág, nincs ts-társítás.
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          power: "mains", // hálózati táp — nincs akku
          // Kalibráció: kg = (raw - zero_count - tempCorr) / count_per_kg.
          // master = lineáris hő-komp.: tempCorr = tempCoef * (cellTemp - refTemp),
          // cellTemp = temp_c (p.it, a board-rekordból — a régi scale_c).
          // a/c/tempCoef ismert (hő-kalibráció kész); zero_count/count_per_kg/refTemp
          // a súly-kalibráció végén töltendő (addig placeholderKg a fallback).
          calib: { tempModel: "linear", tempCoef: 318, zero_count: -660656.85, count_per_kg: -46267.227, refTemp: 26.121 }, // súly-kalib kész (calibration.json, R²=0.99981)
        },
        {
          id: "node_1", label: "Node 1", path: "hives/hive2/boards/node_1/readings",
          weightField: "weight_raw", placeholderKg: { base: 28, kgPerCount: 0.0008 },
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv", noVbatHw: true, // node1: nincs akkumérő HW (mask VBAT-bit ≠ hiba)
          // Kalibráció: kg = (raw - zero_count - tempCorr) / count_per_kg.
          // node = logaritmikus hő-komp.: tempCorr = a * ln((cellTemp+c)/(refTemp+c)),
          // cellTemp = temp_c (p.it, a rod-rekordból). a/c ismert (hő-kalibráció kész);
          // zero_count/count_per_kg/refTemp a súly-kalibráció végén töltendő.
          calib: { tempModel: "log", a: -8370, c: 5.04, zero_count: 20301.43, count_per_kg: -39503.5691, refTemp: 27.837 }, // súly-kalib kész (calibration.json, R²=0.99992)
        },
        {
          id: "node_2", label: "Node 2", path: "hives/hive2/boards/node_2/readings",
          weightField: "weight_raw", placeholderKg: { base: 45, kgPerCount: 0.0008 },
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv",
          // Kalibráció (lásd node_1 komment): tempCorr = a * ln((cellTemp+c)/(refTemp+c)).
          calib: { tempModel: "log", a: -5281, c: 2.85, zero_count: -186914.47, count_per_kg: -39127.5917, refTemp: 28.927 }, // súly-kalib kész (calibration.json, R²=0.99992)
        },
        {
          id: "node_3", label: "Node 3", path: "hives/hive2/boards/node_3/readings",
          weightField: "weight_raw", placeholderKg: { base: 39, kgPerCount: 0.0008 },
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv",
          // Kalibráció (lásd node_1 komment): tempCorr = a * ln((cellTemp+c)/(refTemp+c)).
          calib: { tempModel: "log", a: -11034, c: 5.18, zero_count: 6298.8, count_per_kg: -40104.2897, refTemp: 27.594 }, // súly-kalib kész (calibration.json, R²=0.99994)
        },
      ],
      // Telepítés-szintű környezet (közös a master szenzoraiból).
      // - rainTile: csempeként a felső #weather sávban, az utolsó `windowH` óra
      //   rain_mm (INTERVALLUM-DELTA) értékeinek ÖSSZEGE (NINCS a plotolható listában).
      // - sensors: (a) pillanatnyi chip a #weather sávban (LEGUTOLSÓ érték), ÉS
      //   (b) a "Környezet" panel <select>-jében egyszerre EGY plotolható idősor.
      //   Minden hive2 környezeti adat EGY ágban van (env/readings) — a kaptár-szonda
      //   (hive_c) is oda került a firmware séma-tisztításkor; path→sorozat map olvassa.
      env: {
        rainTile: { path: "hives/hive2/env/readings", field: "rain_mm", windowH: 24, label: "Csapadék 24ó", unit: "mm", fmt: 1, icon: "🌧" },
        sensors: [
          { path: "hives/hive2/env/readings",  field: "air_t_c",     label: "Levegő",        unit: "°C",  fmt: 1, icon: "🌡" },
          { path: "hives/hive2/env/readings",  field: "air_rh",      label: "Páratartalom",  unit: "%",   fmt: 0, icon: "💧" },
          { path: "hives/hive2/env/readings",  field: "pressure_pa", label: "Légnyomás",     unit: "hPa", fmt: 0, scale: 0.01, icon: "⏲" },
          { path: "hives/hive2/env/readings",  field: "hive_c",      label: "Kaptár-szonda", unit: "°C",  fmt: 1, icon: "🐝" },
          // Eső: a legördülőben VISSZANÉZHETŐ idősor (időszakonkénti rain_mm delta).
          // chip:false → NEM jelenik meg pillanatnyi chipként a felső sávban, mert
          // ott a 24h csapadék-összeg csempe (rainTile) már lefedi az eső-státuszt.
          // step:true → lépcsős, NEM interpolált vonal (a densify rámpa félrevezető
          // lenne az eső 0→spike deltáinál; így minden időszak a saját mm-jét tartja).
          { path: "hives/hive2/env/readings",  field: "rain_mm",     label: "Eső (időszak)", unit: "mm",  fmt: 1, icon: "🌧", chip: false, step: true },
        ],
      },
    },

    {
      id: "hive1",
      label: "Prototípus",
      sublabel: "2 mérleg · kalibrált · 5 perc",
      schema: "legacy",
      weightUnit: "kg", // a régi rendszer már kalibrált kg-ot ír (w)
      // hive1: w1 és w2 EGY kaptár két mérőrúdja → a fő görbe az ÖSSZEGÜK
      // (legközelebbi-szomszéd párosítás tolSec toleranciával); a rúdak külön
      // vonalként a legendből bekapcsolhatók, de alapból rejtve.
      aggregate: { label: "Össz-súly", tolSec: 180 },
      // Spike-szűrés (az éles prototípus-app viselkedése): rúdankénti CENTRÁLT
      // gördülő medián a rajzolás/összegzés ELŐTT, a HW-eredetű 1-2 mintás
      // tüskék (HX711/mechanikai zaj) levágására. k=5 (páratlan; 1/0 = kikapcs).
      // CSAK hive1-re — a hive2 (wireless) más mintavételezés, ott NINCS szűrés.
      spikeFilter: { medianK: 5 },
      // hive1 board-oknak nincs innerTempFrom → a kártyán nem jelenik meg cella-hő.
      boards: [
        {
          id: "w1", label: "Mérleg 1", path: "hives/hive1/rods/w1/readings",
          weightField: "w",
        },
        {
          id: "w2", label: "Mérleg 2", path: "hives/hive1/rods/w2/readings",
          weightField: "w",
        },
      ],
      // hive1-nek nincs eső-adata → rainTile: null (a kód ezt tűrje).
      env: {
        rainTile: null,
        sensors: [
          { path: "hives/hive1/env/readings",  field: "t", label: "Levegő",        unit: "°C", fmt: 1, icon: "🌡" },
          { path: "hives/hive1/env/readings",  field: "h", label: "Páratartalom",  unit: "%",  fmt: 0, icon: "💧" },
          { path: "hives/hive1/temp/readings", field: "t", label: "Kaptár-szonda", unit: "°C", fmt: 1, icon: "🐝" },
        ],
      },
    },
  ],
};

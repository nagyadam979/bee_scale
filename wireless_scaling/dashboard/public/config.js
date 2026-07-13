// bee_scale dashboard — konfiguráció (adatréteg-vezérlő)
// -----------------------------------------------------------------------------
// Ez a fájl írja le, MILYEN telepítések (hive-ok) és azon belül milyen
// mérőegységek léteznek, és melyik RTDB-ágból, milyen mezőkkel olvasandók.
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
      weightUnit: "raw", // nyers HX711 count — kalibráció (kg) a felhőben, később
      // PLACEHOLDER kg-normalizálás, amíg nincs valódi kalibráció: minden mérleg
      // saját nyers offsettel/előjellel írja a weight_raw-t, ezért egységenként a
      // betöltött adat MEDIÁNJÁHOZ képest skálázunk egy feltételezett kg-tartományba:
      //   kg = base + (raw - median(raw)) * kgPerCount
      // A base cellánként eltér (hogy a görbék elkülönüljenek), a kgPerCount közös
      // becslés. EZ NEM VALÓDI SÚLY — a UI "becsült kg"-ként jelöli.
      weightEstimated: true,
      // Mérőegységek (kaptárak). Mindegyik saját HX711 → egyetlen súly.
      units: [
        {
          id: "master", label: "Master", path: "hives/hive2/rods/master/readings",
          weightField: "weight_raw", placeholderKg: { base: 34, kgPerCount: 0.0008 },
          // A master belső hőmérője (mérleg-hőm) a KÖZÖS temp ágban van (scale_c),
          // nem a rod-rekordban — ts-alapon társítjuk.
          innerTempFrom: { path: "weather.temp", field: "scale_c", label: "mérleg-hőm" },
          power: "mains", // hálózati táp — nincs akku
          // Kalibráció: kg = (raw - zero_count - tempCorr) / count_per_kg.
          // master = lineáris hő-komp.: tempCorr = tempCoef * (cellTemp - refTemp),
          // cellTemp = scale_c (a fenti innerTempFrom.path-os series, ts-interpolálva).
          // a/c/tempCoef ismert (hő-kalibráció kész); zero_count/count_per_kg/refTemp
          // a súly-kalibráció végén töltendő (addig placeholderKg a fallback).
          calib: { tempModel: "linear", tempCoef: 318, zero_count: null, count_per_kg: null, refTemp: null }, // TODO: súly-kalib
        },
        {
          id: "node_1", label: "Node 1", path: "hives/hive2/rods/node_1/readings",
          weightField: "weight_raw", placeholderKg: { base: 28, kgPerCount: 0.0008 },
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv", noVbatHw: true, // node1: nincs akkumérő HW (mask VBAT-bit ≠ hiba)
          // Kalibráció: kg = (raw - zero_count - tempCorr) / count_per_kg.
          // node = logaritmikus hő-komp.: tempCorr = a * ln((cellTemp+c)/(refTemp+c)),
          // cellTemp = temp_c (p.it, a rod-rekordból). a/c ismert (hő-kalibráció kész);
          // zero_count/count_per_kg/refTemp a súly-kalibráció végén töltendő.
          calib: { tempModel: "log", a: -8370, c: 5.04, zero_count: null, count_per_kg: null, refTemp: null }, // TODO: súly-kalib
        },
        {
          id: "node_2", label: "Node 2", path: "hives/hive2/rods/node_2/readings",
          weightField: "weight_raw", placeholderKg: { base: 45, kgPerCount: 0.0008 },
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv",
          // Kalibráció (lásd node_1 komment): tempCorr = a * ln((cellTemp+c)/(refTemp+c)).
          calib: { tempModel: "log", a: -5281, c: 2.85, zero_count: null, count_per_kg: null, refTemp: null }, // TODO: súly-kalib
        },
        {
          id: "node_3", label: "Node 3", path: "hives/hive2/rods/node_3/readings",
          weightField: "weight_raw", placeholderKg: { base: 39, kgPerCount: 0.0008 },
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv",
          // Kalibráció (lásd node_1 komment): tempCorr = a * ln((cellTemp+c)/(refTemp+c)).
          calib: { tempModel: "log", a: -11034, c: 5.18, zero_count: null, count_per_kg: null, refTemp: null }, // TODO: súly-kalib
        },
      ],
      // Telepítés-szintű időjárás (közös a master szenzoraiból).
      weather: {
        env: {
          path: "hives/hive2/env/readings",
          metrics: [
            { field: "air_t_c", label: "Levegő", unit: "°C", fmt: 1, icon: "🌡" },
            { field: "air_rh", label: "Pára", unit: "%", fmt: 0, icon: "💧" },
            { field: "pressure_pa", label: "Nyomás", unit: "hPa", fmt: 0, scale: 0.01, icon: "⏲" },
            { field: "rain_mm", label: "Eső", unit: "mm", fmt: 1, icon: "🌧" },
          ],
        },
        temp: {
          path: "hives/hive2/temp/readings",
          metrics: [
            { field: "hive_c", label: "Kaptár", unit: "°C", fmt: 1, icon: "🐝" },
            { field: "scale_c", label: "Mérleg", unit: "°C", fmt: 1, icon: "⚖" },
          ],
        },
      },
      // Hőm-chart: CSAK a valós környezeti hőmérsékletek rajzolódnak.
      // A node-ok temp_c-je és a scale_c a HŐM-KOMPENZÁCIÓS/kalibrációs adat →
      // NEM plotoljuk. Levegő = AHT20 (air_t_c), Kaptár = DS18B20 szonda (hive_c).
      tempPlot: [
        { src: "env",  field: "air_t_c", label: "Levegő (AHT20)", color: "#4fc3f7" },
        { src: "temp", field: "hive_c",  label: "Kaptár (szonda)", color: "#f5b301" },
      ],
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
      units: [
        {
          id: "w1", label: "Mérleg 1", path: "hives/hive1/rods/w1/readings",
          weightField: "w",
        },
        {
          id: "w2", label: "Mérleg 2", path: "hives/hive1/rods/w2/readings",
          weightField: "w",
        },
      ],
      weather: {
        env: {
          path: "hives/hive1/env/readings",
          metrics: [
            { field: "t", label: "Levegő", unit: "°C", fmt: 1, icon: "🌡" },
            { field: "h", label: "Pára", unit: "%", fmt: 0, icon: "💧" },
          ],
        },
        temp: {
          path: "hives/hive1/temp/readings",
          metrics: [
            { field: "t", label: "Kaptár", unit: "°C", fmt: 1, icon: "🐝" },
          ],
        },
      },
      tempPlot: [
        { src: "env",  field: "t", label: "Levegő",          color: "#4fc3f7" },
        { src: "temp", field: "t", label: "Kaptár (szonda)", color: "#f5b301" },
      ],
    },
  ],
};

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
  loadLimit: 1000,
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
      // Mérőegységek (kaptárak). Mindegyik saját HX711 → egyetlen súly.
      units: [
        {
          id: "master", label: "Master", path: "hives/hive2/rods/master/readings",
          weightField: "weight_raw",
          // A master belső hőmérője (mérleg-hőm) a KÖZÖS temp ágban van (scale_c),
          // nem a rod-rekordban — ts-alapon társítjuk.
          innerTempFrom: { path: "weather.temp", field: "scale_c", label: "mérleg-hőm" },
          power: "mains", // hálózati táp — nincs akku
        },
        {
          id: "node_1", label: "Node 1", path: "hives/hive2/rods/node_1/readings",
          weightField: "weight_raw",
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv", noVbatHw: true, // node1: nincs akkumérő HW (mask VBAT-bit ≠ hiba)
        },
        {
          id: "node_2", label: "Node 2", path: "hives/hive2/rods/node_2/readings",
          weightField: "weight_raw",
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv",
        },
        {
          id: "node_3", label: "Node 3", path: "hives/hive2/rods/node_3/readings",
          weightField: "weight_raw",
          innerTempFrom: { field: "temp_c", label: "cella-hőm" },
          vbatField: "vbat_mv",
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
    },

    {
      id: "hive1",
      label: "Prototípus",
      sublabel: "2 mérleg · kalibrált · 5 perc",
      schema: "legacy",
      weightUnit: "kg", // a régi rendszer már kalibrált kg-ot ír (w)
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
    },
  ],
};

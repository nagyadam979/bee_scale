// bee_scale — web dashboard (Firebase Hosting + RTDB read, read-only)
// Modular Firebase JS SDK + Chart.js, build-lépés nélkül (ES module a böngészőből).
//
// FONTOS: ez a frontend KIZÁRÓLAG OLVAS. Soha nem ír az RTDB-be.
// A védelmet a Security Rules (auth != null olvasáshoz) + a Hosting-domain adja.
//
// ADATMODELL (2026-06-08): a két board ÖNÁLLÓAN ír a saját rúd-ágába:
//   /hives/hive1/rods/w1/readings/{pushId}: { ts, w }       (board_21)
//   /hives/hive1/rods/w2/readings/{pushId}: { ts, w }       (board_12)
// A dashboard mindkét streamet olvassa, külön rajzolja (w1, w2), és a w_total-t
// a két rúd legközelebbi-szomszéd párosításával (időtolerancián belül) összegzi.
//
// KÖRNYEZETI ÁG (board_12-re kötött AHT20[+BMP280], kaptár-szintű):
//   /hives/hive1/env/readings/{pushId}: { ts, t, h[, p] }   (°C, %RH, opc. hPa)
// Külön (alsó) chartra rajzoljuk: t bal, h jobb Y-tengelyen. A p (nyomás) most
// nincs a buszon → opcionális; ha később bekerül, a rekord tartalmazza.
//
// KAPTÁR-HŐM ÁG (board_21-re kötött DS18B20 OneWire szonda, GPIO4):
//   /hives/hive1/temp/readings/{pushId}: { ts, t }          (°C)
// Az env-charton osztozik, harmadik vonalként a bal (°C) tengelyen — így a doboz-
// hőm (env, AHT20) és a kaptár belső hőm (DS18B20) közvetlenül összevethető.

import { initializeApp } from "https://www.gstatic.com/firebasejs/12.4.0/firebase-app.js";
import {
  getAuth,
  signInAnonymously,
  onAuthStateChanged,
} from "https://www.gstatic.com/firebasejs/12.4.0/firebase-auth.js";
import {
  getDatabase,
  ref,
  query,
  orderByChild,
  startAt,
  limitToLast,
  get,
  onChildAdded,
  off,
} from "https://www.gstatic.com/firebasejs/12.4.0/firebase-database.js";

// ---------------------------------------------------------------------------
// firebaseConfig — a web apiKey NEM titok (a védelmet a Rules + domain adja).
// Projekt: terrarium1 (Spark / no-cost). appId nincs megadva: csak az
// Analyticshez kéne, az Auth + RTDB működéséhez nem szükséges.
// ---------------------------------------------------------------------------
const firebaseConfig = {
  apiKey: "AIzaSyC2wncwThhpj8GY4cIXPFjNPhaJv4OAAUM",
  authDomain: "terrarium1.firebaseapp.com",
  databaseURL: "https://terrarium1-default-rtdb.europe-west1.firebasedatabase.app",
  projectId: "terrarium1",
  storageBucket: "terrarium1.appspot.com",
  messagingSenderId: "379636506503",
};

const HIVE_ID = "hive1";
// Rúdankénti útvonalak — minden board a saját ágába ír { ts, w }-t.
const ROD_PATHS = {
  w1: `hives/${HIVE_ID}/rods/w1/readings`,
  w2: `hives/${HIVE_ID}/rods/w2/readings`,
};
// Környezeti ág — { ts, t, h[, p] }.
const ENV_PATH = `hives/${HIVE_ID}/env/readings`;
// Kaptár-hőm ág (board_21 DS18B20 szonda) — { ts, t }. Az env doboz-hőmérséklete
// mellé, közös °C tengelyre rajzolva (doboz vs kaptár belső hőm összevethető).
const TEMP_PATH = `hives/${HIVE_ID}/temp/readings`;

// Időablakok (másodperc). null = összes (limittel megfogva).
const WINDOWS = { "24h": 86400, "7d": 604800, "30d": 2592000, all: null };
const ALL_LIMIT = 10000; // "összes" biztonsági plafon (ne húzza le korlátlanul)
const SAMPLE_INTERVAL_SEC = 300; // 5 perc
const STALE_AFTER_SEC = SAMPLE_INTERVAL_SEC * 3; // 15 perc → "nincs friss adat"
// w_total párosítási tolerancia: ennél közelebbi w1/w2 mintát tekintünk egy
// időpontnak. A két board nem szinkronban mintáz (külön millis-fázis), ezért a
// legközelebbi-szomszéd párosítás robusztusabb a fix epoch-binelésnél.
const PAIR_TOLERANCE_SEC = 180;
// Zajszűrés: rúdankénti CENTRÁLT gördülő medián a rajzolás előtt. A HW-eredetű
// 1-2 mintás kiugró tüskéket (±pár kg, HX711/mechanikai zaj) levágja, a valós
// tömegtrendet és a finom dinamikát megőrzi. A DB-be NYERS adat megy; ez tisztán
// megjelenítési szűrés (a nyers minta bármikor újraszűrhető más eljárással).
// k=5 a forrásnál (w1, w2 külön), a párosítás ELŐTT — így a w1/w2 nézet is tiszta.
const MEDIAN_K = 5; // páratlan; 0/1 = kikapcsolva

// ---------------------------------------------------------------------------
const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getDatabase(app);

// ---- DOM ----
const $ = (id) => document.getElementById(id);
const splashView = $("splash-view");
const dashboardView = $("dashboard-view");
const splashError = $("splash-error");

let currentRange = "7d";
// Rúdankénti live listenerek (off()-hoz) + baseline ts.
const liveListeners = { w1: null, w2: null };
let liveBaselineTs = null;
// Rúdankénti pontok: pushKey -> { ts, w }
const points = { w1: new Map(), w2: new Map() };
let chart = null;
// Env pontok: pushKey -> { ts, t, h[, p] } + külön chart és live listener.
const envPoints = new Map();
let envChart = null;
let envLiveQuery = null;
// Kaptár-hőm pontok (DS18B20): pushKey -> { ts, t }. Az env-charton osztozik.
const tempPoints = new Map();
let tempLiveQuery = null;

// ===========================================================================
// Auth — anonim (nincs bejelentkező képernyő). A Rules `auth != null`-t vár az
// olvasáshoz; az anonim token ezt kielégíti. Írni továbbra is csak a device-user tud.
// ===========================================================================
onAuthStateChanged(auth, (user) => {
  if (user) {
    showDashboard();
  } else {
    showSplash();
    signInAnonymously(auth).catch((err) => showSplashError(err));
  }
});

function showSplashError(err) {
  const code = err && err.code ? err.code : "";
  let msg;
  if (code.includes("operation-not-allowed"))
    msg = "Az anonim belépés nincs engedélyezve a Firebase Console-ban (Authentication → Sign-in method → Anonymous).";
  else if (code.includes("network")) msg = "Hálózati hiba a csatlakozáskor.";
  else msg = "Csatlakozási hiba: " + (code || err.message || "ismeretlen");
  splashError.textContent = msg;
  splashError.classList.remove("hidden");
  $("splash-status").classList.add("hidden");
}

function showSplash() {
  detachLive();
  points.w1.clear();
  points.w2.clear();
  envPoints.clear();
  tempPoints.clear();
  if (chart) {
    chart.destroy();
    chart = null;
  }
  if (envChart) {
    envChart.destroy();
    envChart = null;
  }
  dashboardView.classList.add("hidden");
  splashView.classList.remove("hidden");
}

function showDashboard() {
  splashView.classList.add("hidden");
  dashboardView.classList.remove("hidden");
  ensureChart();
  ensureEnvChart();
  loadRange(currentRange);
  attachLive(); // élő frissítés a betöltés pillanatától
}

// ===========================================================================
// Chart
// ===========================================================================
function ensureChart() {
  if (chart) return;
  const ctx = $("chart").getContext("2d");
  chart = new Chart(ctx, {
    type: "line",
    data: {
      datasets: [
        ds("Össz-súly (w1+w2)", getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#f5b301", false),
        ds("Rúd 1 (w1)", getComputedStyle(document.documentElement).getPropertyValue("--accent-2").trim() || "#4fc3f7", true),
        ds("Rúd 2 (w2)", getComputedStyle(document.documentElement).getPropertyValue("--accent-3").trim() || "#81c784", true),
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      parsing: false, // {x,y} pontokat adunk át közvetlenül
      interaction: { mode: "index", intersect: false },
      scales: {
        x: {
          type: "time",
          // Date-eket adunk át (ms) → Chart.js a böngésző helyi időzónájában rajzol.
          time: { tooltipFormat: "yyyy-MM-dd HH:mm" },
          ticks: { color: "#9aa0aa", maxRotation: 0, autoSkip: true },
          grid: { color: "#2c313c" },
        },
        y: {
          title: { display: true, text: "kg", color: "#9aa0aa" },
          ticks: { color: "#9aa0aa" },
          grid: { color: "#2c313c" },
        },
      },
      plugins: {
        legend: { labels: { color: "#e8eaed" } },
        tooltip: { callbacks: { label: (c) => `${c.dataset.label}: ${c.parsed.y.toFixed(2)} kg` } },
      },
    },
  });

  // w1 / w2 togglek
  $("toggle-w1").addEventListener("change", (e) => setSeriesVisible(1, e.target.checked));
  $("toggle-w2").addEventListener("change", (e) => setSeriesVisible(2, e.target.checked));
}

function ds(label, color, hidden) {
  return {
    label,
    data: [],
    borderColor: color,
    backgroundColor: color,
    borderWidth: 2,
    pointRadius: 0,
    pointHoverRadius: 4,
    tension: 0.15,
    spanGaps: true,
    hidden,
  };
}

function setSeriesVisible(idx, visible) {
  if (!chart) return;
  chart.data.datasets[idx].hidden = !visible;
  chart.update();
}

// ===========================================================================
// Env-chart — t (bal Y, °C) + h (jobb Y, %RH). Külön panel a súly-chart alatt.
// ===========================================================================
const ENV_T_COLOR = "#ff8a65"; // narancs — env (doboz) hőmérséklet
const ENV_H_COLOR = "#4fc3f7"; // kék — páratartalom
const TEMP_COLOR  = "#e57373"; // piros — kaptár belső hőm (DS18B20 szonda)

function ensureEnvChart() {
  if (envChart) return;
  const ctx = $("env-chart").getContext("2d");
  envChart = new Chart(ctx, {
    type: "line",
    data: {
      datasets: [
        envDs("Doboz-hőm (°C)", ENV_T_COLOR, "y"),
        envDs("Páratartalom (%RH)", ENV_H_COLOR, "y1"),
        envDs("Kaptár-hőm (szonda, °C)", TEMP_COLOR, "y"),
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      parsing: false,
      interaction: { mode: "index", intersect: false },
      scales: {
        x: {
          type: "time",
          time: { tooltipFormat: "yyyy-MM-dd HH:mm" },
          ticks: { color: "#9aa0aa", maxRotation: 0, autoSkip: true },
          grid: { color: "#2c313c" },
        },
        y: {
          position: "left",
          title: { display: true, text: "°C", color: ENV_T_COLOR },
          ticks: { color: "#9aa0aa" },
          grid: { color: "#2c313c" },
        },
        y1: {
          position: "right",
          title: { display: true, text: "%RH", color: ENV_H_COLOR },
          ticks: { color: "#9aa0aa" },
          grid: { drawOnChartArea: false }, // ne dupla rácsozzon
        },
      },
      plugins: {
        legend: { labels: { color: "#e8eaed" } },
        tooltip: {
          callbacks: {
            label: (c) => {
              const unit = c.dataset.yAxisID === "y1" ? " %RH" : " °C";
              return `${c.dataset.label}: ${c.parsed.y.toFixed(1)}${unit}`;
            },
          },
        },
      },
    },
  });
}

function envDs(label, color, yAxisID) {
  return {
    label,
    data: [],
    borderColor: color,
    backgroundColor: color,
    borderWidth: 2,
    pointRadius: 0,
    pointHoverRadius: 4,
    tension: 0.15,
    spanGaps: true,
    yAxisID,
  };
}

// ===========================================================================
// Időablak gombok + Reload
// ===========================================================================
document.querySelectorAll(".range-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".range-btn").forEach((b) => b.classList.remove("active"));
    btn.classList.add("active");
    currentRange = btn.dataset.range;
    loadRange(currentRange);
  });
});

$("reload-btn").addEventListener("click", () => loadRange(currentRange));

// ===========================================================================
// Adatlekérés — ts szerinti, indexelt query (NEM a teljes ág), rúdankénti útvonalon
// ===========================================================================
function buildQuery(path, range) {
  const base = ref(db, path);
  const windowSec = WINDOWS[range];
  if (windowSec == null) {
    // "összes" — limittel megfogva, ts szerint rendezve
    return query(base, orderByChild("ts"), limitToLast(ALL_LIMIT));
  }
  const startTs = Math.floor(Date.now() / 1000) - windowSec;
  return query(base, orderByChild("ts"), startAt(startTs));
}

async function loadRange(range) {
  setStatus("Betöltés…");
  points.w1.clear();
  points.w2.clear();
  envPoints.clear();
  tempPoints.clear();
  try {
    const [snapW1, snapW2, snapEnv, snapTemp] = await Promise.all([
      get(buildQuery(ROD_PATHS.w1, range)),
      get(buildQuery(ROD_PATHS.w2, range)),
      get(buildQuery(ENV_PATH, range)),
      get(buildQuery(TEMP_PATH, range)),
    ]);
    let n1 = 0, n2 = 0;
    snapW1.forEach((c) => { if (addPoint("w1", c.key, c.val())) n1++; });
    snapW2.forEach((c) => { if (addPoint("w2", c.key, c.val())) n2++; });
    snapEnv.forEach((c) => { addEnvPoint(c.key, c.val()); });
    snapTemp.forEach((c) => { addTempPoint(c.key, c.val()); });
    redraw();
    envRedraw();
    const n = n1 + n2;
    if (range === "all" && (n1 >= ALL_LIMIT || n2 >= ALL_LIMIT)) {
      setStatus(`⚠️ w1:${n1} / w2:${n2} rekord (plafon: ${ALL_LIMIT}) — a legrégebbiek hiányozhatnak`);
    } else {
      setStatus(`${n} rekord betöltve (w1:${n1} / w2:${n2})`);
    }
  } catch (err) {
    setStatus("Hiba: " + (err.code || err.message));
    console.error(err);
  }
}

// Egy rúd-rekord validálása + felvétele a megfelelő Map-be. true, ha új és érvényes.
function addPoint(rod, key, val) {
  if (!val || typeof val.ts !== "number" || typeof val.w !== "number") return false;
  const map = points[rod];
  if (map.has(key)) return false;
  map.set(key, { ts: val.ts, w: val.w });
  return true;
}

// Env-rekord validálása + felvétele. t,h kötelező; p opcionális. true, ha új és érvényes.
function addEnvPoint(key, val) {
  if (!val || typeof val.ts !== "number" ||
      typeof val.t !== "number" || typeof val.h !== "number") return false;
  if (envPoints.has(key)) return false;
  const rec = { ts: val.ts, t: val.t, h: val.h };
  if (typeof val.p === "number") rec.p = val.p;
  envPoints.set(key, rec);
  return true;
}

// Kaptár-hőm rekord (DS18B20) validálása + felvétele. ts,t kötelező. true, ha új és érvényes.
function addTempPoint(key, val) {
  if (!val || typeof val.ts !== "number" || typeof val.t !== "number") return false;
  if (tempPoints.has(key)) return false;
  tempPoints.set(key, { ts: val.ts, t: val.t });
  return true;
}

// ===========================================================================
// Élő frissítés — csak a betöltés óta ÚJONNAN érkezett pushok, MINDKÉT rúdon
// (startAt = baseline → meglévő rekordokra nem tüzel, csak az új mintákra)
// ===========================================================================
function attachLive() {
  detachLive();
  liveBaselineTs = Math.floor(Date.now() / 1000);
  for (const rod of ["w1", "w2"]) {
    const q = query(ref(db, ROD_PATHS[rod]), orderByChild("ts"), startAt(liveBaselineTs));
    liveListeners[rod] = q;
    onChildAdded(q, (child) => {
      if (addPoint(rod, child.key, child.val())) {
        redraw();
        setStatus(`Új minta érkezett (${rod})`);
      }
    });
  }
  // Env live stream (ugyanazon baseline-tól, csak az új mintákra tüzel).
  envLiveQuery = query(ref(db, ENV_PATH), orderByChild("ts"), startAt(liveBaselineTs));
  onChildAdded(envLiveQuery, (child) => {
    if (addEnvPoint(child.key, child.val())) {
      envRedraw();
      setStatus("Új env minta érkezett");
    }
  });
  // Kaptár-hőm live stream (DS18B20) — szintén az env-chartra redraw-ol.
  tempLiveQuery = query(ref(db, TEMP_PATH), orderByChild("ts"), startAt(liveBaselineTs));
  onChildAdded(tempLiveQuery, (child) => {
    if (addTempPoint(child.key, child.val())) {
      envRedraw();
      setStatus("Új kaptár-hőm minta érkezett");
    }
  });
}

function detachLive() {
  for (const rod of ["w1", "w2"]) {
    if (liveListeners[rod]) {
      off(liveListeners[rod]);
      liveListeners[rod] = null;
    }
  }
  if (envLiveQuery) {
    off(envLiveQuery);
    envLiveQuery = null;
  }
  if (tempLiveQuery) {
    off(tempLiveQuery);
    tempLiveQuery = null;
  }
}

// ===========================================================================
// w_total párosítás — legközelebbi-szomszéd időtolerancián belül.
// Minden w1 mintához megkeresi a legközelebbi w2 mintát; ha az időkülönbség
// <= PAIR_TOLERANCE_SEC, összegzi. Kétmutatós merge a két rendezett tömbön.
// ===========================================================================
// Centrált gördülő medián egy ts-rendezett [{ts, w}] tömbön. A ts-eket változat-
// lanul hagyja, csak a w-t cseréli a ±(k/2) ablak mediánjára. A széleknél az ablak
// rövidül (clamp), nincs torzító edge-pad. k<=1 → változatlan (kikapcsolva).
function medianSmooth(rows, k) {
  const n = rows.length;
  if (k <= 1 || n === 0) return rows;
  const half = k >> 1;
  const out = new Array(n);
  for (let i = 0; i < n; i++) {
    const lo = Math.max(0, i - half);
    const hi = Math.min(n, i + half + 1);
    const win = [];
    for (let j = lo; j < hi; j++) win.push(rows[j].w);
    win.sort((a, b) => a - b);
    const m = win.length;
    const med = m & 1 ? win[m >> 1] : (win[m / 2 - 1] + win[m / 2]) / 2;
    out[i] = { ts: rows[i].ts, w: med };
  }
  return out;
}

function pairTotal(rows1, rows2) {
  const out = [];
  if (!rows1.length || !rows2.length) return out;
  let j = 0;
  for (const a of rows1) {
    // j-t a-hoz legközelebbi w2 felé léptetjük
    while (j + 1 < rows2.length && Math.abs(rows2[j + 1].ts - a.ts) <= Math.abs(rows2[j].ts - a.ts)) {
      j++;
    }
    const b = rows2[j];
    if (Math.abs(b.ts - a.ts) <= PAIR_TOLERANCE_SEC) {
      out.push({ ts: a.ts, w: a.w + b.w });
    }
  }
  return out;
}

// ===========================================================================
// Rajzolás + fejléc-állapot
// ===========================================================================
function windowMinTs() {
  const windowSec = WINDOWS[currentRange];
  return windowSec == null ? -Infinity : Math.floor(Date.now() / 1000) - windowSec;
}

function sortedRows(rod, minTs) {
  return [...points[rod].values()].filter((p) => p.ts >= minTs).sort((a, b) => a.ts - b.ts);
}

function redraw() {
  if (!chart) return;
  const minTs = windowMinTs();
  // Rúdankénti centrált medián a párosítás ELŐTT (forrásnál fogja a tüskét).
  const rows1 = medianSmooth(sortedRows("w1", minTs), MEDIAN_K);
  const rows2 = medianSmooth(sortedRows("w2", minTs), MEDIAN_K);
  const total = pairTotal(rows1, rows2);

  chart.data.datasets[0].data = total.map((p) => ({ x: p.ts * 1000, y: p.w }));
  chart.data.datasets[1].data = rows1.map((p) => ({ x: p.ts * 1000, y: p.w }));
  chart.data.datasets[2].data = rows2.map((p) => ({ x: p.ts * 1000, y: p.w }));
  chart.update();

  const hasAny = rows1.length > 0 || rows2.length > 0;
  $("empty-state").classList.toggle("hidden", hasAny);
  updateLatest(rows1, rows2);
}

// Fejléc: az össz-súly a két rúd LEGUTOLSÓ ismert értékének összege; az időbélyeg
// és a "friss-e" a legutóbbi minta (bármelyik rúd) alapján.
function updateLatest(rows1, rows2) {
  const valEl = $("latest-value");
  const timeEl = $("latest-time");
  const banner = $("stale-banner");

  const last1 = rows1.length ? rows1[rows1.length - 1] : null;
  const last2 = rows2.length ? rows2[rows2.length - 1] : null;

  if (!last1 && !last2) {
    valEl.textContent = "– kg";
    timeEl.textContent = "nincs adat";
    banner.classList.add("hidden");
    return;
  }

  // Össz-súly: csak ha mindkét rúdról van adat; egyébként a meglévő rúd értéke
  // (a felhasználó a w1/w2 togglékkel látja, melyik hiányzik).
  let total, label;
  if (last1 && last2) {
    total = last1.w + last2.w;
    label = "kg (w1+w2)";
  } else {
    total = (last1 || last2).w;
    label = last1 ? "kg (csak w1)" : "kg (csak w2)";
  }
  valEl.textContent = `${total.toFixed(2)} ${label}`;

  const newestTs = Math.max(last1 ? last1.ts : 0, last2 ? last2.ts : 0);
  const d = new Date(newestTs * 1000);
  timeEl.textContent = d.toLocaleString("hu-HU"); // helyi időzóna
  const ageSec = Math.floor(Date.now() / 1000) - newestTs;
  banner.classList.toggle("hidden", ageSec <= STALE_AFTER_SEC);
}

// ===========================================================================
// Env-chart rajzolás + fejléc-chip
// ===========================================================================
function envRedraw() {
  if (!envChart) return;
  const minTs = windowMinTs();
  const rows = [...envPoints.values()]
    .filter((p) => p.ts >= minTs)
    .sort((a, b) => a.ts - b.ts);
  const tempRows = [...tempPoints.values()]
    .filter((p) => p.ts >= minTs)
    .sort((a, b) => a.ts - b.ts);

  envChart.data.datasets[0].data = rows.map((p) => ({ x: p.ts * 1000, y: p.t }));
  envChart.data.datasets[1].data = rows.map((p) => ({ x: p.ts * 1000, y: p.h }));
  envChart.data.datasets[2].data = tempRows.map((p) => ({ x: p.ts * 1000, y: p.t }));
  envChart.update();

  const hasAny = rows.length > 0 || tempRows.length > 0;
  $("env-empty-state").classList.toggle("hidden", hasAny);
  updateEnvChip(rows, tempRows);
}

// Fejléc-chip: az aktuális időablak LEGUTOLSÓ env (doboz) + kaptár-hőm mintája.
function updateEnvChip(rows, tempRows) {
  const valEl = $("env-value");
  const timeEl = $("env-time");
  const lastEnv = rows.length ? rows[rows.length - 1] : null;
  const lastTemp = tempRows.length ? tempRows[tempRows.length - 1] : null;

  if (!lastEnv && !lastTemp) {
    valEl.textContent = "– °C · – %";
    timeEl.textContent = "nincs env adat";
    return;
  }

  const parts = [];
  if (lastEnv) {
    const p = typeof lastEnv.p === "number" ? ` · ${lastEnv.p.toFixed(0)} hPa` : "";
    parts.push(`${lastEnv.t.toFixed(1)} °C · ${lastEnv.h.toFixed(0)} %${p}`);
  }
  if (lastTemp) parts.push(`kaptár ${lastTemp.t.toFixed(1)} °C`);
  valEl.textContent = parts.join(" · ");

  // Időbélyeg: a két stream közül a frissebb minta ideje.
  const newestTs = Math.max(lastEnv ? lastEnv.ts : 0, lastTemp ? lastTemp.ts : 0);
  timeEl.textContent = new Date(newestTs * 1000).toLocaleString("hu-HU");
}

function setStatus(msg) {
  $("status").textContent = msg;
}

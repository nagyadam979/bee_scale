// bee_scale — web dashboard (Firebase Hosting + RTDB read, read-only)
// Modular Firebase JS SDK + Chart.js, build-lépés nélkül (ES module a böngészőből).
//
// FONTOS: ez a frontend KIZÁRÓLAG OLVAS. Soha nem ír az RTDB-be.
// A védelmet a Security Rules (auth != null olvasáshoz) + a Hosting-domain adja.

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
const READINGS_PATH = `hives/${HIVE_ID}/readings`;

// Időablakok (másodperc). null = összes (limittel megfogva).
const WINDOWS = { "24h": 86400, "7d": 604800, "30d": 2592000, all: null };
const ALL_LIMIT = 10000; // "összes" biztonsági plafon (ne húzza le korlátlanul)
const SAMPLE_INTERVAL_SEC = 300; // 5 perc
const STALE_AFTER_SEC = SAMPLE_INTERVAL_SEC * 3; // 15 perc → "nincs friss adat"

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
let liveListenerRef = null; // épp aktív onChildAdded forrás (off()-hoz)
let liveBaselineTs = null; // a live listener startAt-ja (csak ennél újabb pushok)
const points = new Map(); // pushKey -> { ts, w_total, w1, w2 }
let chart = null;

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
  points.clear();
  if (chart) {
    chart.destroy();
    chart = null;
  }
  dashboardView.classList.add("hidden");
  splashView.classList.remove("hidden");
}

function showDashboard() {
  splashView.classList.add("hidden");
  dashboardView.classList.remove("hidden");
  ensureChart();
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
        ds("Össz-súly (w_total)", getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#f5b301", false),
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
// Adatlekérés — ts szerinti, indexelt query (NEM a teljes ág)
// ===========================================================================
function buildQuery(range) {
  const base = ref(db, READINGS_PATH);
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
  points.clear();
  try {
    const snap = await get(buildQuery(range));
    let n = 0;
    snap.forEach((child) => {
      if (addPoint(child.key, child.val())) n++;
    });
    redraw();
    if (range === "all" && n >= ALL_LIMIT) {
      setStatus(`⚠️ ${n} rekord (plafon: ${ALL_LIMIT}) — a legrégebbiek hiányozhatnak`);
    } else {
      setStatus(`${n} rekord betöltve`);
    }
  } catch (err) {
    setStatus("Hiba: " + (err.code || err.message));
    console.error(err);
  }
}

// Egy rekord validálása + felvétele a Map-be. true, ha új és érvényes.
function addPoint(key, val) {
  if (!val || typeof val.ts !== "number" || typeof val.w_total !== "number") return false;
  if (points.has(key)) return false;
  points.set(key, { ts: val.ts, w_total: val.w_total, w1: val.w1, w2: val.w2 });
  return true;
}

// ===========================================================================
// Élő frissítés — csak a betöltés óta ÚJONNAN érkezett pushok
// (startAt = baseline → meglévő rekordokra nem tüzel, csak az új mintákra)
// ===========================================================================
function attachLive() {
  detachLive();
  liveBaselineTs = Math.floor(Date.now() / 1000);
  const q = query(ref(db, READINGS_PATH), orderByChild("ts"), startAt(liveBaselineTs));
  liveListenerRef = q;
  onChildAdded(q, (child) => {
    if (addPoint(child.key, child.val())) {
      redraw();
      setStatus("Új minta érkezett");
    }
  });
}

function detachLive() {
  if (liveListenerRef) {
    off(liveListenerRef);
    liveListenerRef = null;
  }
}

// ===========================================================================
// Rajzolás + fejléc-állapot
// ===========================================================================
function redraw() {
  if (!chart) return;
  const sorted = [...points.values()].sort((a, b) => a.ts - b.ts);

  // Az "összes"/régi map-tartalom kiszűrése az aktuális ablakra (live append miatt).
  const windowSec = WINDOWS[currentRange];
  const minTs = windowSec == null ? -Infinity : Math.floor(Date.now() / 1000) - windowSec;
  const rows = sorted.filter((p) => p.ts >= minTs);

  chart.data.datasets[0].data = rows.map((p) => ({ x: p.ts * 1000, y: p.w_total }));
  chart.data.datasets[1].data = rows.map((p) => ({ x: p.ts * 1000, y: p.w1 }));
  chart.data.datasets[2].data = rows.map((p) => ({ x: p.ts * 1000, y: p.w2 }));
  chart.update();

  $("empty-state").classList.toggle("hidden", rows.length > 0);
  updateLatest(rows[rows.length - 1]);
}

function updateLatest(last) {
  const valEl = $("latest-value");
  const timeEl = $("latest-time");
  const banner = $("stale-banner");
  if (!last) {
    valEl.textContent = "– kg";
    timeEl.textContent = "nincs adat";
    banner.classList.add("hidden");
    return;
  }
  valEl.textContent = `${last.w_total.toFixed(2)} kg`;
  const d = new Date(last.ts * 1000);
  timeEl.textContent = d.toLocaleString("hu-HU"); // helyi időzóna
  const ageSec = Math.floor(Date.now() / 1000) - last.ts;
  banner.classList.toggle("hidden", ageSec <= STALE_AFTER_SEC);
}

function setStatus(msg) {
  $("status").textContent = msg;
}

// bee_scale dashboard — read-only. Adatréteg config.js (window.BEESCALE_CONFIG).
// Firebase modular SDK (ESM CDN) + ECharts (UMD global). Build-lépés nélkül.
//
// Elv: telepítés-választó (hive1/hive2) → telepítés-szintű időjárás sáv +
// kaptáranként egy kártya (aktuális érték, trend-sparkline, státusz). Kártyára
// kattintva zoomolható (kétujjas) részletes plot: súly + belső hőmérséklet.
// A súlyt hive2-n NYERSEN (HX711 count) mutatjuk — a kg-kalibráció a felhőben,
// később; ha megjelenik egy kalibrált mező, a config bővítésével átvált.

import { initializeApp } from "https://www.gstatic.com/firebasejs/12.4.0/firebase-app.js";
import { getAuth, signInAnonymously, onAuthStateChanged }
  from "https://www.gstatic.com/firebasejs/12.4.0/firebase-auth.js";
import { getDatabase, ref, query, orderByChild, limitToLast, get }
  from "https://www.gstatic.com/firebasejs/12.4.0/firebase-database.js";

const CFG = window.BEESCALE_CONFIG;
const $ = (id) => document.getElementById(id);

const app = initializeApp(CFG.firebase);
const auth = getAuth(app);
const db = getDatabase(app);

let activeHiveId = CFG.hives[0].id;
let refreshTimer = null;
let detailChart = null;
// hiveId -> { units: {unitId: [{ts,w,it}]}, env: [{ts,...}], temp: [{ts,...}] }
const store = {};

// ===========================================================================
// Auth (anonim — a Rules auth != null-t vár olvasáshoz)
// ===========================================================================
onAuthStateChanged(auth, (user) => {
  if (user) startApp();
  else signInAnonymously(auth).catch(showAuthError);
});

function showAuthError(err) {
  const code = (err && err.code) || "";
  let msg = "Csatlakozási hiba: " + (code || err.message || "ismeretlen");
  if (code.includes("operation-not-allowed"))
    msg = "Az anonim belépés nincs engedélyezve (Firebase Console → Authentication → Anonymous).";
  else if (code.includes("network")) msg = "Hálózati hiba a csatlakozáskor.";
  $("splash-status").classList.add("hidden");
  const e = $("splash-error"); e.textContent = msg; e.classList.remove("hidden");
}

function startApp() {
  $("splash").classList.add("hidden");
  $("app").classList.remove("hidden");
  renderTabs();
  $("refresh-btn").addEventListener("click", () => loadHive(activeHiveId));
  $("detail-close").addEventListener("click", closeDetail);
  $("detail-reset").addEventListener("click", resetZoom);
  $("detail").addEventListener("click", (e) => { if (e.target.id === "detail") closeDetail(); });
  selectHive(activeHiveId);
  if (refreshTimer) clearInterval(refreshTimer);
  refreshTimer = setInterval(() => loadHive(activeHiveId), CFG.refreshMs);
}

// ===========================================================================
// Telepítés-választó
// ===========================================================================
function renderTabs() {
  const nav = $("hive-tabs");
  nav.innerHTML = "";
  for (const h of CFG.hives) {
    const b = document.createElement("button");
    b.className = "tab" + (h.id === activeHiveId ? " active" : "");
    b.textContent = h.label;
    b.addEventListener("click", () => selectHive(h.id));
    nav.appendChild(b);
  }
}
function hiveById(id) { return CFG.hives.find((h) => h.id === id); }

function selectHive(id) {
  activeHiveId = id;
  renderTabs();
  loadHive(id);
}

// ===========================================================================
// Adatlekérés — ts-indexelt query, egységenként limitToLast
// ===========================================================================
function fetchSeries(path) {
  const q = query(ref(db, path), orderByChild("ts"), limitToLast(CFG.loadLimit));
  return get(q).then((snap) => {
    const out = [];
    snap.forEach((c) => { const v = c.val(); if (v && typeof v.ts === "number") out.push({ key: c.key, ...v }); });
    out.sort((a, b) => a.ts - b.ts);
    return out;
  }).catch((err) => { console.warn("fetch fail", path, err.code || err.message); return []; });
}

async function loadHive(id) {
  const hive = hiveById(id);
  setStatus("Betöltés…");
  try {
    const unitPromises = hive.units.map((u) => fetchSeries(u.path));
    const envP = hive.weather?.env ? fetchSeries(hive.weather.env.path) : Promise.resolve([]);
    const tempP = hive.weather?.temp ? fetchSeries(hive.weather.temp.path) : Promise.resolve([]);
    const [unitData, env, temp] = await Promise.all([Promise.all(unitPromises), envP, tempP]);

    const st = { units: {}, env, temp };
    hive.units.forEach((u, i) => {
      const w = u.weightField;
      // belső hőm forrása: a saját rekord mezője (node), vagy külön ág (master)
      const rows = unitData[i].map((r) => {
        const p = { ts: r.ts, w: typeof r[w] === "number" ? r[w] : null, mask: r.sensor_error_mask };
        if (u.innerTempFrom && u.innerTempFrom.field && !u.innerTempFrom.path)
          p.it = typeof r[u.innerTempFrom.field] === "number" ? r[u.innerTempFrom.field] : null;
        if (u.vbatField && typeof r[u.vbatField] === "number") p.vbat = r[u.vbatField];
        return p;
      });
      st.units[u.id] = rows;
    });
    store[id] = st;

    if (id === activeHiveId) { renderWeather(hive, st); renderCards(hive, st); }
    setStatus(`Frissítve · ${new Date().toLocaleTimeString("hu-HU")}`);
  } catch (err) {
    console.error(err);
    setStatus("Hiba a betöltéskor: " + (err.code || err.message));
  }
}

// belső-hőm sorozat egy egységhez (node: saját it; master: külön temp-ág mező)
function innerTempSeries(hive, st, unit) {
  const src = unit.innerTempFrom;
  if (!src) return [];
  if (src.path === "weather.temp") {
    const f = src.field;
    return st.temp.filter((r) => typeof r[f] === "number").map((r) => [r.ts * 1000, r[f]]);
  }
  return st.units[unit.id].filter((r) => typeof r.it === "number").map((r) => [r.ts * 1000, r.it]);
}

// ===========================================================================
// Időjárás sáv
// ===========================================================================
function latest(arr) { return arr.length ? arr[arr.length - 1] : null; }

function renderWeather(hive, st) {
  const el = $("weather");
  el.innerHTML = "";
  const groups = [];
  if (hive.weather?.env) groups.push([hive.weather.env, latest(st.env)]);
  if (hive.weather?.temp) groups.push([hive.weather.temp, latest(st.temp)]);
  for (const [grp, rec] of groups) {
    for (const m of grp.metrics) {
      let v = rec ? rec[m.field] : null;
      let txt = "–";
      if (typeof v === "number") { if (m.scale) v *= m.scale; txt = v.toFixed(m.fmt ?? 1); }
      const chip = document.createElement("div");
      chip.className = "wchip";
      chip.innerHTML = `<span class="wi">${m.icon || ""}</span>
        <span class="wtxt"><span class="wv">${txt}<small> ${m.unit}</small></span>
        <span class="wl">${m.label}</span></span>`;
      el.appendChild(chip);
    }
  }
}

// ===========================================================================
// Kaptár-kártyák
// ===========================================================================
function renderCards(hive, st) {
  const el = $("cards");
  el.innerHTML = "";
  for (const u of hive.units) {
    const rows = (st.units[u.id] || []).filter((r) => r.w !== null);
    el.appendChild(buildCard(hive, u, rows));
  }
}

function buildCard(hive, unit, rows) {
  const card = document.createElement("div");
  card.className = "card";
  const last = rows.length ? rows[rows.length - 1] : null;
  const first = rows.length ? rows[0] : null;

  // fejléc + állapot-badge-ek
  const badges = [];
  const ageBadge = freshnessBadge(last);
  badges.push(ageBadge);
  if (unit.power === "mains") badges.push({ txt: "⚡ hálózat", cls: "" });
  else badges.push(batteryBadge(unit, last));
  const errB = errorBadge(unit, last);
  if (errB) badges.push(errB);

  const badgeHtml = badges.map((b) => `<span class="badge ${b.cls}">${b.txt}</span>`).join("");

  // fő érték + trend
  const valTxt = last ? fmtWeight(hive, last.w) : "–";
  const delta = (last && first) ? last.w - first.w : null;
  const dHtml = deltaHtml(hive, delta);

  card.innerHTML = `
    <div class="card-head">
      <span class="card-name">${unit.label}</span>
      <span class="card-badges">${badgeHtml}</span>
    </div>
    <div class="card-value"><span class="v">${valTxt.v}</span><span class="u">${valTxt.u}</span></div>
    <div class="card-delta ${dHtml.cls}">${dHtml.txt}</div>
    ${sparkSvg(rows)}
    <div class="card-foot">
      ${last ? `<span class="fi">🕒 ${ageText(last.ts)}</span>` : ""}
      ${footInnerTemp(unit, last)}
      <span class="fi">📈 ${rows.length} pont</span>
    </div>`;

  card.addEventListener("click", () => openDetail(hive, unit));
  return card;
}

function footInnerTemp(unit, last) {
  // node: a rekord it-je; master: a foot-ban nem mutatjuk (a temp sáv úgyis kint van)
  if (unit.innerTempFrom && !unit.innerTempFrom.path && last && typeof last.it === "number")
    return `<span class="fi">🌡 ${last.it.toFixed(1)} °C</span>`;
  return "";
}

// ---- állapot-jelzők ----
function freshnessBadge(last) {
  if (!last) return { txt: "nincs adat", cls: "danger" };
  const ageMin = (Date.now() / 1000 - last.ts) / 60;
  if (ageMin > CFG.staleMinutes) return { txt: "⚠ elavult", cls: "warn" };
  return { txt: "friss", cls: "ok" };
}
function batteryBadge(unit, last) {
  if (unit.noVbatHw) return { txt: "🔋 nincs HW", cls: "" };
  const mv = last && typeof last.vbat === "number" ? last.vbat : null;
  if (mv == null || mv === 0) return { txt: "🔋 –", cls: "" };
  const v = (mv / 1000).toFixed(2);
  // 18650: ~3.3V ürült … 4.2V teli
  const pct = Math.max(0, Math.min(100, Math.round((mv - 3300) / (4200 - 3300) * 100)));
  const cls = pct < 15 ? "danger" : pct < 35 ? "warn" : "ok";
  return { txt: `🔋 ${v}V`, cls };
}
function errorBadge(unit, last) {
  if (!last || typeof last.mask !== "number" || last.mask === 0) return null;
  let mask = last.mask;
  if (unit.noVbatHw) mask &= ~(1 << 6); // node1: a VBAT-bit várt, nem hiba
  if (mask === 0) return null;
  const names = [];
  const map = ["HX711", "node-DS", "kaptár-DS", "mérleg-DS", "AHT20", "BMP280", "VBAT"];
  for (let i = 0; i < map.length; i++) if (mask & (1 << i)) names.push(map[i]);
  return { txt: "⚠ " + names.join(","), cls: "danger" };
}

// ---- súly-formázás (raw vs kg) ----
function fmtWeight(hive, w) {
  if (w == null) return { v: "–", u: "" };
  if (hive.weightUnit === "kg") return { v: w.toFixed(2), u: "kg" };
  // raw: ezres tagolás
  return { v: Math.round(w).toLocaleString("hu-HU"), u: "raw" };
}
function deltaHtml(hive, d) {
  if (d == null) return { txt: "&nbsp;", cls: "flat" };
  const up = d > 0, cls = Math.abs(d) < 1e-9 ? "flat" : up ? "up" : "down";
  let mag;
  if (hive.weightUnit === "kg") mag = `${up ? "+" : ""}${d.toFixed(2)} kg`;
  else {
    const a = Math.abs(d);
    const s = a >= 1000 ? (d / 1000).toFixed(1) + "k" : Math.round(d).toString();
    mag = (up ? "+" : d < 0 ? "−" : "") + s.replace("-", "");
  }
  const arrow = cls === "flat" ? "→" : up ? "↗" : "↘";
  return { txt: `${arrow} ${mag} <span class="muted">az ablakban</span>`, cls };
}

// ---- inline SVG sparkline ----
function sparkSvg(rows) {
  const W = 240, H = 46, pad = 3;
  if (rows.length < 2) return `<svg class="spark" viewBox="0 0 ${W} ${H}"></svg>`;
  const xs = rows.map((r) => r.ts), ys = rows.map((r) => r.w);
  const x0 = xs[0], x1 = xs[xs.length - 1] || x0 + 1;
  const yMin = Math.min(...ys), yMax = Math.max(...ys), yr = yMax - yMin || 1;
  const px = (x) => pad + (x - x0) / (x1 - x0 || 1) * (W - 2 * pad);
  const py = (y) => H - pad - (y - yMin) / yr * (H - 2 * pad);
  const d = rows.map((r, i) => `${i ? "L" : "M"}${px(r.ts).toFixed(1)},${py(r.w).toFixed(1)}`).join("");
  const up = ys[ys.length - 1] >= ys[0];
  const col = up ? "var(--ok)" : "var(--accent-2)";
  return `<svg class="spark" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">
    <path d="${d}" fill="none" stroke="${col}" stroke-width="1.6" stroke-linejoin="round"/></svg>`;
}

function ageText(ts) {
  const s = Math.floor(Date.now() / 1000 - ts);
  if (s < 90) return "most";
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} perce`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h} órája`;
  return `${Math.floor(h / 24)} napja`;
}

// ===========================================================================
// Részletes plot (ECharts) — súly + belső hőm, kétujjas zoom, auto-skála
// ===========================================================================
function openDetail(hive, unit) {
  const st = store[hive.id];
  const rows = (st.units[unit.id] || []).filter((r) => r.w !== null);
  const weightData = rows.map((r) => [r.ts * 1000, r.w]);
  const tempData = innerTempSeries(hive, st, unit);
  const tempLabel = unit.innerTempFrom?.label || "belső hőm";
  const wUnit = hive.weightUnit === "kg" ? "kg" : "raw";

  $("detail-title").textContent = `${hive.label} · ${unit.label}`;
  $("detail-sub").textContent = `${rows.length} mérés · súly (${wUnit}) + ${tempLabel}`;
  $("detail").classList.remove("hidden");

  if (!detailChart) detailChart = echarts.init($("chart"), null, { renderer: "canvas" });
  detailChart.setOption(buildChartOption(weightData, tempData, wUnit, tempLabel), true);
  requestAnimationFrame(() => detailChart.resize());
}

function buildChartOption(weightData, tempData, wUnit, tempLabel) {
  const AMBER = "#f5b301", CYAN = "#4fc3f7", GRID = "#242a34", MUT = "#8b93a1";
  return {
    animation: false,
    backgroundColor: "transparent",
    grid: { top: 42, left: 8, right: 8, bottom: 56, containLabel: true },
    legend: { top: 6, textStyle: { color: "#eceef1" }, icon: "roundRect" },
    tooltip: {
      trigger: "axis",
      backgroundColor: "#191d24", borderColor: GRID, textStyle: { color: "#eceef1" },
      axisPointer: { type: "cross", label: { backgroundColor: "#20252e" } },
      valueFormatter: null,
    },
    xAxis: {
      type: "time",
      axisLine: { lineStyle: { color: GRID } },
      axisLabel: { color: MUT, hideOverlap: true },
      splitLine: { show: true, lineStyle: { color: GRID } },
    },
    yAxis: [
      {
        type: "value", scale: true, name: wUnit, position: "left",
        nameTextStyle: { color: AMBER }, axisLabel: { color: MUT },
        splitLine: { lineStyle: { color: GRID } },
      },
      {
        type: "value", scale: true, name: "°C", position: "right",
        nameTextStyle: { color: CYAN }, axisLabel: { color: MUT },
        splitLine: { show: false },
      },
    ],
    // inside = kétujjas pinch + húzás mobilon + görgő; filter → az Y-tengelyek
    // a látható X-ablakhoz igazodnak (scale:true).
    dataZoom: [
      { type: "inside", xAxisIndex: 0, filterMode: "filter", zoomOnMouseWheel: true, moveOnMouseMove: true },
    ],
    series: [
      {
        name: `Súly (${wUnit})`, type: "line", yAxisIndex: 0,
        data: weightData, showSymbol: false, smooth: false, connectNulls: true,
        lineStyle: { color: AMBER, width: 2.2 }, itemStyle: { color: AMBER },
        emphasis: { focus: "series" },
      },
      {
        name: tempLabel + " (°C)", type: "line", yAxisIndex: 1,
        data: tempData, showSymbol: false, smooth: false, connectNulls: true,
        lineStyle: { color: CYAN, width: 1.6, type: "dashed" }, itemStyle: { color: CYAN },
        emphasis: { focus: "series" },
      },
    ],
  };
}

function resetZoom() {
  if (detailChart) detailChart.dispatchAction({ type: "dataZoom", start: 0, end: 100 });
}
function closeDetail() { $("detail").classList.add("hidden"); }

window.addEventListener("resize", () => { if (detailChart && !$("detail").classList.contains("hidden")) detailChart.resize(); });

function setStatus(msg) { $("load-status").textContent = msg; }

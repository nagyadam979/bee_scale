// bee_scale dashboard — read-only. Adatréteg config.js (window.BEESCALE_CONFIG).
// Firebase modular SDK (ESM CDN) + ECharts (UMD global). Build-lépés nélkül.
//
// Elv: telepítés-választó (hive1/hive2) → telepítés-szintű időjárás-sáv +
// EGY fő SÚLY-CHART, amin az adott telepítés összes mérlegének görbéje látszik
// (kg-ban), alul timeline-sliderrel (teljes történet böngészés) + kétujjas zoom.
// Alatta belső-hőm chart és kompakt állapot-kártyák.
//
// SÚLY-EGYSÉG:
//  * hive1 (legacy): valódi kalibrált kg (w). A fő görbe a w1+w2 ÖSSZEG.
//  * hive2 (wireless): a weight_raw NYERS count → PLACEHOLDER kg-normalizálás
//    egységenként (config.placeholderKg): kg = base + (raw - median(raw))*kgPerCount.
//    EZ NEM VALÓDI SÚLY — a UI "becsült kg"-ként jelöli; a valódi kalibráció +
//    hőm-kompenzáció külön, felhős (Cloud Function) munka lesz.

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

// vonal-színpaletta (a fő/össz görbe mindig az első = méz-sárga)
const PALETTE = ["#f5b301", "#4fc3f7", "#81c784", "#ba68c8", "#ff8a65", "#e57373"];
const GRID = "#242a34", MUT = "#8b93a1", TXT = "#eceef1";

let activeHiveId = CFG.hives[0].id;
let refreshTimer = null;
const charts = {}; // id -> echarts instance
const store = {};  // hiveId -> { units:{id:[{ts,w,it,mask,vbat}]}, env:[], temp:[] }

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
  charts["weight-chart"] = echarts.init($("weight-chart"), null, { renderer: "canvas" });
  charts["temp-chart"] = echarts.init($("temp-chart"), null, { renderer: "canvas" });
  renderTabs();
  $("refresh-btn").addEventListener("click", () => loadHive(activeHiveId));
  document.querySelectorAll("[data-reset]").forEach((b) =>
    b.addEventListener("click", () => resetZoom(b.dataset.reset)));
  window.addEventListener("resize", () => Object.values(charts).forEach((c) => c.resize()));
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
      const wf = u.weightField;
      st.units[u.id] = unitData[i].map((r) => {
        const p = { ts: r.ts, w: typeof r[wf] === "number" ? r[wf] : null, mask: r.sensor_error_mask };
        if (u.innerTempFrom && u.innerTempFrom.field && !u.innerTempFrom.path)
          p.it = typeof r[u.innerTempFrom.field] === "number" ? r[u.innerTempFrom.field] : null;
        if (u.vbatField && typeof r[u.vbatField] === "number") p.vbat = r[u.vbatField];
        return p;
      });
    });
    store[id] = st;

    if (id === activeHiveId) {
      renderWeather(hive, st);
      renderWeightChart(hive, st);
      renderTempChart(hive, st);
      renderCards(hive, st);
      $("weight-note").textContent = hive.weightEstimated
        ? "⚠ becsült kg (placeholder — valódi kalibráció még nincs)"
        : "kalibrált kg";
    }
    setStatus(`Frissítve · ${new Date().toLocaleTimeString("hu-HU")}`);
  } catch (err) {
    console.error(err);
    setStatus("Hiba a betöltéskor: " + (err.code || err.message));
  }
}

// ===========================================================================
// Súly-normalizálás (kg)
// ===========================================================================
function median(nums) {
  if (!nums.length) return 0;
  const s = [...nums].sort((a, b) => a - b), m = s.length >> 1;
  return s.length & 1 ? s[m] : (s[m - 1] + s[m]) / 2;
}
// egy egység súly-sorozata kg-ban [[tsMs, kg]]
function unitWeightKg(hive, unit, rows) {
  const r = rows.filter((p) => p.w !== null);
  if (hive.weightUnit === "kg") return r.map((p) => [p.ts * 1000, p.w]);
  const med = median(r.map((p) => p.w));
  const { base, kgPerCount } = unit.placeholderKg || { base: 0, kgPerCount: 1 };
  return r.map((p) => [p.ts * 1000, +(base + (p.w - med) * kgPerCount).toFixed(3)]);
}
// hive1 w1+w2 összeg — legközelebbi-szomszéd párosítás (a régi frontend logikája)
function pairTotalKg(rows1, rows2, tolSec) {
  const a = rows1.filter((p) => p.w !== null), b = rows2.filter((p) => p.w !== null);
  const out = [];
  if (!a.length || !b.length) return out;
  let j = 0;
  for (const x of a) {
    while (j + 1 < b.length && Math.abs(b[j + 1].ts - x.ts) <= Math.abs(b[j].ts - x.ts)) j++;
    if (Math.abs(b[j].ts - x.ts) <= tolSec) out.push([x.ts * 1000, +(x.w + b[j].w).toFixed(3)]);
  }
  return out;
}

// ===========================================================================
// Fő súly-chart
// ===========================================================================
function renderWeightChart(hive, st) {
  const series = [], selected = {};
  if (hive.aggregate) {
    // fő görbe = össz (w1+w2), a rúdak külön vonalként de alapból rejtve
    const total = pairTotalKg(st.units[hive.units[0].id], st.units[hive.units[1].id], hive.aggregate.tolSec);
    series.push(lineSeries(hive.aggregate.label, total, PALETTE[0], 2.6));
    selected[hive.aggregate.label] = true;
    hive.units.forEach((u, i) => {
      series.push(lineSeries(u.label, unitWeightKg(hive, u, st.units[u.id]), PALETTE[i + 1], 1.6));
      selected[u.label] = false;
    });
  } else {
    hive.units.forEach((u, i) => {
      series.push(lineSeries(u.label, unitWeightKg(hive, u, st.units[u.id]), PALETTE[i % PALETTE.length], 2.2));
      selected[u.label] = true;
    });
  }
  charts["weight-chart"].setOption(chartOption(series, "kg", selected, (v) => v.toFixed(2) + " kg"), true);
  requestAnimationFrame(() => charts["weight-chart"].resize());
}

// ===========================================================================
// Belső-hőm chart
// ===========================================================================
function innerTempSeries(hive, st, unit) {
  const src = unit.innerTempFrom;
  if (!src) return [];
  if (src.path === "weather.temp")
    return st.temp.filter((r) => typeof r[src.field] === "number").map((r) => [r.ts * 1000, r[src.field]]);
  return st.units[unit.id].filter((r) => typeof r.it === "number").map((r) => [r.ts * 1000, r.it]);
}
function renderTempChart(hive, st) {
  const series = [];
  hive.units.forEach((u, i) => {
    if (u.innerTempFrom) {
      const label = `${u.label} · ${u.innerTempFrom.label}`;
      series.push(lineSeries(label, innerTempSeries(hive, st, u), PALETTE[(hive.aggregate ? i + 1 : i) % PALETTE.length], 1.8));
    }
  });
  if (!series.length && hive.weather?.temp) {
    // legacy: nincs egység-szintű belső hőm → a telepítés temp-metrikái (kaptár-hőm)
    hive.weather.temp.metrics.forEach((m, i) => {
      const data = st.temp.filter((r) => typeof r[m.field] === "number").map((r) => [r.ts * 1000, r[m.field]]);
      series.push(lineSeries(m.label, data, PALETTE[(i + 1) % PALETTE.length], 1.8));
    });
  }
  charts["temp-chart"].setOption(chartOption(series, "°C", null, (v) => v.toFixed(1) + " °C"), true);
  requestAnimationFrame(() => charts["temp-chart"].resize());
}

// ===========================================================================
// ECharts közös építők
// ===========================================================================
function lineSeries(name, data, color, width) {
  return {
    name, type: "line", data, showSymbol: false, smooth: false, connectNulls: true,
    sampling: "lttb", lineStyle: { color, width }, itemStyle: { color },
    emphasis: { focus: "series" },
  };
}
function chartOption(series, yName, selected, valFmt) {
  return {
    animation: false, backgroundColor: "transparent",
    color: PALETTE,
    grid: { top: 40, left: 8, right: 10, bottom: 58, containLabel: true },
    legend: { top: 4, textStyle: { color: TXT }, icon: "roundRect", selected: selected || undefined },
    tooltip: {
      trigger: "axis", backgroundColor: "#191d24", borderColor: GRID, textStyle: { color: TXT },
      axisPointer: { type: "cross", label: { backgroundColor: "#20252e" } },
      valueFormatter: (v) => (typeof v === "number" ? valFmt(v) : "–"),
    },
    xAxis: {
      type: "time", axisLine: { lineStyle: { color: GRID } },
      axisLabel: { color: MUT, hideOverlap: true }, splitLine: { show: true, lineStyle: { color: GRID } },
    },
    yAxis: {
      type: "value", scale: true, name: yName, nameTextStyle: { color: MUT },
      axisLabel: { color: MUT }, splitLine: { lineStyle: { color: GRID } },
    },
    // inside = kétujjas pinch/húzás + görgő; slider = teljes timeline böngésző.
    // filterMode:filter → az Y-tengely a látható X-ablakhoz igazodik (scale:true).
    dataZoom: [
      { type: "inside", filterMode: "filter", zoomOnMouseWheel: true, moveOnMouseMove: true },
      { type: "slider", filterMode: "filter", bottom: 8, height: 20,
        borderColor: GRID, fillerColor: "rgba(245,179,1,.12)",
        handleStyle: { color: "#f5b301" }, textStyle: { color: MUT },
        dataBackground: { lineStyle: { color: GRID }, areaStyle: { color: "#20252e" } } },
    ],
    series,
  };
}

function resetZoom(chartId) {
  const c = charts[chartId];
  if (c) c.dispatchAction({ type: "dataZoom", start: 0, end: 100 });
}

// ===========================================================================
// Időjárás-sáv
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
      let v = rec ? rec[m.field] : null, txt = "–";
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
// Kompakt állapot-kártyák
// ===========================================================================
function renderCards(hive, st) {
  const el = $("cards");
  el.innerHTML = "";
  for (const u of hive.units) {
    const kg = unitWeightKg(hive, u, st.units[u.id]);
    el.appendChild(buildCard(hive, u, st.units[u.id], kg));
  }
}
function buildCard(hive, unit, rawRows, kg) {
  const card = document.createElement("div");
  card.className = "card";
  const lastRaw = rawRows.filter((r) => r.w !== null).slice(-1)[0] || null;
  const lastKg = kg.length ? kg[kg.length - 1][1] : null;
  const firstKg = kg.length ? kg[0][1] : null;

  const badges = [freshnessBadge(lastRaw)];
  if (unit.power === "mains") badges.push({ txt: "⚡ hálózat", cls: "" });
  else badges.push(batteryBadge(unit, lastRaw));
  const errB = errorBadge(unit, lastRaw);
  if (errB) badges.push(errB);
  const badgeHtml = badges.map((b) => `<span class="badge ${b.cls}">${b.txt}</span>`).join("");

  const valTxt = lastKg == null ? "–" : (hive.weightEstimated ? "~" : "") + lastKg.toFixed(hive.weightEstimated ? 1 : 2);
  const d = (lastKg != null && firstKg != null) ? lastKg - firstKg : null;
  const dCls = d == null ? "flat" : Math.abs(d) < 0.05 ? "flat" : d > 0 ? "up" : "down";
  const dTxt = d == null ? "&nbsp;" : `${dCls === "flat" ? "→" : d > 0 ? "↗" : "↘"} ${d > 0 ? "+" : ""}${d.toFixed(1)} kg <span class="muted">az ablakban</span>`;

  card.innerHTML = `
    <div class="card-head">
      <span class="card-name">${unit.label}</span>
      <span class="card-badges">${badgeHtml}</span>
    </div>
    <div class="card-value"><span class="v">${valTxt}</span><span class="u">kg</span></div>
    <div class="card-delta ${dCls}">${dTxt}</div>
    <div class="card-foot">
      ${lastRaw ? `<span class="fi">🕒 ${ageText(lastRaw.ts)}</span>` : ""}
      ${lastRaw && typeof lastRaw.it === "number" ? `<span class="fi">🌡 ${lastRaw.it.toFixed(1)} °C</span>` : ""}
    </div>`;
  return card;
}

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
  const pct = Math.max(0, Math.min(100, Math.round((mv - 3300) / (4200 - 3300) * 100)));
  const cls = pct < 15 ? "danger" : pct < 35 ? "warn" : "ok";
  return { txt: `🔋 ${(mv / 1000).toFixed(2)}V`, cls };
}
function errorBadge(unit, last) {
  if (!last || typeof last.mask !== "number" || last.mask === 0) return null;
  let mask = last.mask;
  if (unit.noVbatHw) mask &= ~(1 << 6); // node1: a VBAT-bit várt, nem hiba
  if (mask === 0) return null;
  const map = ["HX711", "node-DS", "kaptár-DS", "mérleg-DS", "AHT20", "BMP280", "VBAT"];
  const names = [];
  for (let i = 0; i < map.length; i++) if (mask & (1 << i)) names.push(map[i]);
  return { txt: "⚠ " + names.join(","), cls: "danger" };
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

function setStatus(msg) { $("load-status").textContent = msg; }

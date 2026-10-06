// bee_scale dashboard — read-only. Adatréteg config.js (window.BEESCALE_CONFIG).
// Firebase modular SDK (ESM CDN) + ECharts (UMD global). Build-lépés nélkül.
//
// Elv: telepítés-választó (hive1/hive2) → telepítés-szintű környezet-sáv
// (24h csapadék-csempe + szenzor-chipek) + EGY fő SÚLY-CHART (össz-súly) +
// "Környezet" chart (legördülőből EGY szenzor idősora) + board-kártyák.
//
// BOARD-MODELL: minden mérőrúd (node_1..3 ÉS master) a SAJÁT board-rekordjában
// viszi a súlyt (weight_raw) és a kalibrációs cella-hőjét (temp_c). A környezeti
// adatok (levegő/pára/nyomás/eső + kaptár-szonda hive_c) az env ágban.
//
// SÚLY-VONAL (egy össz-görbe / telepítés):
//  * hive1 (legacy): valódi kalibrált kg (w) → w1+w2 összeg.
//  * hive2 (wireless): a weight_raw NYERS count → valódi kalibrált kg boardonként
//    (config.calib: kg = (raw - zero_count - tempCorr)/count_per_kg; node=log,
//    master=lineáris hőkorrekció), majd a 4 board ciklusonkénti VALÓS ÖSSZEGE. Ha egy
//    board calib-ja hiányos, arra placeholderKg-fallback (median-alap) van.
//  Az async pontokat a legközelebbi mérési ciklushoz rendeljük, és csak ott
//  összegzünk, ahol MINDEN boardnak van tényleges mintája. Adatlyukon nincs vonal.
//
// MOBIL: az IDŐ-tengelyt nagyítod/tolod (kétujjas pinch = zoom, egyujjas
// VÍZSZINTES húzás = idő-pan); az egyujjas FÜGGŐLEGES húzás az OLDALT görgeti
// (nem plot-pan). A függőleges mindig a látható adathoz igazodik → a vonal nem
// csúszik ki a képből. NINCS alsó csúszka, NINCS érték-kiíró tooltip.
// KRITIKUS: touch-action:pan-y (CSS+JS) → a böngésző natívan görget függőlegesen;
// az irány-eldöntést capture-fázisú touch-figyelő végzi (l. hardenTouch).

import { initializeApp } from "https://www.gstatic.com/firebasejs/12.4.0/firebase-app.js";
import { getAuth, signInAnonymously, onAuthStateChanged }
  from "https://www.gstatic.com/firebasejs/12.4.0/firebase-auth.js";
import { getDatabase, ref, query, orderByChild, limitToLast, get }
  from "https://www.gstatic.com/firebasejs/12.4.0/firebase-database.js";
import { densifyTotalSeries, estimateSamplePeriod, totalKgSeries }
  from "./weight-total.js";

const CFG = window.BEESCALE_CONFIG;
const $ = (id) => document.getElementById(id);

const app = initializeApp(CFG.firebase);
const auth = getAuth(app);
const db = getDatabase(app);

// vonal-színpaletta (a fő/össz görbe mindig az első = méz-sárga)
const PALETTE = ["#f5b301", "#4fc3f7", "#81c784", "#ba68c8", "#ff8a65", "#e57373"];
const GRID = "#242a34", MUT = "#8b93a1", TXT = "#eceef1";
// Interpolációs pont-sűrítés: a ritka mérések közé ennyi lépésenként szúrunk be
// lineárisan interpolált pontot (hogy legyen mire ránagyítani); gap-enként max ennyi
// (nagy adatkimaradást nem tömünk tele pontokkal).
const DENSIFY_MS = 5 * 60 * 1000, DENSIFY_MAX = 6;
// Enyhe vonal-simítás (ECharts spline, 0..1) a folytonos idősorokra (súly, hőm,
// pára, nyomás) → kevésbé szögletes a törtvonal. Kicsi érték, hogy a spline ne
// lőjön túl (ne rajzoljon a mérési tartományon kívüli hamis púpot/gödröt).
const LINE_SMOOTH = 0.3;

let activeHiveId = CFG.hives[0].id;
let refreshTimer = null;
let envSelIdx = 0;   // a "Környezet" chart legördülőjének kiválasztott indexe
const charts = {}; // id -> echarts instance
const store = {};  // hiveId -> { boards:{id:[{ts,w,it,mask,vbat}]}, byPath:{path:[rawRec]} }

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
  charts["env-chart"] = echarts.init($("env-chart"), null, { renderer: "canvas" });
  hardenTouch("weight-chart");
  hardenTouch("env-chart");
  // A nap-sáv (dátumok) a látható szakaszhoz igazodik minden zoom/pan után
  // (és a ⟲ Teljes nézet reset is dataZoom-ot vált ki → szintén frissül).
  ["weight-chart", "env-chart"].forEach((id) =>
    charts[id].on("dataZoom", () => updateDayBands(id)));
  renderTabs();
  $("refresh-btn").addEventListener("click", () => loadHive(activeHiveId));
  document.querySelectorAll("[data-reset]").forEach((b) =>
    b.addEventListener("click", () => resetZoom(b.dataset.reset)));
  document.querySelectorAll("[data-range-days]").forEach((b) =>
    b.addEventListener("click", () => setQuickRange(b.dataset.chart, Number(b.dataset.rangeDays))));
  window.addEventListener("resize", () => {
    Object.values(charts).forEach((c) => c.resize());
    ["weight-chart", "env-chart"].forEach(updateDayBands); // a napok pixel-pozíciója változott
  });
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
  envSelIdx = 0; // új telepítésnél az első szenzorra állunk
  renderTabs();
  loadHive(id);
}

// ===========================================================================
// Adatlekérés — ts-indexelt query, utanként limitToLast
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
    // Minden EGYEDI RTDB-utat egyszer töltünk (board-ok + env-szenzorok + eső-csempe).
    const sensors = hive.env?.sensors || [];
    const rainTile = hive.env?.rainTile || null;
    const paths = new Set();
    hive.boards.forEach((b) => paths.add(b.path));
    sensors.forEach((s) => paths.add(s.path));
    if (rainTile) paths.add(rainTile.path);
    const pathList = [...paths];
    const fetched = await Promise.all(pathList.map((p) => fetchSeries(p)));
    const byPath = {};
    pathList.forEach((p, i) => { byPath[p] = fetched[i]; });

    const st = { boards: {}, byPath };
    hive.boards.forEach((b) => {
      const wf = b.weightField;
      st.boards[b.id] = (byPath[b.path] || []).map((r) => {
        const p = { ts: r.ts, w: typeof r[wf] === "number" ? r[wf] : null, mask: r.sensor_error_mask };
        // cella-hő a SAJÁT board-rekordból (node: temp_c; master: temp_c = régi scale_c).
        if (b.innerTempFrom && b.innerTempFrom.field)
          p.it = typeof r[b.innerTempFrom.field] === "number" ? r[b.innerTempFrom.field] : null;
        if (b.vbatField && typeof r[b.vbatField] === "number") p.vbat = r[b.vbatField];
        return p;
      });
    });
    store[id] = st;

    if (id === activeHiveId) {
      renderWeather(hive, st);
      renderWeightChart(hive, st);
      renderEnvChart(hive, st);
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
// Centrált gördülő medián a [[tsMs, v]] kg-sorozaton — az éles (prototípus) app
// spike-szűrője (prototype/public/app.js medianSmooth). A HW-eredetű 1-2 mintás
// kiugró tüskéket (±pár kg, HX711/mechanikai zaj) levágja, a valós trendet és a
// finom dinamikát megőrzi. Tisztán MEGJELENÍTÉSI szűrés — a nyers DB-adat érintetlen.
function medianSmoothSeries(series, k) {
  const n = series.length;
  if (k <= 1 || n === 0) return series;
  const half = k >> 1;
  const out = new Array(n);
  for (let i = 0; i < n; i++) {
    const lo = Math.max(0, i - half), hi = Math.min(n, i + half + 1);
    const win = [];
    for (let j = lo; j < hi; j++) win.push(series[j][1]);
    win.sort((a, b) => a - b);
    const m = win.length;
    const med = m & 1 ? win[m >> 1] : (win[m / 2 - 1] + win[m / 2]) / 2;
    out[i] = [series[i][0], +med.toFixed(3)];
  }
  return out;
}
// egy board súly-sorozata kg-ban [[tsMs, kg]]. A cella-hő MINDEN boardnál a saját
// rekordból (p.it) jön — a master temp_c-je is a board-rekordban van már.
function boardWeightKg(hive, board, rows) {
  const r = rows.filter((p) => p.w !== null);
  let series;
  if (hive.weightUnit === "kg") {
    series = r.map((p) => [p.ts * 1000, p.w]);
  } else {
    const c = board.calib;
    const calibReady = c && c.zero_count != null && c.count_per_kg != null && c.refTemp != null;
    if (!calibReady) {
      // Súly-kalibráció még nincs kész → placeholder-fallback (medián-alapú).
      const med = median(r.map((p) => p.w));
      const { base, kgPerCount } = board.placeholderKg || { base: 0, kgPerCount: 1 };
      series = r.map((p) => [p.ts * 1000, +(base + (p.w - med) * kgPerCount).toFixed(3)]);
    } else {
      // Valódi kalibrált kg: kg = (raw - zero_count - tempCorr) / count_per_kg.
      series = r.map((p) => {
        const cellTemp = typeof p.it === "number" ? p.it : null;
        let tempCorr = 0;
        if (cellTemp != null) {
          if (c.tempModel === "log") tempCorr = c.a * Math.log((cellTemp + c.c) / (c.refTemp + c.c));
          else if (c.tempModel === "linear") tempCorr = c.tempCoef * (cellTemp - c.refTemp);
        }
        const kg = (p.w - c.zero_count - tempCorr) / c.count_per_kg;
        return [p.ts * 1000, +kg.toFixed(3)];
      });
    }
  }
  // Spike-szűrés CSAK ott, ahol a config kéri (hive1 prototípus: HX711/mech. zaj).
  // A totálba összegzés ELŐTT, a per-board sorozaton → a kártya és a plot is tiszta.
  // hive2-t NEM szűrjük (más mintavételezés) — nincs spikeFilter a configjában.
  const mk = hive.spikeFilter?.medianK;
  if (mk > 1) series = medianSmoothSeries(series, mk);
  return series;
}
// A ritka (pl. 20 perces) mérések KÖZÉ lineárisan interpolált pontokat szúr, hogy
// nagyításnál a szakaszon is legyenek pontok, ne csak egyenes. Az eredetieket megtartja.
function densify(data, stepMs, maxPerGap) {
  if (data.length < 2) return data;
  const out = [];
  for (let i = 0; i < data.length - 1; i++) {
    const a = data[i], b = data[i + 1];
    out.push(a);
    const gap = b[0] - a[0];
    const n = Math.min(maxPerGap, Math.floor(gap / stepMs) - 1);
    for (let k = 1; k <= n; k++) {
      const t = a[0] + k * stepMs;
      out.push([t, +(a[1] + (b[1] - a[1]) * (t - a[0]) / gap).toFixed(3)]);
    }
  }
  out.push(data[data.length - 1]);
  return out;
}

// ===========================================================================
// Fő súly-chart — telepítésenként EGYETLEN vonal (össz-súly)
// ===========================================================================
function renderWeightChart(hive, st) {
  const perBoard = hive.boards.map((b) => boardWeightKg(hive, b, st.boards[b.id]));
  // A total-pontok ciklusonként összegzett, VALÓS board-mérések. Normál ciklusok
  // között a plot sűríthet; 1,8 ciklusnál hosszabb adatlyukat null-pont szakít meg.
  const periodMs = estimateSamplePeriod(perBoard);
  const total = densifyTotalSeries(
    totalKgSeries(perBoard), DENSIFY_MS, DENSIFY_MAX, periodMs * 1.8);
  const series = [lineSeries("Össz-súly", total, PALETTE[0], 2.6,
    { smooth: LINE_SMOOTH, connectNulls: false })];
  charts["weight-chart"].setOption(chartOption(series, "kg", null, (v) => v.toFixed(2) + " kg"), true);
  requestAnimationFrame(() => { charts["weight-chart"].resize(); updateDayBands("weight-chart"); });
}

// ===========================================================================
// "Környezet" chart — legördülőből EGY szenzor idősora (config.env.sensors).
// A szenzorok a saját RTDB-útjukból (byPath) jönnek. Az eső IS itt van (chip:false
// → csak plotolható, felső chip nélkül); a felső sávban a 24h csapadék-csempe áll.
// ===========================================================================
function renderEnvChart(hive, st) {
  const sensors = hive.env?.sensors || [];
  const sel = $("env-select");
  sel.innerHTML = "";
  sensors.forEach((s, i) => {
    const o = document.createElement("option");
    o.value = String(i);
    o.textContent = `${s.icon || ""} ${s.label}`.trim();
    sel.appendChild(o);
  });
  if (envSelIdx >= sensors.length) envSelIdx = 0;
  sel.value = String(envSelIdx);
  sel.disabled = sensors.length === 0;
  sel.onchange = () => { envSelIdx = +sel.value; drawEnvSeries(hive, st); };
  drawEnvSeries(hive, st);
}
function drawEnvSeries(hive, st) {
  const sensors = hive.env?.sensors || [];
  const s = sensors[envSelIdx];
  const chart = charts["env-chart"];
  if (!s) { chart.clear(); return; }
  const scale = s.scale || 1;
  const rows = st.byPath[s.path] || [];
  const raw = rows.filter((r) => typeof r[s.field] === "number").map((r) => [r.ts * 1000, +(r[s.field] * scale).toFixed(3)]);
  const fmt = s.fmt ?? 1;
  // Lépcsős szenzor (pl. eső, config step:true): NEM interpoláljuk (a densify rámpa
  // félrevezető lenne — folyamatos esőnek tűnne), és step-vonalat rajzolunk, ami az
  // adott időszakra tartja a delta-értéket. Folytonos szenzor: densify + enyhe simítás.
  const data = s.step ? raw : densify(raw, DENSIFY_MS, DENSIFY_MAX);
  const opts = s.step ? { step: "end" } : { smooth: LINE_SMOOTH };
  const series = [lineSeries(`${s.label} (${s.unit})`, data, PALETTE[1], 1.8, opts)];
  chart.setOption(chartOption(series, s.unit, null, (v) => v.toFixed(fmt) + " " + s.unit), true);
  requestAnimationFrame(() => { chart.resize(); updateDayBands("env-chart"); });
}

// ===========================================================================
// Idő-tengely felirat: óra:perc tick + KÖZÉPSŐ dátum-felirat
// ===========================================================================
const p2 = (n) => String(n).padStart(2, "0");
const MONTHS_HU = ["jan", "feb", "márc", "ápr", "máj", "jún", "júl", "aug", "szept", "okt", "nov", "dec"];
// A tick-label mindig csak óra:perc (a value ms-timestamp a time-tengelyről).
function hmLabel(val) { const d = new Date(val); return p2(d.getHours()) + ":" + p2(d.getMinutes()); }
// Helyi éjfél az adott ms-hez (a nap kezdete).
function dayStart(ms) { const d = new Date(ms); d.setHours(0, 0, 0, 0); return d.getTime(); }
// Egy nap dátum-felirata a rendelkezésre álló pixel-szélesség szerint:
// bő helyen teljes ("2026. júl 13"), szűkebben rövid ("júl 13").
function dayLabelFor(dayMs, availPx) {
  const d = new Date(dayMs);
  const short = `${MONTHS_HU[d.getMonth()]} ${d.getDate()}`;
  return availPx >= 120 ? `${d.getFullYear()}. ${short}` : short;
}
// A jelenleg LÁTHATÓ idő-tengely tartománya ms-ben (a szűrt/zoomolt adathoz
// igazodó tengely-extent). Belső ECharts model-API; hibatűrően csomagolva.
function currentXExtent(chart) {
  try {
    const ax = chart.getModel().getComponent("xAxis", 0).axis;
    const ext = ax.scale.getExtent();
    return (Array.isArray(ext) && ext.length === 2) ? ext : null;
  } catch (e) { return null; }
}
// Nap-sáv ("date band"): a látható tartomány MINDEN napja alá a saját dátuma,
// a nap szakaszának KÖZEPÉRE igazítva (graphic text-ek, a tengely alatt). A napok
// pixel-pozícióját a látható extentből + convertToPixel-ből számoljuk, ezért
// minden zoom/pan/resize után újra kell hívni. Zsúfolás-kezelés: ha a legkeskenyebb
// napszakasz szűkebb a min. feliratnál (MIN px), ritkítunk (minden step-edik nap),
// és a kiírt feliratnak a step-nyi hely jut → bő tartományon is olvasható marad.
// A hónapváltás magától látszik (minden felirat hordozza a hónapot). replaceMerge
// → a régi graphic-ok eltűnnek, és NEM triggerel új dataZoom-ot.
function updateDayBands(chartId) {
  const chart = charts[chartId];
  if (!chart) return;
  const clear = () => chart.setOption({ graphic: [] }, { replaceMerge: ["graphic"] });
  const ext = currentXExtent(chart);
  if (!ext || !isFinite(ext[0]) || !isFinite(ext[1]) || ext[1] <= ext[0]) { clear(); return; }
  const [a, b] = ext;
  const yDate = chart.getHeight() - 12;
  const px = (ms) => chart.convertToPixel({ xAxisIndex: 0 }, ms);
  const days = [];
  for (let t = dayStart(a); t < b; ) {
    const nx = new Date(t); nx.setDate(nx.getDate() + 1); const tn = nx.getTime(); // DST-helyes lépés
    const s = Math.max(t, a), e = Math.min(tn, b);
    days.push({ dayMs: t, mid: (px(s) + px(e)) / 2, segW: px(e) - px(s) });
    t = tn;
  }
  if (!days.length) { clear(); return; }
  const MIN = 50; // egy dátum-felirat minimális helyigénye px-ben
  const minSeg = Math.min(...days.map((d) => d.segW));
  const step = minSeg >= MIN ? 1 : Math.ceil(MIN / Math.max(minSeg, 1));
  const els = [];
  days.forEach((d, i) => {
    if (i % step !== 0) return;
    els.push({
      type: "text", x: d.mid, y: yDate, silent: true,
      style: { text: dayLabelFor(d.dayMs, d.segW * step), fill: MUT,
               font: "600 12px sans-serif", align: "center", verticalAlign: "middle" },
    });
  });
  chart.setOption({ graphic: els }, { replaceMerge: ["graphic"] });
}

// ===========================================================================
// ECharts közös építők
// ===========================================================================
// opts.smooth: 0..1 spline-simítás (folytonos idősorokra); opts.step: "start"|
// "middle"|"end" lépcsős vonal (pl. eső — a simítás ilyenkor kikapcsol).
function lineSeries(name, data, color, width, opts = {}) {
  const s = {
    // showSymbol: minden pont látszik (a valós mérések ÉS a köztük beszúrt
    // interpoláltak) → van mire ránagyítani. Nincs lttb-sampling, ami eldobná őket.
    name, type: "line", data, showSymbol: true, symbol: "circle", symbolSize: 3.5,
    smooth: opts.smooth ?? false, connectNulls: opts.connectNulls ?? true,
    lineStyle: { color, width }, itemStyle: { color },
    emphasis: { focus: "series" },
  };
  if (opts.step) { s.step = opts.step; s.smooth = false; } // lépcsős: nincs simítás
  return s;
}
function chartOption(series, yName, selected, valFmt) {
  return {
    animation: false, backgroundColor: "transparent",
    color: PALETTE,
    grid: { top: 40, left: 8, right: 14, bottom: 46, containLabel: true },
    legend: { show: series.length > 1, top: 4, textStyle: { color: TXT }, icon: "roundRect", selected: selected || undefined },
    // Nincs tooltip: érintésre NEM írunk ki tizedes értéket.
    tooltip: { show: false },
    // Idő-tengely: a tick-ek MINDIG csak óra:perc (nincs vegyes nap-hónap tick).
    // A dátumok külön "nap-sávként" a tengely ALATT: minden nap alá a saját dátuma,
    // a nap szakaszának közepére igazítva (l. updateDayBands, graphic text-ek).
    xAxis: {
      type: "time", axisLine: { lineStyle: { color: GRID } },
      axisLabel: { color: MUT, hideOverlap: true, formatter: hmLabel },
      splitLine: { show: true, lineStyle: { color: GRID } },
      axisPointer: { show: false },
    },
    yAxis: {
      type: "value", scale: true, name: yName, nameTextStyle: { color: MUT },
      axisLabel: { color: MUT }, splitLine: { lineStyle: { color: GRID } },
      axisPointer: { show: false },
    },
    // Navigáció: az IDŐ (X) tengelyt nagyítjuk/toljuk (kétujjas pinch = zoom,
    // egyujjas húzás = pan). A függőleges (Y) NEM szabad-zoomolható: mindig a
    // LÁTHATÓ adathoz igazodik (filterMode:'filter' + yAxis scale:true), így a
    // vonal soha nem csúszik ki a képből — nem lehet "üres semmibe" nagyítani.
    // NINCS alsó csúszka. Feltétel: belső canvas touch-action:none (CSS).
    dataZoom: [
      { type: "inside", xAxisIndex: 0, filterMode: "filter",
        zoomOnMouseWheel: true, moveOnMouseMove: true, moveOnMouseWheel: false, preventDefaultMouseMove: true },
    ],
    series,
  };
}

// Gesztus-szétválasztás mobilon:
//  - egyujjas FÜGGŐLEGES húzás  → OLDALGÖRGETÉS (a böngésző végzi, ne plot-pan),
//  - egyujjas VÍZSZINTES húzás → idő-pan (ECharts),
//  - kétujjas pinch            → idő-zoom (ECharts).
// Hogyan:
//  1) touch-action:pan-y a konténeren + a belső canvas/div-en → a böngésző a
//     függőleges görgetést natívan végezheti, a vízszintes/kétujjas gesztust JS
//     kapja (a JS a CSS !important-tal is összhangban pan-y-t állít).
//  2) capture-fázisú touch-figyelő eldönti az irányt: dominánsan függőleges
//     gesztusnál stopPropagation → az ECharts NEM kap touchmove-ot, tehát nem
//     panol, a böngésző pedig görget; vízszintes/kétujjas → átengedjük.
//  3) iOS Safari 'gesture*' eseményeinek letiltása (az oldal-zoomot ezek lövik;
//     az ECharts touchstart/move-ot használ, ezt nem érinti).
function hardenTouch(chartId) {
  const el = $(chartId);
  if (!el) return;
  el.style.touchAction = "pan-y";
  el.querySelectorAll("canvas, div").forEach((n) => { n.style.touchAction = "pan-y"; });
  ["gesturestart", "gesturechange", "gestureend"].forEach((ev) =>
    el.addEventListener(ev, (e) => e.preventDefault(), { passive: false }));

  const DEADZONE = 8; // px — eddig nem dől el az irány (holtsáv)
  let sx = 0, sy = 0, mode = "idle"; // idle | scroll | pan | pinch
  el.addEventListener("touchstart", (e) => {
    if (e.touches.length >= 2) { mode = "pinch"; return; } // kétujjas → ECharts zoom
    mode = "idle";
    sx = e.touches[0].clientX; sy = e.touches[0].clientY;
  }, { capture: true, passive: true });
  el.addEventListener("touchmove", (e) => {
    if (e.touches.length >= 2) { mode = "pinch"; return; }
    if (mode === "pinch") return;
    if (mode === "idle") {
      const dx = Math.abs(e.touches[0].clientX - sx);
      const dy = Math.abs(e.touches[0].clientY - sy);
      if (dx < DEADZONE && dy < DEADZONE) return; // még nem dől el
      mode = dy > dx ? "scroll" : "pan";
    }
    // FÜGGŐLEGES: az ECharts NE lássa a mozgást (ne panoljon) → a böngésző görget.
    // VÍZSZINTES: átengedjük az ECharts-nak (idő-pan).
    if (mode === "scroll") e.stopPropagation();
  }, { capture: true, passive: true });
}

function resetZoom(chartId) {
  const c = charts[chartId];
  if (c) c.dispatchAction({ type: "dataZoom", start: 0, end: 100 });
}

// Gyors időtáv-választó. A chartban ténylegesen jelen levő adatok tartományából
// számolunk, ezért az ablak vége mindig az utolsó adatpont (nem a falióra).
// Rövidebb történetnél a számított start 0%, vagyis a teljes tartomány látszik.
function setQuickRange(chartId, days) {
  const chart = charts[chartId];
  if (!chart || !isFinite(days) || days <= 0) return;
  const timestamps = [];
  for (const series of chart.getOption().series || []) {
    for (const point of series.data || []) {
      const value = Array.isArray(point) ? point[0] : Array.isArray(point?.value) ? point.value[0] : null;
      const ts = value instanceof Date ? value.getTime() : Number(value);
      if (isFinite(ts)) timestamps.push(ts);
    }
  }
  if (timestamps.length < 2) { resetZoom(chartId); return; }
  const first = Math.min(...timestamps), last = Math.max(...timestamps);
  const availableMs = last - first;
  const requestedMs = days * 24 * 60 * 60 * 1000;
  const start = availableMs > 0 ? Math.max(0, (availableMs - requestedMs) / availableMs * 100) : 0;
  chart.dispatchAction({ type: "dataZoom", start, end: 100 });
}

// ===========================================================================
// Környezet-sáv — 24h csapadék-csempe + szenzorok pillanatnyi értékei
// ===========================================================================
function lastNumeric(rows, field) {
  for (let i = rows.length - 1; i >= 0; i--) if (typeof rows[i][field] === "number") return rows[i][field];
  return null;
}
function weatherChip(icon, txt, unit, label) {
  const chip = document.createElement("div");
  chip.className = "wchip";
  chip.innerHTML = `<span class="wi">${icon || ""}</span>
    <span class="wtxt"><span class="wv">${txt}<small> ${unit}</small></span>
    <span class="wl">${label}</span></span>`;
  return chip;
}
function renderWeather(hive, st) {
  const el = $("weather");
  el.innerHTML = "";
  // Aktuális össz-súly csempe (a felső sor elején): a LEGUTÓBBI kör VALÓS összege
  // (l. totalKgSeries) — nem interpolált, a plot alatti utolsó valós total-pont.
  const total = totalKgSeries(hive.boards.map((b) => boardWeightKg(hive, b, st.boards[b.id])));
  const lastTotal = total.length ? total[total.length - 1][1] : null;
  el.appendChild(weatherChip("⚖️", lastTotal != null ? lastTotal.toFixed(2) : "–", "kg", "Össz-súly (utolsó mérés)"));
  const env = hive.env;
  if (!env) return;
  // 24h csapadék-összeg csempe: a rain_mm INTERVALLUM-DELTÁK összege az ablakban.
  if (env.rainTile) {
    const rt = env.rainTile;
    const rows = st.byPath[rt.path] || [];
    const cutoff = Date.now() - (rt.windowH || 24) * 3600 * 1000;
    let sum = 0, any = false;
    for (const r of rows) {
      if (typeof r[rt.field] === "number" && r.ts * 1000 >= cutoff) { sum += r[rt.field]; any = true; }
    }
    el.appendChild(weatherChip(rt.icon, any ? sum.toFixed(rt.fmt ?? 1) : "–", rt.unit, rt.label));
  }
  // Szenzorok pillanatnyi (LEGUTOLSÓ érvényes) értéke chipként.
  // chip:false → a szenzor csak a legördülőben plotolható, de a felső sávban NEM
  // kap chipet (pl. eső: ott a 24h rainTile-csempe fedi le a státuszt).
  for (const s of env.sensors || []) {
    if (s.chip === false) continue;
    let v = lastNumeric(st.byPath[s.path] || [], s.field);
    let txt = "–";
    if (v != null) { if (s.scale) v *= s.scale; txt = v.toFixed(s.fmt ?? 1); }
    el.appendChild(weatherChip(s.icon, txt, s.unit, s.label));
  }
}

// ===========================================================================
// Kompakt állapot-kártyák (boardonként: súly-kg + saját cella-hő)
// ===========================================================================
function renderCards(hive, st) {
  const el = $("cards");
  el.innerHTML = "";
  for (const b of hive.boards) {
    const kg = boardWeightKg(hive, b, st.boards[b.id]);
    el.appendChild(buildCard(hive, b, st.boards[b.id], kg));
  }
}
function buildCard(hive, board, rawRows, kg) {
  const card = document.createElement("div");
  card.className = "card";
  const lastRaw = rawRows.filter((r) => r.w !== null).slice(-1)[0] || null;
  const lastKg = kg.length ? kg[kg.length - 1][1] : null;

  const badges = [freshnessBadge(lastRaw)];
  if (board.power === "mains") badges.push({ txt: "⚡ hálózat", cls: "" });
  else badges.push(batteryBadge(board, lastRaw));
  const errB = errorBadge(board, lastRaw);
  if (errB) badges.push(errB);
  const badgeHtml = badges.map((b) => `<span class="badge ${b.cls}">${b.txt}</span>`).join("");

  const valTxt = lastKg == null ? "–" : (hive.weightEstimated ? "~" : "") + lastKg.toFixed(hive.weightEstimated ? 1 : 2);

  card.innerHTML = `
    <div class="card-head">
      <span class="card-name">${board.label}</span>
      <span class="card-badges">${badgeHtml}</span>
    </div>
    <div class="card-value"><span class="v">${valTxt}</span><span class="u">kg</span></div>
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
function batteryBadge(board, last) {
  if (board.noVbatHw) return { txt: "🔋 nincs HW", cls: "" };
  const mv = last && typeof last.vbat === "number" ? last.vbat : null;
  if (mv == null || mv === 0) return { txt: "🔋 –", cls: "" };
  const pct = Math.max(0, Math.min(100, Math.round((mv - 3300) / (4200 - 3300) * 100)));
  const cls = pct < 15 ? "danger" : pct < 35 ? "warn" : "ok";
  return { txt: `🔋 ${(mv / 1000).toFixed(2)}V`, cls };
}
function errorBadge(board, last) {
  if (!last || typeof last.mask !== "number" || last.mask === 0) return null;
  let mask = last.mask;
  if (board.noVbatHw) mask &= ~(1 << 6); // node1: a VBAT-bit várt, nem hiba
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

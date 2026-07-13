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
//    master=lineáris hőkorrekció), majd a 4 board interpolált ÖSSZEGE. Ha egy
//    board calib-ja hiányos, arra placeholderKg-fallback (median-alap) van.
//  Az async pontokat közös idővonalra lineárisan interpoláljuk, és csak ott
//  összegzünk, ahol MINDEN board lefedi az időt (nincs ugrás).
//
// MOBIL: az IDŐ-tengelyt nagyítod/tolod (kétujjas pinch = zoom, egyujjas húzás
// = pan); a függőleges mindig a látható adathoz igazodik → a vonal nem csúszik
// ki a képből. NINCS alsó csúszka, NINCS érték-kiíró tooltip.
// KRITIKUS: a belső <canvas> touch-action:none (CSS), különben a böngésző
// (iOS Safari) az oldalt zoomolja pinchre.

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
// Interpolációs pont-sűrítés: a ritka mérések közé ennyi lépésenként szúrunk be
// lineárisan interpolált pontot (hogy legyen mire ránagyítani); gap-enként max ennyi
// (nagy adatkimaradást nem tömünk tele pontokkal).
const DENSIFY_MS = 5 * 60 * 1000, DENSIFY_MAX = 6;

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
// egy board súly-sorozata kg-ban [[tsMs, kg]]. A cella-hő MINDEN boardnál a saját
// rekordból (p.it) jön — a master temp_c-je is a board-rekordban van már.
function boardWeightKg(hive, board, rows) {
  const r = rows.filter((p) => p.w !== null);
  if (hive.weightUnit === "kg") return r.map((p) => [p.ts * 1000, p.w]);
  const c = board.calib;
  const calibReady = c && c.zero_count != null && c.count_per_kg != null && c.refTemp != null;
  if (!calibReady) {
    // Súly-kalibráció még nincs kész → placeholder-fallback (medián-alapú).
    const med = median(r.map((p) => p.w));
    const { base, kgPerCount } = board.placeholderKg || { base: 0, kgPerCount: 1 };
    return r.map((p) => [p.ts * 1000, +(base + (p.w - med) * kgPerCount).toFixed(3)]);
  }
  // Valódi kalibrált kg: kg = (raw - zero_count - tempCorr) / count_per_kg.
  return r.map((p) => {
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
// Össz-súly = KÖRÖNKÉNTI VALÓS összeg. A cellák ~egyszerre (a felhasználó ±2-3 percen
// belül indítja őket) töltenek fel; ezeket az "együtt érkezett" mintákat KÖRÖKBE
// csoportosítjuk, és körönként a board-ok VALÓS (mért) értékeit adjuk össze — így a
// kiadott total tényleg mért érték, NEM interpolált kompozit, és egy körből SEMMI súly
// nem vész el. Total-pont csak akkor születik, ha a körben MIND a board képviselve van
// (különben a kör kimarad; a PLOT interpolál át rajta — az csak vizuális, l. densify).
// Egy kör = az első mintájától ROUND_WINDOW_MS-en belüli minták; körönként boardonként
// egy érték (a körbe eső utolsó). A kör időbélyege a benne levő minták átlaga.
const ROUND_WINDOW_MS = 8 * 60 * 1000; // > a ~6 perc körön belüli szórás, < a ~14 perc körök-közti rés
function totalKgSeries(perBoard) {
  const n = perBoard.length;
  if (!n || perBoard.some((s) => !s.length)) return [];
  const all = []; // [tsMs, boardIndex, kg] minden mintára, időrendben
  perBoard.forEach((s, bi) => s.forEach((p) => all.push([p[0], bi, p[1]])));
  all.sort((a, b) => a[0] - b[0]);
  const out = [];
  for (let i = 0; i < all.length; ) {
    const round = new Map(); // boardIndex -> kg (körön belül az utolsó)
    let tsum = 0, tcnt = 0, j = i;
    while (j < all.length && all[j][0] - all[i][0] <= ROUND_WINDOW_MS) {
      round.set(all[j][1], all[j][2]);
      tsum += all[j][0]; tcnt++; j++;
    }
    if (round.size === n) {
      let sum = 0;
      for (const v of round.values()) sum += v;
      out.push([Math.round(tsum / tcnt), +sum.toFixed(3)]);
    }
    i = j;
  }
  return out;
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
  // A total-pontok VALÓS, körönként összegzett mértékek (l. totalKgSeries); a PLOT
  // köztük szabadon interpolál (densify + sima vonal) — ez csak vizuális simítás, az
  // értékek maradnak valósak.
  const total = densify(totalKgSeries(perBoard), DENSIFY_MS, DENSIFY_MAX);
  const series = [lineSeries("Össz-súly", total, PALETTE[0], 2.6)];
  charts["weight-chart"].setOption(chartOption(series, "kg", null, (v) => v.toFixed(2) + " kg"), true);
  requestAnimationFrame(() => charts["weight-chart"].resize());
}

// ===========================================================================
// "Környezet" chart — legördülőből EGY szenzor idősora (config.env.sensors).
// A szenzorok a saját RTDB-útjukból (byPath) jönnek; az eső NINCS itt (l. csempe).
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
  const series = [lineSeries(`${s.label} (${s.unit})`, densify(raw, DENSIFY_MS, DENSIFY_MAX), PALETTE[1], 1.8)];
  chart.setOption(chartOption(series, s.unit, null, (v) => v.toFixed(fmt) + " " + s.unit), true);
  requestAnimationFrame(() => chart.resize());
}

// ===========================================================================
// ECharts közös építők
// ===========================================================================
function lineSeries(name, data, color, width) {
  return {
    // showSymbol: minden pont látszik (a valós mérések ÉS a köztük beszúrt
    // interpoláltak) → van mire ránagyítani. Nincs lttb-sampling, ami eldobná őket.
    name, type: "line", data, showSymbol: true, symbol: "circle", symbolSize: 3.5,
    smooth: false, connectNulls: true,
    lineStyle: { color, width }, itemStyle: { color },
    emphasis: { focus: "series" },
  };
}
function chartOption(series, yName, selected, valFmt) {
  return {
    animation: false, backgroundColor: "transparent",
    color: PALETTE,
    grid: { top: 40, left: 8, right: 14, bottom: 24, containLabel: true },
    legend: { show: series.length > 1, top: 4, textStyle: { color: TXT }, icon: "roundRect", selected: selected || undefined },
    // Nincs tooltip: érintésre NEM írunk ki tizedes értéket.
    tooltip: { show: false },
    xAxis: {
      type: "time", axisLine: { lineStyle: { color: GRID } },
      axisLabel: { color: MUT, hideOverlap: true }, splitLine: { show: true, lineStyle: { color: GRID } },
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

// A kétujjas pinch-zoom feltétele, hogy a böngésző NE vegye el a gesztust:
//  1) touch-action:none a konténeren + a belső canvas/div-en (JS, biztos, ami CSS
//     mellett is győz, ha az ECharts inline stílust tenne rá),
//  2) iOS Safari 'gesture*' eseményeinek letiltása (az oldal-zoomot ezek lövik;
//     az ECharts touchstart/move-ot használ, ezt nem érinti).
function hardenTouch(chartId) {
  const el = $(chartId);
  if (!el) return;
  el.style.touchAction = "none";
  el.querySelectorAll("canvas, div").forEach((n) => { n.style.touchAction = "none"; });
  ["gesturestart", "gesturechange", "gestureend"].forEach((ev) =>
    el.addEventListener(ev, (e) => e.preventDefault(), { passive: false }));
}

function resetZoom(chartId) {
  const c = charts[chartId];
  if (c) c.dispatchAction({ type: "dataZoom", start: 0, end: 100 });
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
  for (const s of env.sensors || []) {
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
  const firstKg = kg.length ? kg[0][1] : null;

  const badges = [freshnessBadge(lastRaw)];
  if (board.power === "mains") badges.push({ txt: "⚡ hálózat", cls: "" });
  else badges.push(batteryBadge(board, lastRaw));
  const errB = errorBadge(board, lastRaw);
  if (errB) badges.push(errB);
  const badgeHtml = badges.map((b) => `<span class="badge ${b.cls}">${b.txt}</span>`).join("");

  const valTxt = lastKg == null ? "–" : (hive.weightEstimated ? "~" : "") + lastKg.toFixed(hive.weightEstimated ? 1 : 2);
  const d = (lastKg != null && firstKg != null) ? lastKg - firstKg : null;
  const dCls = d == null ? "flat" : Math.abs(d) < 0.05 ? "flat" : d > 0 ? "up" : "down";
  const dTxt = d == null ? "&nbsp;" : `${dCls === "flat" ? "→" : d > 0 ? "↗" : "↘"} ${d > 0 ? "+" : ""}${d.toFixed(1)} kg <span class="muted">az ablakban</span>`;

  card.innerHTML = `
    <div class="card-head">
      <span class="card-name">${board.label}</span>
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

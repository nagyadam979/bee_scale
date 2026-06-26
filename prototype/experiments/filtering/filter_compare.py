#!/usr/bin/env python3
"""Szűrő-összehasonlító a méhkaptár össz-súly idősorra (w_total).

Cél: a hardver-eredetű, 1-2 mintás KIUGRÓ tüskék (±pár kg) levágása a valós
tömegtrend megőrzése mellett. Több szűrőcsaládot futtat ugyanazon a jelen,
összeveti statisztikával + ábrával, és kimenti az eredményt.

Szűrők:
  raw            – nyers, referencia
  ma_k           – mozgóátlag (centrált), MUTATJA miért rossz: elkeni a tüskét
  median_k       – gördülő medián (k=5): olcsó, 1-mintás tüskére kiváló
  hampel         – gördülő medián + MAD-küszöb: tüskét mediánra cseréli, trendet hagyja
  diff_clip      – sebesség-küszöb: ha |Δw| > thr, az ugrást levágja/interpolálja
  diff_mad       – sebesség-küszöb ADAPTÍV határral (a Δ-k MAD-jából)
  spike_io       – "ki-vissza" detektor: csak az izolált oda-vissza tüskét pótolja
  hampel_ma      – hampel + enyhe utósimítás (ma_k=3)

Használat:  python3 filter_compare.py [csv_path]
Kimenet:    filter_compare.png, filter_zoom.png, bee_weights_filtered.csv
"""
import sys, csv
from datetime import datetime, timezone, timedelta
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---------------------------------------------------------------- paraméterek
MA_K       = 5      # mozgóátlag ablak (minta)
MED_K      = 5      # medián ablak (páratlan)
HAMPEL_WIN = 7      # hampel teljes ablak (páratlan), ±3 minta
HAMPEL_NS  = 3.0    # hampel küszöb [MAD-szigma]
DIFF_THR   = 0.40   # fix sebesség-küszöb [kg/minta]  (~normál zaj << 0.1)
DIFF_NS    = 5.0    # adaptív sebesség-küszöb [MAD-szigma a Δ-kon]
SPIKE_THR  = 0.40   # ki-vissza tüske amplitúdó-küszöb [kg]
CEST       = timezone(timedelta(hours=2))

CSV_IN  = sys.argv[1] if len(sys.argv) > 1 else \
    "/Users/adamnagy/Documents/bee_scale/experiments/filtering/bee_weights.csv"
OUTDIR  = "/Users/adamnagy/Documents/bee_scale/experiments/filtering"

# --------------------------------------------------------------- beolvasás
ts, w = [], []
with open(CSV_IN) as f:
    for r in csv.DictReader(f):
        ts.append(int(r["ts"])); w.append(float(r["w_total"]))
ts = np.asarray(ts); w = np.asarray(w, float)
n = len(w)
dt = np.array([datetime.fromtimestamp(int(t), CEST) for t in ts])

# --------------------------------------------------------------- szűrők
def moving_average(x, k):
    pad = k // 2
    xp = np.pad(x, pad, mode="edge")
    return np.convolve(xp, np.ones(k) / k, mode="valid")

def median_filter(x, k):
    pad = k // 2
    xp = np.pad(x, pad, mode="edge")
    return np.array([np.median(xp[i:i + k]) for i in range(len(x))])

def hampel(x, win, nsig):
    """Gördülő medián + MAD. Visszaad: szűrt jel, outlier-maszk."""
    k = win // 2
    y = x.copy(); mask = np.zeros(len(x), bool)
    for i in range(len(x)):
        lo, hi = max(0, i - k), min(len(x), i + k + 1)
        seg = x[lo:hi]
        med = np.median(seg)
        mad = 1.4826 * np.median(np.abs(seg - med))
        if mad > 0 and abs(x[i] - med) > nsig * mad:
            y[i] = med; mask[i] = True
    return y, mask

def diff_clip(x, thr):
    """Sebesség-limit: a küszöböt meghaladó lépést levágja, majd a kilógó
    pontokat lineárisan interpolálja a jó szomszédokból."""
    d = np.abs(np.diff(x, prepend=x[0]))
    bad = d > thr
    # egy izolált tüske két nagy Δ-t ad (be+ki); a teljes "rossz" pontot pótoljuk
    y = x.copy(); mask = bad.copy()
    good = np.where(~mask)[0]
    if len(good) >= 2:
        y[mask] = np.interp(np.where(mask)[0], good, x[good])
    return y, mask

def diff_mad(x, nsig):
    d = np.diff(x, prepend=x[0])
    mad = 1.4826 * np.median(np.abs(d - np.median(d)))
    thr = max(nsig * mad, 1e-6)
    return diff_clip(x, thr) + (thr,)

def spike_io(x, thr):
    """Izolált 'ki-vissza' tüske: a pont mindkét szomszédjától >thr-rel tér el
    AZONOS irányban -> mediánra cseréli. A valós szintlépcsőt békén hagyja."""
    y = x.copy(); mask = np.zeros(len(x), bool)
    for i in range(1, len(x) - 1):
        a, b = x[i] - x[i - 1], x[i] - x[i + 1]
        if abs(a) > thr and abs(b) > thr and np.sign(a) == np.sign(b):
            y[i] = np.median([x[i - 1], x[i], x[i + 1]]); mask[i] = True
    return y, mask

# --------------------------------------------------------------- futtatás
ma          = moving_average(w, MA_K)
med         = median_filter(w, MED_K)
hp, hp_m    = hampel(w, HAMPEL_WIN, HAMPEL_NS)
dc, dc_m    = diff_clip(w, DIFF_THR)
dm, dm_m, dm_thr = diff_mad(w, DIFF_NS)
sp, sp_m    = spike_io(w, SPIKE_THR)
hpma        = moving_average(hp, 3)

FILTERS = [
    ("raw",                       w,    None,  "0.5"),
    (f"ma_k{MA_K}",               ma,   None,  "tab:gray"),
    (f"median_k{MED_K}",          med,  None,  "tab:green"),
    (f"hampel(w{HAMPEL_WIN},{HAMPEL_NS}σ)", hp, hp_m, "tab:blue"),
    (f"diff_clip(>{DIFF_THR})",   dc,   dc_m,  "tab:red"),
    (f"diff_mad({DIFF_NS}σ→{dm_thr:.2f})", dm, dm_m, "tab:purple"),
    (f"spike_io(>{SPIKE_THR})",   sp,   sp_m,  "tab:orange"),
    ("hampel+ma3",                hpma, hp_m,  "tab:cyan"),
]

# --------------------------------------------------------------- statisztika
def roughness(y):           # maradék "tüskésség": a Δ-k 99.9 pctilis abszolútja
    d = np.abs(np.diff(y))
    return np.percentile(d, 99.9), d.max()

print(f"\nminták: {n} | tartomány: {dt[0]:%Y-%m-%d %H:%M} .. {dt[-1]:%Y-%m-%d %H:%M}")
print(f"{'szűrő':<26}{'módosított':>11}{'maxΔ':>9}{'Δp99.9':>9}{'min':>8}{'max':>8}")
for name, y, m, _ in FILTERS:
    p999, mx = roughness(y)
    nm = int(m.sum()) if m is not None else 0
    print(f"{name:<26}{nm:>11}{mx:>9.2f}{p999:>9.3f}{y.min():>8.2f}{y.max():>8.2f}")

# --------------------------------------------------------------- ábra 1: áttekintő
fig, axes = plt.subplots(len(FILTERS), 1, figsize=(15, 2.0 * len(FILTERS)),
                         sharex=True, constrained_layout=True)
for ax, (name, y, m, c) in zip(axes, FILTERS):
    ax.plot(dt, w, color="0.8", lw=0.6, label="raw", zorder=1)
    if name != "raw":
        ax.plot(dt, y, color=c, lw=0.9, label=name, zorder=2)
    if m is not None and m.any():
        ax.scatter(dt[m], w[m], s=14, color="red", zorder=3, label=f"flagged ({int(m.sum())})")
    ax.set_ylabel("kg"); ax.legend(loc="upper right", fontsize=8, ncol=3)
    ax.grid(alpha=0.25); ax.margins(x=0.005)
axes[0].set_title("Méhkaptár össz-súly — szűrők összehasonlítása (teljes idősor)")
fig.savefig(f"{OUTDIR}/filter_compare.png", dpi=110)
print(f"\nábra: {OUTDIR}/filter_compare.png")

# --------------------------------------------------------------- ábra 2: zoom a legnagyobb tüskére
spike_idx = int(np.argmax(np.abs(np.diff(w, prepend=w[0]))))
lo = max(0, spike_idx - 60); hi = min(n, spike_idx + 60)   # ±~5 óra (5 perc/minta)
fig2, ax = plt.subplots(figsize=(15, 7))
ax.plot(dt[lo:hi], w[lo:hi], color="0.7", lw=1.4, marker=".", ms=4, label="raw")
for name, y, m, c in FILTERS:
    if name in ("raw", f"ma_k{MA_K}"):
        continue
    ax.plot(dt[lo:hi], y[lo:hi], color=c, lw=1.1, label=name)
ax.axvline(dt[spike_idx], color="red", ls=":", alpha=0.5)
ax.set_title(f"Zoom a legnagyobb tüskére ({dt[spike_idx]:%Y-%m-%d %H:%M})")
ax.set_ylabel("kg"); ax.legend(fontsize=9, ncol=2); ax.grid(alpha=0.3)
fig2.tight_layout(); fig2.savefig(f"{OUTDIR}/filter_zoom.png", dpi=110)
print(f"zoom: {OUTDIR}/filter_zoom.png")

# --------------------------------------------------------------- szűrt CSV
out_csv = f"{OUTDIR}/bee_weights_filtered.csv"
with open(out_csv, "w", newline="") as f:
    wr = csv.writer(f)
    cols = [name for name, *_ in FILTERS]
    wr.writerow(["ts", "datetime_cest"] + cols)
    for i in range(n):
        wr.writerow([ts[i], dt[i].strftime("%Y-%m-%d %H:%M:%S")] +
                    [f"{y[i]:.4f}" for _, y, *_ in FILTERS])
print(f"szűrt CSV: {out_csv}")

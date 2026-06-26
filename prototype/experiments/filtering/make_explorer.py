#!/usr/bin/env python3
"""Interaktív, belenagyítható HTML-ábra a szűrt össz-súly idősorra.

A bee_weights_filtered.csv minden oszlopát (raw + szűrők) külön trace-ként
rakja egy Plotly-ábrára: tetszőleges zoom/pan, range-slider, legend-toggle,
hover. Önálló fájl, böngészőben megnyitható (Plotly CDN-ről töltődik).

Használat:  python3 make_explorer.py
Kimenet:    filter_explorer.html
"""
import csv, json

DIR = "/Users/adamnagy/Documents/bee_scale/experiments/filtering"
CSV = f"{DIR}/bee_weights_filtered.csv"
OUT = f"{DIR}/filter_explorer.html"

# default láthatóság: ne legyen zsúfolt — raw + hampel látszik, a többi legendonly
DEFAULT_VISIBLE = {"raw", "hampel"}
# színek (a static ábrával összhangban)
COLOR = {
    "raw": "#bbbbbb", "ma_k5": "#888888", "median_k5": "#2ca02c",
    "hampel": "#1f77b4", "diff_clip": "#d62728", "diff_mad": "#9467bd",
    "spike_io": "#ff7f0e", "hampel+ma3": "#17becf",
}
def base(name):  # "hampel(w7,3.0σ)" -> "hampel" a szín/visibility kulcshoz
    for k in COLOR:
        if name.startswith(k):
            return k
    return name

# --- beolvasás ----------------------------------------------------------
rows = list(csv.DictReader(open(CSV)))
cols = [c for c in rows[0].keys() if c not in ("ts", "datetime_cest")]
x = [r["datetime_cest"] for r in rows]

traces = []
for c in cols:
    b = base(c)
    vis = True if b in DEFAULT_VISIBLE else "legendonly"
    traces.append({
        "type": "scattergl", "mode": "lines", "name": c, "x": x,
        "y": [float(r[c]) for r in rows],
        "line": {"width": 1.6 if b == "hampel" else 1.0, "color": COLOR.get(b)},
        "visible": vis,
        "hovertemplate": "%{x|%m-%d %H:%M}<br>%{y:.3f} kg<extra>" + c + "</extra>",
    })

layout = {
    "title": {"text": "Méhkaptár össz-súly — interaktív szűrő-böngésző "
                      f"({x[0][:10]} … {x[-1][:10]})"},
    "xaxis": {"title": "idő (CEST)", "rangeslider": {"visible": True},
              "type": "date"},
    "yaxis": {"title": "kg"},
    "hovermode": "x unified",
    "legend": {"orientation": "h", "y": 1.06},
    "margin": {"t": 70, "r": 20, "b": 30, "l": 60},
    "template": "plotly_dark",
}
config = {"scrollZoom": True, "displaylogo": False,
          "modeBarButtonsToAdd": ["v1hovermode", "toggleSpikelines"],
          "responsive": True}

html = f"""<!doctype html><html><head><meta charset="utf-8">
<title>bee_scale — szűrő-böngésző</title>
<script src="https://cdn.plot.ly/plotly-2.35.2.min.js"></script>
<style>html,body{{margin:0;height:100%;background:#111}}#g{{width:100vw;height:100vh}}</style>
</head><body><div id="g"></div><script>
Plotly.newPlot("g", {json.dumps(traces)}, {json.dumps(layout)}, {json.dumps(config)});
</script></body></html>"""

open(OUT, "w").write(html)
print(f"kész: {OUT}  ({len(traces)} trace, {len(rows)} pont)")
print("nyisd meg böngészőben — scroll=zoom, húzás=box-zoom, dupla katt=reset, "
      "legend-katt=trace ki/be")

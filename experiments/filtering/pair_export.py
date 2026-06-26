#!/usr/bin/env python3
"""w1+w2 rúd-readings párosítása össz-súllyá (app.js logika tükre) -> CSV.

Párosítás: minden w1 mintához a hozzá időben legközelebbi w2 minta, ha az
|dt| <= TOL (180 s). Páratlan minta kimarad (nincs w_total). Sorrend: ts növekvő.
A w_total NINCS tárolva a DB-ben; a dashboard is így állítja elő.
"""
import json, csv, sys
from datetime import datetime, timezone, timedelta

BK = "/private/tmp/claude-503/-Users-adamnagy-Documents-bee-scale/e31a7eda-1b68-4af0-b695-42d7d6a5966e/scratchpad"
TOL = 180  # sec, == app.js PAIR_TOLERANCE_SEC
CEST = timezone(timedelta(hours=2))

def load(path):
    d = json.load(open(path))
    return sorted(((v["ts"], v["w"]) for v in d.values()), key=lambda x: x[0])

w1 = load(f"{BK}/raw_w1.json")
w2 = load(f"{BK}/raw_w2.json")

# kétmutatós legközelebbi-szomszéd párosítás (mindkettő ts-rendezett)
rows = []
j = 0
unpaired = 0
for ts1, val1 in w1:
    # léptesd j-t amíg w2[j+1] közelebb van ts1-hez
    while j + 1 < len(w2) and abs(w2[j + 1][0] - ts1) <= abs(w2[j][0] - ts1):
        j += 1
    if j < len(w2) and abs(w2[j][0] - ts1) <= TOL:
        ts2, val2 = w2[j]
        rows.append((ts1, val1, val2, round(val1 + val2, 4)))
    else:
        unpaired += 1

out = f"{BK}/bee_weights.csv"
with open(out, "w", newline="") as f:
    wr = csv.writer(f)
    wr.writerow(["ts", "datetime_cest", "w1", "w2", "w_total"])
    for ts, v1, v2, tot in rows:
        wr.writerow([ts, datetime.fromtimestamp(ts, CEST).strftime("%Y-%m-%d %H:%M:%S"), v1, v2, tot])

print(f"párosított sorok: {len(rows)}  | párosítatlan w1: {unpaired}")
print(f"tartomány: {rows[0][0]} .. {rows[-1][0]}  ({datetime.fromtimestamp(rows[0][0],CEST):%Y-%m-%d %H:%M} .. {datetime.fromtimestamp(rows[-1][0],CEST):%Y-%m-%d %H:%M})")
print(f"w_total min/max: {min(r[3] for r in rows)} / {max(r[3] for r in rows)}")
print(f"CSV: {out}")

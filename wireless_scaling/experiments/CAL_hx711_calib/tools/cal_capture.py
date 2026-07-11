#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cal_capture.py — a CAL_hx711_calib firmware PC-oldali kalibráló eszköze.

A node kalibrációs firmware (experiments/CAL_hx711_calib) NYERS adatot ad soros porton;
ez a script gyűjti, illeszti, és cellánként egy közös `calibration.json`-be írja a felhő-
oldali konverzió paramétereit.

MODELL (cellánként, unit_id szerint):
    kg = (raw - zero_count - tempCoef*(scale_c - refTemp)) / count_per_kg
  - zero_count    : üres platform nyers count-ja (a "0" a fizikailag üres mérleg, nincs tare)
  - count_per_kg  : count↔kg meredekség (ismert súly-lépcsőkre illesztve)
  - tempCoef      : nulla-drift hő-együttható, count/°C (terheletlen hő-szkenre illesztve)
  - refTemp       : az a scale_c, amin a súly-kalibráció készült (a hő-tag itt ~0)

SOROS PROTOKOLL (a firmware ad ki; minden `#`-kezdetű és minden ESP-LOG sor eldobandó):
    WGT <raw_median> <scale_c|nan>          # súly-stream (a `w` parancs után)
    TLG <idx> <raw> <scale_c|nan>           # hő-log élő sor
    REC <idx> <raw> <scale_c|nan>           # dump rekord (a `d` parancs után), zárás: `# DUMP END n=..`
Parancsok (1 karakter): w=stream, x=stop, g=hő-log, d=dump, s=status, e+Y=erase.

ALPARANCSOK:
    weight  --port P --unit U   : élő súly-capture (interaktív kg-címkézés) -> <U>_weight.csv
    temp    --port P --unit U   : a hő-log dumpja (`d`) -> <U>_temp.csv
    fit     --unit U            : a két CSV illesztése -> calibration.json (adott unit frissítése)
    show                        : a calibration.json tartalmának kiírása

A fit-mag CSAK stdlib (saját OLS least-squares) — az m1b_analyze.py idiómáját követi.
A capture-höz pyserial kell:  .venv/bin/pip install pyserial
"""
from __future__ import annotations

import argparse
import csv
import datetime
import json
import math
import os
import statistics
import sys
import time

# A hő-együttható M1b-referenciája (cella+HX711, R²=0,974). Csak SANITY-CHECK: a valódi
# tempCoef cella-specifikus, cellánként mérjük. Ha az illesztett érték ettől nagyságrendben
# eltér, az bekötés-/cella-gyanú.
TEMPCOEF_REF = 1031.0
TEMPCOEF_REF_TOL = 500.0   # count/°C; ezen túl figyelmeztetünk

CAL_JSON_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "calibration.json")
DATA_DIR_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
CAL_MODEL_STR = "kg = (raw - zero_count - tempCoef*(scale_c - refTemp)) / count_per_kg"


# ============================ OLS least-squares (stdlib) ============================

def linfit(xs, ys):
    """y = a*x + b OLS-illesztés. Visszaad: (a, b, r2, n, rms_resid). n<2 -> None."""
    n = len(xs)
    if n < 2:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0.0:
        return None
    sxy = sum((xs[i] - mx) * (ys[i] - my) for i in range(n))
    a = sxy / sxx
    b = my - a * mx
    # R² és reziduál
    ss_tot = sum((y - my) ** 2 for y in ys)
    resid = [ys[i] - (a * xs[i] + b) for i in range(n)]
    ss_res = sum(r * r for r in resid)
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0
    rms = math.sqrt(ss_res / n)
    return {"a": a, "b": b, "r2": r2, "n": n, "rms_resid": rms,
            "max_abs_resid": max(abs(r) for r in resid)}


# ============================ Soros port (lazy pyserial) ============================

def open_serial(port, baud=115200, timeout=0.2):
    try:
        import serial  # noqa: WPS433 (lazy: csak a capture-hoz kell)
    except ImportError:
        sys.exit("HIBA: pyserial nincs telepítve. Telepítsd:\n"
                 "  .venv/bin/pip install pyserial\n"
                 "(az illesztéshez — `fit` — nem kell serial.)")
    ser = serial.Serial(port, baud, timeout=timeout)
    time.sleep(0.6)                 # boot/port-beállás
    ser.reset_input_buffer()
    return ser


def send_cmd(ser, ch):
    ser.write(ch.encode())
    ser.flush()


def read_lines(ser, duration_s):
    """`duration_s`-ig olvas, dekódolt sorokat ad vissza (rstrip)."""
    out = []
    buf = b""
    deadline = time.time() + duration_s
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                out.append(line.decode(errors="replace").rstrip("\r"))
    return out


def parse_data_line(line, tag):
    """`<tag> <a> <b> [<c>]` -> lista floatokból, vagy None. A `nan` NaN-ként jön."""
    parts = line.split()
    if not parts or parts[0] != tag:
        return None
    vals = []
    for p in parts[1:]:
        try:
            vals.append(float(p))
        except ValueError:
            return None
    return vals


# ============================ CSV I/O ============================

def unit_path(data_dir, unit, suffix):
    return os.path.join(data_dir, f"{unit}_{suffix}.csv")


def ensure_data_dir(data_dir):
    os.makedirs(data_dir, exist_ok=True)
    return data_dir


# ============================ weight capture ============================

def cmd_weight(args):
    ensure_data_dir(args.data_dir)
    path = unit_path(args.data_dir, args.unit, "weight")
    ser = open_serial(args.port)
    print(f"# Súly-capture: unit={args.unit}  port={args.port}")
    print("# Ismert súlyt teszel a mérlegre, beírod a tömeget kg-ban (üres platform = 0),")
    print(f"#  a script {args.settle}s-ig gyűjti a WGT count-okat -> medián + scale_c.")
    print("# Parancsok itt:  <kg> Enter = pont rögzítése,  'q' Enter = kész+mentés.")
    send_cmd(ser, "w")                      # súly-stream indítása a boardon
    time.sleep(0.5)
    ser.reset_input_buffer()

    points = []   # (kg, count_median, scale_c_mean, n)
    try:
        while True:
            # élő minta a prompt elé
            live = [parse_data_line(l, "WGT") for l in read_lines(ser, 0.8)]
            live = [v for v in live if v]
            if live:
                print(f"\r# élő: raw={live[-1][0]:.0f}  scale_c={live[-1][1]:.2f}   ", end="", flush=True)
            raw_in = input(f"\n[{args.unit}] kg (vagy q): ").strip()
            if raw_in.lower() == "q":
                break
            try:
                kg = float(raw_in.replace(",", "."))
            except ValueError:
                print("  ! nem szám, újra")
                continue
            ser.reset_input_buffer()
            samples = [parse_data_line(l, "WGT") for l in read_lines(ser, args.settle)]
            samples = [v for v in samples if v]
            counts = [v[0] for v in samples]
            temps = [v[1] for v in samples if not math.isnan(v[1])]
            if not counts:
                print("  ! nem jött WGT adat, a board streamel? (w) — kihagyva")
                continue
            cmed = statistics.median(counts)
            tmean = statistics.fmean(temps) if temps else float("nan")
            points.append((kg, cmed, tmean, len(counts)))
            print(f"  -> kg={kg:g}  count_median={cmed:.0f}  scale_c={tmean:.2f}  (n={len(counts)})")
    finally:
        send_cmd(ser, "x")
        ser.close()

    if not points:
        print("# nincs rögzített pont, nem mentek.")
        return
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kg", "count_median", "scale_c", "n"])
        w.writerows(points)
    print(f"# {len(points)} pont mentve: {os.path.normpath(path)}")


# ============================ temp dump ============================

def cmd_temp(args):
    ensure_data_dir(args.data_dir)
    path = unit_path(args.data_dir, args.unit, "temp")
    ser = open_serial(args.port)
    print(f"# Hő-log dump: unit={args.unit}  port={args.port}")
    ser.reset_input_buffer()
    send_cmd(ser, "d")
    recs = []
    buf = b""
    deadline = time.time() + args.timeout
    done = False
    while time.time() < deadline and not done:
        chunk = ser.read(512)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode(errors="replace").rstrip("\r")
            if s.startswith("# DUMP END"):
                done = True
                break
            v = parse_data_line(s, "REC")
            if v and len(v) >= 3:
                recs.append((int(v[0]), int(v[1]), v[2]))
    ser.close()
    if not done:
        print(f"# FIGYELEM: nem jött '# DUMP END' {args.timeout}s alatt (részleges dump).")
    if not recs:
        print("# nincs rekord (üres a log? `s`-sel ellenőrizd).")
        return
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["seq", "raw", "scale_c"])
        w.writerows(recs)
    print(f"# {len(recs)} rekord mentve: {os.path.normpath(path)}")


# ============================ fit ============================

def read_weight_csv(path):
    pts = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            pts.append((float(row["kg"]), float(row["count_median"]), float(row["scale_c"])))
    return pts


def read_temp_csv(path):
    pts = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            sc = float(row["scale_c"])
            if math.isnan(sc):
                continue
            pts.append((sc, float(row["raw"])))
    return pts


def cmd_fit(args):
    wpath = unit_path(args.data_dir, args.unit, "weight")
    tpath = unit_path(args.data_dir, args.unit, "temp")
    if not os.path.exists(wpath):
        sys.exit(f"HIBA: hiányzik a súly-CSV: {os.path.normpath(wpath)} (futtasd: weight)")

    wpts = read_weight_csv(wpath)
    kgs = [p[0] for p in wpts]
    counts = [p[1] for p in wpts]
    wtemps = [p[2] for p in wpts if not math.isnan(p[2])]
    wf = linfit(kgs, counts)          # count = count_per_kg*kg + zero_count
    if wf is None:
        sys.exit("HIBA: a súly-illesztéshez legalább 2, eltérő kg-jú pont kell.")
    count_per_kg = wf["a"]
    zero_count = wf["b"]
    ref_temp = statistics.fmean(wtemps) if wtemps else float("nan")

    unit_entry = {
        "zero_count": round(zero_count, 2),
        "count_per_kg": round(count_per_kg, 4),
        "tempCoef": None,
        "refTemp": round(ref_temp, 3) if not math.isnan(ref_temp) else None,
        "fit": {
            "weight": {
                "n": wf["n"], "r2": round(wf["r2"], 6),
                "rms_resid_count": round(wf["rms_resid"], 1),
                "rms_resid_kg": round(wf["rms_resid"] / abs(count_per_kg), 4) if count_per_kg else None,
                "kg_range": [min(kgs), max(kgs)],
            }
        },
        "calibrated_at": datetime.datetime.now().isoformat(timespec="seconds"),
    }

    print(f"# --- {args.unit} súly-illesztés ---")
    print(f"#  count_per_kg = {count_per_kg:.3f} count/kg")
    print(f"#  zero_count   = {zero_count:.1f} count (üres platform)")
    print(f"#  refTemp      = {ref_temp:.2f} °C" if not math.isnan(ref_temp) else "#  refTemp    = n/a (nincs scale_c)")
    print(f"#  R²={wf['r2']:.5f}  RMS-reziduál={wf['rms_resid']:.1f} count "
          f"(~{wf['rms_resid']/abs(count_per_kg):.3f} kg)  n={wf['n']}")

    # hő-illesztés (opcionális, ha van dump)
    if os.path.exists(tpath):
        tpts = read_temp_csv(tpath)
        tf = linfit([p[0] for p in tpts], [p[1] for p in tpts]) if len(tpts) >= 2 else None
        if tf is None:
            print("# hő-CSV megvan, de <2 érvényes pont — tempCoef kihagyva.")
        else:
            tempcoef = tf["a"]
            unit_entry["tempCoef"] = round(tempcoef, 3)
            span = [min(p[0] for p in tpts), max(p[0] for p in tpts)]
            off = abs(tempcoef - TEMPCOEF_REF)
            unit_entry["fit"]["temp"] = {
                "n": tf["n"], "r2": round(tf["r2"], 6),
                "scale_c_span": [round(span[0], 2), round(span[1], 2)],
                "ref_1031_ok": off <= TEMPCOEF_REF_TOL,
            }
            print(f"# --- {args.unit} hő-illesztés ---")
            print(f"#  tempCoef = {tempcoef:.1f} count/°C   R²={tf['r2']:.4f}  n={tf['n']}  "
                  f"span={span[0]:.1f}..{span[1]:.1f}°C")
            if off > TEMPCOEF_REF_TOL:
                print(f"#  ! FIGYELEM: tempCoef {off:.0f}-rel eltér az M1b-referencia {TEMPCOEF_REF:.0f}-től "
                      f"(>±{TEMPCOEF_REF_TOL:.0f}) — bekötés/cella gyanú.")
            if span[1] - span[0] < 10.0:
                print(f"#  ! FIGYELEM: a hő-span csak {span[1]-span[0]:.1f}°C — bizonytalan meredekség, "
                      f"tágabb hőpontok kellenek.")
    else:
        print(f"# nincs hő-CSV ({os.path.normpath(tpath)}) — tempCoef=None (futtasd: temp).")

    # calibration.json frissítése (a többi unit megmarad)
    data = load_cal_json(args.json)
    data["units"][args.unit] = unit_entry
    save_cal_json(args.json, data)
    print(f"# calibration.json frissítve ({args.unit}): {os.path.normpath(args.json)}")


# ============================ calibration.json ============================

def load_cal_json(path):
    if os.path.exists(path):
        with open(path) as f:
            data = json.load(f)
        data.setdefault("units", {})
        return data
    return {"generated_by": "cal_capture.py", "model": CAL_MODEL_STR, "units": {}}


def save_cal_json(path, data):
    data["model"] = CAL_MODEL_STR
    with open(path, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")


def cmd_show(args):
    data = load_cal_json(args.json)
    print(json.dumps(data, indent=2, ensure_ascii=False))


# ============================ önteszt ============================

def cmd_selftest(args):
    """A fit-mag ellenőrzése szintetikus adatokon (serial nélkül)."""
    # súly: count = 400*kg + 120000, kis zajjal
    kgs = [0, 10, 20, 40, 60]
    counts = [400.0 * k + 120000.0 for k in kgs]
    wf = linfit(kgs, counts)
    assert abs(wf["a"] - 400.0) < 1e-6, wf
    assert abs(wf["b"] - 120000.0) < 1e-6, wf
    assert wf["r2"] > 0.999999
    # hő: raw = 1031*scale_c + 90000
    scs = [5, 15, 25, 35, 45]
    raws = [1031.0 * s + 90000.0 for s in scs]
    tf = linfit(scs, raws)
    assert abs(tf["a"] - 1031.0) < 1e-6, tf
    # degenerált esetek
    assert linfit([1], [1]) is None
    assert linfit([2, 2], [3, 4]) is None
    print("# SELFTEST OK: linfit súly + hő + degenerált esetek rendben.")


# ============================ CLI ============================

def main():
    ap = argparse.ArgumentParser(description="CAL_hx711_calib PC-oldali kalibráló eszköz.")
    ap.add_argument("--json", default=os.path.normpath(CAL_JSON_DEFAULT),
                    help="calibration.json útvonala")
    ap.add_argument("--data-dir", default=os.path.normpath(DATA_DIR_DEFAULT),
                    help="a nyers CSV-k mappája (default: ../data)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    pw = sub.add_parser("weight", help="élő súly-capture (interaktív kg-címkézés)")
    pw.add_argument("--port", required=True, help="soros port (pl. /dev/cu.usbmodemXXXX)")
    pw.add_argument("--unit", required=True, help="cella azonosító (pl. node1)")
    pw.add_argument("--settle", type=float, default=4.0, help="pontonkénti gyűjtési idő (s)")
    pw.set_defaults(func=cmd_weight)

    pt = sub.add_parser("temp", help="hő-log dump lehúzása CSV-be")
    pt.add_argument("--port", required=True, help="soros port")
    pt.add_argument("--unit", required=True, help="cella azonosító")
    pt.add_argument("--timeout", type=float, default=30.0, help="dump-olvasás timeout (s)")
    pt.set_defaults(func=cmd_temp)

    pf = sub.add_parser("fit", help="illesztés a CSV-kből -> calibration.json")
    pf.add_argument("--unit", required=True, help="cella azonosító")
    pf.set_defaults(func=cmd_fit)

    ps = sub.add_parser("show", help="calibration.json kiírása")
    ps.set_defaults(func=cmd_show)

    px = sub.add_parser("selftest", help="a fit-mag öntesztje (serial nélkül)")
    px.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

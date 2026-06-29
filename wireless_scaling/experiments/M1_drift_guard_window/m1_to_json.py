#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M1 — a drift-elemzo CSV-jet (m1_drift_analyze.py --csv) strukturalt JSON-na alakitja,
hogy analitikai eszkozok (pandas, jq, BI) konnyen olvassak. NEM olvas nyers logot: a mar
strukturalt per-cycle CSV-bol dolgozik, es ujraszamolja az osszegzo metrikakat (linearis
fit host- es master-referencian, awake-korrekcio, ho-korrelacio, veszteseg-statisztika,
guard-window). A szamok kereszt-ellenorizhetok az m1_drift_analyze.py riportjaval.

Hasznalat:
  python3 m1_to_json.py m1_drift.csv [--nominal-period-s 30] [--out m1_drift.json] [--guard-n 10,60,1440]
"""
import sys, csv, json, argparse, math
from collections import Counter


def linfit(xs, ys):
    """Egyszeru OLS: y = a + b*x. Visszaad (b meredekseg, a tengelymetszet, R^2)."""
    n = len(xs)
    if n < 2:
        return float('nan'), float('nan'), float('nan')
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((xs[i] - mx) * (ys[i] - my) for i in range(n))
    if sxx == 0:
        return float('nan'), float('nan'), float('nan')
    b = sxy / sxx
    a = my - b * mx
    syy = sum((y - my) ** 2 for y in ys)
    ss_res = sum((ys[i] - (a + b * xs[i])) ** 2 for i in range(n))
    r2 = (1 - ss_res / syy) if syy > 0 else float('nan')
    return b, a, r2


def drift_block(cycles, times_s, period):
    """Linearis fit (ido vs cycle) -> mert periodus + drift metrikak."""
    b, a, r2 = linfit(cycles, times_s)          # b = s / cycle
    drift = (b - period) / period                # relativ drift
    resid = [times_s[i] - (a + b * cycles[i]) for i in range(len(cycles))]
    rstd = (sum(r * r for r in resid) / len(resid)) ** 0.5 if resid else float('nan')
    return {
        "T_meas_s_per_cycle": round(b, 6),
        "drift_ppm": round(drift * 1e6, 2),
        "drift_pct": round(drift * 100, 5),
        "drift_s_per_hour": round(drift * 3600, 4),
        "r2": round(r2, 8),
        "residual_std_s": round(rstd, 4),
    }, resid


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--nominal-period-s", type=float, default=30.0)
    ap.add_argument("--out", default=None)
    ap.add_argument("--guard-n", default="10,60,1440")
    args = ap.parse_args(argv)
    out = args.out or args.csv.rsplit('.', 1)[0] + ".json"
    period = args.nominal_period_s

    rows = list(csv.DictReader(open(args.csv)))
    samples = []
    for r in rows:
        die = r.get('die_c', '')
        try:
            die_v = float(die)
            if math.isnan(die_v):
                die_v = None
        except (ValueError, TypeError):
            die_v = None
        samples.append({
            "boot": r['boot'],
            "cycle": int(r['cycle']),
            "host_ts_iso": r['host_ts_iso'],
            "rx_us": int(r['rx_us']) if r['rx_us'] else None,
            "host_rel_s": float(r['host_rel_s']),
            "master_rel_s": float(r['master_rel_s']) if r.get('master_rel_s') else None,
            "residual_host_s": float(r['residual_host_s']) if r.get('residual_host_s') else None,
            "awake_ms": int(r['awake_ms']) if r['awake_ms'] else None,
            "die_c": die_v,
            "rssi": int(r['rssi']) if r['rssi'] else None,
        })

    cycles = [s['cycle'] for s in samples]
    host_t = [s['host_rel_s'] for s in samples]
    # master-ido relativ (lehet None ha hianyzik); ahol van, abbol fit
    mas_pairs = [(s['cycle'], s['master_rel_s']) for s in samples if s['master_rel_s'] is not None]
    awake_ms = [s['awake_ms'] or 0 for s in samples]

    host_drift, resid = drift_block(cycles, host_t, period)
    master_drift = None
    if len(mas_pairs) >= 2:
        mc = [p[0] for p in mas_pairs]
        mt = [p[1] for p in mas_pairs]
        master_drift, _ = drift_block(mc, mt, period)

    # awake-korrekcio: MINDEN ciklus (a vett ES a kimaradt is) hozzaad egy ~avg_awake ebren-idot
    # az eltelt idohoz, ezert a cycle0-tol szamitott ciklusszammal aranyosan vonjuk le
    # (avg_awake * (cycle - cycle0)). FONTOS: NEM a vett mintak kumulalt awake-jet (az a
    # csomagveszteseg mellett ~9x ALA-korrigalna) -> igy a fit a tiszta ALVAS-driftet adja.
    avg_awake = (sum(awake_ms) / len(awake_ms)) if awake_ms else 0.0
    c0 = cycles[0]
    awake_corr_t = [host_t[i] - (avg_awake / 1000.0) * (cycles[i] - c0) for i in range(len(cycles))]
    awake_drift, _ = drift_block(cycles, awake_corr_t, period)
    avg_awake = round(avg_awake, 1)

    # ho-korrelacio (best-effort): lokalis drift-rata kozelitese a residual-derivalttal vs die_c
    temps = [s['die_c'] for s in samples if s['die_c'] is not None]
    temp_block = {"characterizable": False}
    if len(temps) >= 5 and (max(temps) - min(temps)) >= 0.5:
        # residual valtozasa cycle-onkent (kozeliti az ido-fuggo drift-rata-elterest) vs die_c
        xs_t, ys_d = [], []
        for i in range(1, len(cycles)):
            dc = cycles[i] - cycles[i - 1]
            if dc > 0 and samples[i]['die_c'] is not None:
                xs_t.append(samples[i]['die_c'])
                ys_d.append((resid[i] - resid[i - 1]) / dc / period * 1e6)  # ppm
        if len(xs_t) >= 5:
            b, a, r2 = linfit(xs_t, ys_d)
            temp_block = {
                "characterizable": (r2 == r2 and r2 >= 0.3),  # csak ha erdemi korrelacio
                "coef_ppm_per_C": round(b, 2),
                "r2": round(r2, 4),
                "die_c_min": round(min(temps), 2),
                "die_c_max": round(max(temps), 2),
                "die_c_span": round(max(temps) - min(temps), 2),
                "n": len(xs_t),
                "note": "alacsony R^2 / kis homerseklet-tartomany -> a hofugges ebbol a futasbol NEM jellemezheto megbizhatoan",
            }

    # veszteseg-statisztika
    span = max(cycles) - min(cycles) + 1
    received = len(set(cycles))
    gaps = [cycles[i + 1] - cycles[i] for i in range(len(cycles) - 1)]
    missing_runs = [g - 1 for g in gaps if g > 1]  # hany egymas utani ciklus veszett egy resben
    blackouts = sorted([g - 1 for g in gaps if g - 1 >= 10], reverse=True)
    loss = {
        "cycle_min": min(cycles),
        "cycle_max": max(cycles),
        "expected_span": span,
        "received_unique": received,
        "missing": span - received,
        "loss_pct": round((span - received) / span * 100, 2),
        "max_consecutive_lost": max(missing_runs) if missing_runs else 0,
        "long_blackouts_cycles_ge10": blackouts,
        "gap_histogram": dict(sorted(Counter(gaps).items())),
        "note": "abszolut cycle# -> a veszteseg NEM rontja a drift-fitet (a regresszio a meglevo pontokra illeszt)",
    }

    rssi = [s['rssi'] for s in samples if s['rssi'] is not None]
    drift_for_guard = abs(host_drift['drift_pct']) / 100.0
    margin = 3 * host_drift['residual_std_s']
    guard = []
    for n in [int(x) for x in args.guard_n.split(',')]:
        guard.append({
            "N_cycles": n,
            "hours": round(n * period / 3600.0, 3),
            "guard_window_s": round(drift_for_guard * n * period + margin, 3),
        })

    result = {
        "meta": {
            "source_csv": args.csv,
            "nominal_period_s": period,
            "samples": len(samples),
            "first_host_ts": samples[0]['host_ts_iso'] if samples else None,
            "last_host_ts": samples[-1]['host_ts_iso'] if samples else None,
            "boot_ids": sorted(set(s['boot'] for s in samples)),
            "rssi_dbm": {"min": min(rssi), "max": max(rssi),
                          "avg": round(sum(rssi) / len(rssi), 1)} if rssi else None,
            "avg_awake_ms": avg_awake,
        },
        "drift": {
            "host_ref": host_drift,
            "master_ref": master_drift,
            "awake_corrected": awake_drift,
            "awake_contribution_ppm": round(host_drift['drift_ppm'] - awake_drift['drift_ppm'], 2),
        },
        "temperature": temp_block,
        "packet_loss": loss,
        "guard_window": {
            "formula": "guard_s(N) = |drift| * N * nominal_period_s + margin",
            "drift_used_pct": host_drift['drift_pct'],
            "margin_s": round(margin, 4),
            "margin_rule": "3 x residual_std (host)",
            "table": guard,
        },
        "samples": samples,
    }

    with open(out, 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    # rovid stdout-osszegzo (kereszt-ellenorzeshez)
    print("JSON kiirva:", out)
    print("  drift host =", host_drift['drift_pct'], "% (", host_drift['drift_ppm'], "ppm, ",
          host_drift['drift_s_per_hour'], "s/ora), R^2 =", host_drift['r2'])
    print("  awake-korrigalt =", awake_drift['drift_pct'], "% (", awake_drift['drift_ppm'], "ppm )")
    print("  master kereszt =", master_drift['drift_pct'] if master_drift else None, "%")
    print("  veszteseg =", loss['loss_pct'], "% (", loss['received_unique'], "/", span,
          "), max blackout =", loss['max_consecutive_lost'], "ciklus")
    print("  guard(N=1440) =", guard[-1]['guard_window_s'], "s")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

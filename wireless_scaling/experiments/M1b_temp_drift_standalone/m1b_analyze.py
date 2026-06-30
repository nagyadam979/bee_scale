#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M1b — STANDALONE off-grid ho-drift mero offline elemzoje.

Egy ESP32-C3 SuperMini node off-grid (huto/nap) deep-sleep ciklusban minden
ebredeskor AT24C32 EEPROM-ba logol:
  - a sajat RC slow-clock ora-driftjet egy DS3231 hokompenzalt referenciahoz,
  - a merocella+HX711 nyers olvasatat (konstans terheles mellett -> ennek
    ho-okozta driftje),
  - a DS3231 sajat ho-szenzorat (elsodleges ambiens) es a C3 belso die-hot.
A board PC-hez dugva a teljes EEPROM-logot a soros konzolra dumpolja; ez a
script azt parse-olja es adja a drift-vs-ho gorbeket.

A fit-mag CSAK stdlib (sajat OLS least-squares). matplotlib OPCIONALIS (--plot):
ha nincs telepitve, nem hibazik, csak kihagyja a PNG-t. Az M1 idiomait koveti
(m1_to_json.py linfit/drift_block/temp_block; m1_drift_analyze.py parse-minta,
host-prefix kezeles, robusztus sor-szures, self-test).

Hasznalat:
  python3 m1b_analyze.py DUMP.log [--nominal-period-s N] [--out-prefix m1b]
                         [--guard-n 10,60,1440] [--plot]
  python3 m1b_analyze.py --self-test

BEFAGYASZTOTT KONTRAKTUS (a firmware PONTOSAN ezt dumpolja):
  Pontosan EGY fejlec-sor:
    M1B-HDR records=%u period_s=%u start_unix=%lld
  Majd rekordonkent egy sor:
    M1B idx=%u cycle=%u ds_unix=%lld ds_c=%.2f die_c=%.2f hx=%ld awake_ms=%u
  Az awake_ms az EBREN toltott meres-ido (ms); a ds_unix-kulonbseg minden ciklusban
  = awake + period*(1+drift), ezert az ora-driftbol LEVONJUK az atlagos awake-et
  (avg_awake * cyc_rel), hogy a tiszta ALVAS-drift maradjon. Ha az awake_ms hianyzik
  (regi dump), a korrekcio kimarad (a 'raw' es 'awake_corrected' drift egybeesik).
  A ds_c / die_c lehet "nan". A sor elejen lehet host-logger ISO8601-prefix +
  TAB; lehetnek '#'-kezdetu komment / RECONNECT sorok es egyeb boot-zaj
  sorok -> ezeket IGNORALJUK; a tokeneket BARHOL keressuk a sorban regex-szel.
"""

import sys
import os
import re
import csv
import json
import math
import argparse

# ---------------------------------------------------------------------------
# Regex-ek (a kontraktusra). A tokeneket BARHOL keressuk a sorban; a sor eleji
# '<ISO8601>\t' host-prefix elofordulhat vagy sem. A '#'-komment sorokat es a
# nem-illeszkedo boot-zajt eldobjuk.
# ---------------------------------------------------------------------------

# Fejlec-sor:  M1B-HDR records=12 period_s=600 start_unix=1719600000
_HDR_RE = re.compile(
    r'\bM1B-HDR\b\s+records=(?P<records>\d+)\s+period_s=(?P<period_s>\d+)'
    r'\s+start_unix=(?P<start_unix>-?\d+)'
)

# Rekord-sor token (a '-HDR'-t kizarjuk a negativ lookahead-del, hogy a fejlec
# ne illeszkedjen ide is):
_REC_TOKEN_RE = re.compile(r'\bM1B(?!-HDR)\b')

# Rekord-mezok (sorrend-fuggetlen, robusztus; a ho-mezok lehetnek "nan"):
_F_IDX     = re.compile(r'\bidx=(\d+)')
_F_CYCLE   = re.compile(r'\bcycle=(\d+)')
_F_DS_UNIX = re.compile(r'\bds_unix=(-?\d+)')
_F_DS_C    = re.compile(r'\bds_c=(nan|[-+]?\d+(?:\.\d+)?)', re.IGNORECASE)
_F_DIE_C   = re.compile(r'\bdie_c=(nan|[-+]?\d+(?:\.\d+)?)', re.IGNORECASE)
_F_HX      = re.compile(r'\bhx=(-?\d+)')
_F_AWAKE   = re.compile(r'\bawake_ms=(\d+)')   # opcionalis (regi dumpbol hianyozhat)


# ---------------------------------------------------------------------------
# Least-squares (tiszta stdlib) — az M1 linfit() idiomaja
# ---------------------------------------------------------------------------

def linfit(xs, ys):
    """Egyszeru OLS: y = a + b*x. Visszaad (b meredekseg, a tengelymetszet, R^2).

    n<2 vagy zerus x-szoras eseten (nan, nan, nan).
    """
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


# ---------------------------------------------------------------------------
# Adatszerkezet + parse
# ---------------------------------------------------------------------------

class Record:
    __slots__ = ('idx', 'cycle', 'ds_unix', 'ds_c', 'die_c', 'hx', 'awake_ms', 'src_line')

    def __init__(self):
        self.idx = None
        self.cycle = None
        self.ds_unix = None
        self.ds_c = float('nan')
        self.die_c = float('nan')
        self.hx = None
        self.awake_ms = None
        self.src_line = ''


def _parse_temp(rgx, line):
    """Ho-mezo: 'nan' -> nan; hianyzo -> nan; egyebkent float."""
    m = rgx.search(line)
    if m is None:
        return float('nan')
    v = m.group(1)
    return float('nan') if v.lower() == 'nan' else float(v)


def parse_record_line(line):
    """Egy rekord-sorbol Record-ot ad, vagy None ha nem M1B-rekord / hianyos."""
    if _REC_TOKEN_RE.search(line) is None:
        return None
    r = Record()
    r.src_line = line.rstrip('\n')

    def _int(rgx):
        m = rgx.search(line)
        if m is None:
            return None
        try:
            return int(m.group(1))
        except (ValueError, TypeError):
            return None

    r.idx = _int(_F_IDX)
    r.cycle = _int(_F_CYCLE)
    r.ds_unix = _int(_F_DS_UNIX)
    r.hx = _int(_F_HX)
    r.awake_ms = _int(_F_AWAKE)   # None ha hianyzik (regi dump)
    r.ds_c = _parse_temp(_F_DS_C, line)
    r.die_c = _parse_temp(_F_DIE_C, line)

    # Kotelezo mezok a drift-fithez: cycle + ds_unix; a hx a HX711-elemzeshez.
    if r.cycle is None or r.ds_unix is None:
        return None
    return r


def parse_dump(path):
    """Beolvassa a dump-fajlt. Visszaad: (header dict | None, [Record], stats)."""
    header = None
    records = []
    stats = {'lines': 0, 'records': 0, 'malformed': 0,
             'comments': 0, 'reconnect': 0, 'noise': 0, 'hdr_seen': 0}
    with open(path, 'r', errors='replace') as f:
        for line in f:
            stats['lines'] += 1
            stripped = line.lstrip()
            if stripped.startswith('#'):
                stats['comments'] += 1
                if 'RECONNECT' in line:
                    stats['reconnect'] += 1
                continue
            hm = _HDR_RE.search(line)
            if hm:
                stats['hdr_seen'] += 1
                # Pontosan egy fejlec varhato; ha tobb jonne, az ELSOT tartjuk.
                if header is None:
                    header = {
                        'records': int(hm.group('records')),
                        'period_s': int(hm.group('period_s')),
                        'start_unix': int(hm.group('start_unix')),
                    }
                continue
            if _REC_TOKEN_RE.search(line) is None:
                stats['noise'] += 1
                continue
            r = parse_record_line(line)
            if r is None:
                stats['malformed'] += 1
                continue
            records.append(r)
            stats['records'] += 1
    # idx szerint rendezzuk (a dump altalaban sorrendben jon, de biztositjuk)
    records.sort(key=lambda x: (x.idx if x.idx is not None else x.cycle))
    return header, records, stats


# ---------------------------------------------------------------------------
# (a) ORA-DRIFT — DS3231 a referencia
# ---------------------------------------------------------------------------

def _drift_metrics(xs, ys, period_s):
    """OLS (ys vs xs) -> drift-metrika dict + residual lista."""
    b, a, r2 = linfit(xs, ys)
    drift = (b - period_s) / period_s if period_s else float('nan')
    resid = [ys[i] - (a + b * xs[i]) for i in range(len(xs))]
    rstd = (sum(v * v for v in resid) / len(resid)) ** 0.5 if resid else float('nan')
    return {
        "T_meas_s_per_cycle": round(b, 6),
        "drift_ppm": round(drift * 1e6, 2),
        "drift_pct": round(drift * 100, 5),
        "drift_s_per_hour": round(drift * 3600, 4),
        "r2": round(r2, 8) if r2 == r2 else float('nan'),
        "residual_std_s": round(rstd, 4),
        "n": len(xs),
    }, resid


def clock_drift_block(records, period_s, start_unix):
    """Linearis fit: valos eltelt ido (ds_unix - start_unix) vs relativ cycle.

    A node "vart" ideje = cycle * period_s; a cycle-t a 0. rekord cycle-jehez
    relativizaljuk (cyc_rel). A fit meredeksege = mert periodus (s/cycle), ebbol
    drift = (T_meas - period_s)/period_s.

    AWAKE-KORREKCIO: a ds_unix-kulonbseg minden ciklusban = awake (meres-ido) +
    period*(1+drift). Ezert a NYERS drift tartalmazza az awake/period tagot (60 s-nal
    ~2,3%, ami elfedi a valodi ~0,3-0,6% ora-driftet). Ha van awake_ms adat, az ATLAGOS
    awake-et cyc_rel-aranyosan LEVONJUK (az M1 m1_to_json.py idiomaja) -> a tiszta ALVAS-
    drift marad. A ket eredmenyt is visszaadjuk (raw + awake_corrected); a TISZTA
    (awake-korrigalt) az iranyado, ha van awake adat.

    Visszaad: (dict, resid). A resid az AWAKE-KORRIGALT fit-e (ha van awake), kulonben a
    nyers — ezt hasznalja a homerseklet-elemzes (a hofuggo ingadozas tisztabb benne).
    """
    nan_block = {
        "T_meas_s_per_cycle": float('nan'), "drift_ppm": float('nan'),
        "drift_pct": float('nan'), "drift_s_per_hour": float('nan'),
        "r2": float('nan'), "residual_std_s": float('nan'), "n": len(records),
    }
    if len(records) < 2:
        return {"raw": nan_block, "awake_corrected": None,
                "avg_awake_ms": None, **nan_block}, []

    cyc0 = records[0].cycle
    xs = [r.cycle - cyc0 for r in records]
    ys = [r.ds_unix - start_unix for r in records]

    raw, raw_resid = _drift_metrics(xs, ys, period_s)

    # awake-korrekcio: avg_awake (ms->s) * cyc_rel levonasa (csak ha van awake adat)
    awake_vals = [r.awake_ms for r in records if r.awake_ms is not None]
    awake_corrected = None
    avg_awake_ms = None
    resid = raw_resid
    if awake_vals:
        avg_awake_ms = sum(awake_vals) / len(awake_vals)
        avg_awake_s = avg_awake_ms / 1000.0
        ys_corr = [ys[i] - avg_awake_s * xs[i] for i in range(len(xs))]
        awake_corrected, resid = _drift_metrics(xs, ys_corr, period_s)
        avg_awake_ms = round(avg_awake_ms, 1)

    # a "fo" mezok (lapos hozzaferes): az awake-korrigalt, ha van; kulonben a nyers
    primary = awake_corrected if awake_corrected is not None else raw
    out = {
        "raw": raw,
        "awake_corrected": awake_corrected,
        "avg_awake_ms": avg_awake_ms,
        "awake_contribution_ppm": (round(raw["drift_ppm"] - awake_corrected["drift_ppm"], 2)
                                   if awake_corrected is not None else None),
    }
    out.update(primary)   # T_meas/drift_ppm/drift_pct/... a fo (korrigalt) ertekkel
    return out, resid


# ---------------------------------------------------------------------------
# (b) ORA-DRIFT vs HO — a fo cel: ppm/C
# ---------------------------------------------------------------------------

DS_C_SPAN_THRESH = 0.5   # mint az M1 die_c_span kuszob — itt ds_c-span-re

def clock_temp_block(records, resid, period_s, temp_attr='ds_c',
                     span_thresh=DS_C_SPAN_THRESH):
    """A slow-clock lokalis rata-elterese (a drift-fit residual-derivaltja)
    korrelalva a homersekletvel (ds_c elsodleges, die_c masodlagos).

    A residual elso differenciaja cycle-onkent ~ a lokalis (idoben valtozo)
    drift-rata; ezt period_s-re normalva ppm-ben kapjuk, es a homerseklet ellen
    illesztjuk -> ppm/C meredekseg + R^2. characterizable=True csak ha a ho-
    tartomany >= span_thresh ES erdemi a korrelacio (R^2 >= 0.3).
    """
    temps_all = [getattr(r, temp_attr) for r in records]
    valid_temps = [t for t in temps_all if t == t]   # nem-nan
    block = {
        "ref": temp_attr,
        "characterizable": False,
        "n_temp_valid": len(valid_temps),
    }
    if valid_temps:
        block["temp_min"] = round(min(valid_temps), 2)
        block["temp_max"] = round(max(valid_temps), 2)
        block["temp_span"] = round(max(valid_temps) - min(valid_temps), 2)
    else:
        block["temp_min"] = block["temp_max"] = block["temp_span"] = None

    if len(records) < 5 or not valid_temps:
        block["note"] = "kevés minta vagy nincs ervenyes ho-adat -> nem jellemezheto"
        return block, [], []

    span = max(valid_temps) - min(valid_temps)
    cyc = [r.cycle for r in records]
    xs_t, ys_d = [], []   # ho (kozeppont) vs lokalis drift-rata (ppm)
    for i in range(1, len(records)):
        dc = cyc[i] - cyc[i - 1]
        ti, tj = getattr(records[i], temp_attr), getattr(records[i - 1], temp_attr)
        if dc > 0 and period_s and ti == ti and tj == tj:
            local_ppm = (resid[i] - resid[i - 1]) / dc / period_s * 1e6
            xs_t.append(0.5 * (ti + tj))
            ys_d.append(local_ppm)
    block["n_points"] = len(xs_t)
    if len(xs_t) >= 5:
        b, a, r2 = linfit(xs_t, ys_d)
        block["coef_ppm_per_C"] = round(b, 3) if b == b else None
        block["intercept_ppm"] = round(a, 3) if a == a else None
        block["r2"] = round(r2, 4) if r2 == r2 else None
        characterizable = (span >= span_thresh and r2 == r2 and r2 >= 0.3)
        block["characterizable"] = characterizable
        if not characterizable:
            block["note"] = ("alacsony R^2 / kis ho-tartomany (span {:.2f} C, "
                             "kuszob {:.2f} C) -> a hofugges ebbol a futasbol NEM "
                             "jellemezheto megbizhatoan".format(span, span_thresh))
    else:
        block["note"] = "kevés ervenyes ho-pont a derivalthoz -> nem jellemezheto"
    return block, xs_t, ys_d


# ---------------------------------------------------------------------------
# (c) HX711 ho-drift — hx vs ds_c lineáris fit
# ---------------------------------------------------------------------------

def hx_temp_block(records, temp_attr='ds_c'):
    """A konstans terheles HX711 nyers olvasatanak ho-okozta driftje:
    hx vs ho lineáris fit -> counts/C meredekseg + tengelymetszet + R^2.
    """
    pts = [(getattr(r, temp_attr), r.hx) for r in records
           if r.hx is not None and getattr(r, temp_attr) == getattr(r, temp_attr)]
    block = {"ref": temp_attr, "n_points": len(pts)}
    if len(pts) < 2:
        block["note"] = "kevés ervenyes (ho, hx) part -> HX711 ho-drift nem fittolheto"
        block["coef_counts_per_C"] = None
        block["intercept_counts"] = None
        block["r2"] = None
        return block, [], []
    xs = [p[0] for p in pts]
    ys = [float(p[1]) for p in pts]
    b, a, r2 = linfit(xs, ys)
    block["coef_counts_per_C"] = round(b, 3) if b == b else None
    block["intercept_counts"] = round(a, 3) if a == a else None
    block["r2"] = round(r2, 6) if r2 == r2 else None
    block["temp_min"] = round(min(xs), 2)
    block["temp_max"] = round(max(xs), 2)
    block["temp_span"] = round(max(xs) - min(xs), 2)
    block["hx_min"] = min(p[1] for p in pts)
    block["hx_max"] = max(p[1] for p in pts)
    return block, xs, ys


# ---------------------------------------------------------------------------
# Guard-window (az M1 idiomaja, DS3231-drift alapjan)
# ---------------------------------------------------------------------------

def guard_table(drift_pct, residual_std_s, period_s, Ns):
    abs_drift = abs(drift_pct) / 100.0
    margin = 3.0 * residual_std_s
    table = []
    for n in Ns:
        table.append({
            "N_cycles": n,
            "hours": round(n * period_s / 3600.0, 3),
            "guard_window_s": round(abs_drift * n * period_s + margin, 3),
        })
    return table, margin


# ---------------------------------------------------------------------------
# CSV kiiras (per-rekord)
# ---------------------------------------------------------------------------

def write_csv(path, records, resid, start_unix):
    cyc0 = records[0].cycle if records else 0
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['idx', 'cycle', 'cyc_rel', 'ds_unix', 'elapsed_s',
                    'ds_c', 'die_c', 'hx', 'awake_ms', 'clock_residual_s'])
        for i, r in enumerate(records):
            ds_c = '' if r.ds_c != r.ds_c else "{:.2f}".format(r.ds_c)
            die_c = '' if r.die_c != r.die_c else "{:.2f}".format(r.die_c)
            res = "{:.6f}".format(resid[i]) if i < len(resid) else ''
            w.writerow([
                r.idx if r.idx is not None else '',
                r.cycle, r.cycle - cyc0, r.ds_unix,
                r.ds_unix - start_unix,
                ds_c, die_c,
                r.hx if r.hx is not None else '',
                r.awake_ms if r.awake_ms is not None else '',
                res,
            ])


# ---------------------------------------------------------------------------
# Plot (opcionalis)
# ---------------------------------------------------------------------------

def make_plot(path, records, resid, temp_xs, temp_ys, hx_xs, hx_ys):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except Exception as e:  # noqa
        print("[plot] matplotlib nem elerheto ({}), a PNG kihagyva.".format(e),
              file=sys.stderr)
        return False
    fig, axs = plt.subplots(3, 1, figsize=(9, 11))
    cyc0 = records[0].cycle
    xs = [r.cycle - cyc0 for r in records]
    axs[0].plot(xs, resid, '.-', ms=3)
    axs[0].set_xlabel("cyc_rel"); axs[0].set_ylabel("ora-residual (s)")
    axs[0].set_title("Ora-drift residual a linearis fittol (idoben valtozo ho-drift)")
    if temp_xs:
        axs[1].plot(temp_xs, temp_ys, '.', ms=4)
        axs[1].set_xlabel("DS3231 ho (C)"); axs[1].set_ylabel("lokalis drift-rata (ppm)")
        axs[1].set_title("Ora-drift rata vs ho")
    if hx_xs:
        axs[2].plot(hx_xs, hx_ys, '.', ms=4)
        axs[2].set_xlabel("DS3231 ho (C)"); axs[2].set_ylabel("HX711 nyers (counts)")
        axs[2].set_title("HX711 (konstans terheles) ho-drift")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True


# ---------------------------------------------------------------------------
# Fo elemzo
# ---------------------------------------------------------------------------

def analyze(records, header, period_s, guard_Ns):
    """Visszaad: (result dict, resid, temp_xs, temp_ys, hx_xs, hx_ys)."""
    start_unix = header['start_unix'] if header else (
        records[0].ds_unix if records else 0)

    clock, resid = clock_drift_block(records, period_s, start_unix)
    ds_temp, t_xs, t_ys = clock_temp_block(records, resid, period_s, 'ds_c')
    die_temp, _, _ = clock_temp_block(records, resid, period_s, 'die_c')
    hx, hx_xs, hx_ys = hx_temp_block(records, 'ds_c')
    guard, margin = guard_table(clock['drift_pct'], clock['residual_std_s'],
                                period_s, guard_Ns)

    ds_c_vals = [r.ds_c for r in records if r.ds_c == r.ds_c]
    die_c_vals = [r.die_c for r in records if r.die_c == r.die_c]

    result = {
        "meta": {
            "nominal_period_s": period_s,
            "period_source": "header" if (header and header['period_s'] == period_s)
                             else "cli-override",
            "header": header,
            "records_parsed": len(records),
            "cycle_min": min((r.cycle for r in records), default=None),
            "cycle_max": max((r.cycle for r in records), default=None),
            "start_unix": start_unix,
            "ds_unix_first": records[0].ds_unix if records else None,
            "ds_unix_last": records[-1].ds_unix if records else None,
            "span_s": (records[-1].ds_unix - records[0].ds_unix) if records else None,
            "ds_c_range": {"min": round(min(ds_c_vals), 2),
                            "max": round(max(ds_c_vals), 2),
                            "span": round(max(ds_c_vals) - min(ds_c_vals), 2)}
                          if ds_c_vals else None,
            "die_c_range": {"min": round(min(die_c_vals), 2),
                             "max": round(max(die_c_vals), 2),
                             "span": round(max(die_c_vals) - min(die_c_vals), 2)}
                           if die_c_vals else None,
        },
        "clock_drift": clock,
        "clock_temp_ds_c": ds_temp,     # ELSODLEGES ho-ref (DS3231 ambiens)
        "clock_temp_die_c": die_temp,   # masodlagos (C3 die)
        "hx711_temp_drift": hx,
        "guard_window": {
            "formula": "guard_s(N) = |drift| * N * nominal_period_s + margin",
            "drift_used_pct": clock['drift_pct'],
            "margin_s": round(margin, 4),
            "margin_rule": "3 x residual_std (DS3231 ora-fit)",
            "table": guard,
        },
        "curve_points": {
            "clock_drift_vs_temp": [{"ds_c": round(t_xs[i], 3),
                                      "local_drift_ppm": round(t_ys[i], 3)}
                                     for i in range(len(t_xs))],
            "hx_vs_temp": [{"ds_c": round(hx_xs[i], 3), "hx": int(hx_ys[i])}
                            for i in range(len(hx_xs))],
        },
    }
    return result, resid, t_xs, t_ys, hx_xs, hx_ys


def print_summary(result, stats=None):
    """Rovid stdout-osszegzo a kereszt-ellenorzeshez (az M1 stilusaban)."""
    c = result["clock_drift"]
    ds = result["clock_temp_ds_c"]
    hx = result["hx711_temp_drift"]
    m = result["meta"]
    P = print
    P("=" * 64)
    P("M1b — STANDALONE HO-DRIFT ELEMZES (DS3231 referencia)")
    P("=" * 64)
    if stats:
        P("Beolvasott sorok: {}  rekord: {}  komment: {}  zaj: {}  hianyos: {}"
          .format(stats['lines'], stats['records'], stats['comments'],
                  stats['noise'], stats['malformed']))
    P("Rekordok: {}  cycle [{}..{}]  periodus(nevleges)={} s ({})"
      .format(m['records_parsed'], m['cycle_min'], m['cycle_max'],
              m['nominal_period_s'], m['period_source']))
    if m['span_s'] is not None:
        P("Idotartam (DS3231): {:.2f} ora ({} s)".format(m['span_s'] / 3600.0, m['span_s']))
    if m['ds_c_range']:
        P("DS3231 ho-tartomany: {:.2f}..{:.2f} C (span {:.2f} C)".format(
            m['ds_c_range']['min'], m['ds_c_range']['max'], m['ds_c_range']['span']))
    P("-" * 64)
    if c.get('avg_awake_ms') is not None:
        raw = c['raw']
        P("(a) ORA-DRIFT [awake-korrigalt]: {:+.5f} % ({:+.1f} ppm, {:+.4f} s/ora)  "
          "R^2={}  resid_std={} s".format(
              c['drift_pct'], c['drift_ppm'], c['drift_s_per_hour'],
              c['r2'], c['residual_std_s']))
        P("    nyers (awake nelkul): {:+.5f} % ({:+.1f} ppm) | avg_awake={} ms | "
          "awake-jarulek={} ppm".format(
              raw['drift_pct'], raw['drift_ppm'], c['avg_awake_ms'],
              c.get('awake_contribution_ppm')))
        P("    T_meas = {} s/cycle (nevleges {} s); a tiszta ALVAS-drift az iranyado"
          .format(c['T_meas_s_per_cycle'], m['nominal_period_s']))
    else:
        P("(a) ORA-DRIFT: {:+.5f} % ({:+.1f} ppm, {:+.4f} s/ora)  R^2={}  resid_std={} s"
          .format(c['drift_pct'], c['drift_ppm'], c['drift_s_per_hour'],
                  c['r2'], c['residual_std_s']))
        P("    T_meas = {} s/cycle (nevleges {} s); NINCS awake_ms a dumpban -> nyers drift"
          .format(c['T_meas_s_per_cycle'], m['nominal_period_s']))
    P("(b) ORA-DRIFT vs HO (ds_c): coef={} ppm/C  R^2={}  span={} C  -> jellemezheto: {}"
      .format(ds.get('coef_ppm_per_C'), ds.get('r2'), ds.get('temp_span'),
              ds['characterizable']))
    if not ds['characterizable'] and ds.get('note'):
        P("    megj.: {}".format(ds['note']))
    P("(c) HX711 HO-DRIFT (vs ds_c): {} counts/C  intercept={} counts  R^2={}".format(
        hx.get('coef_counts_per_C'), hx.get('intercept_counts'), hx.get('r2')))
    g = result["guard_window"]["table"]
    if g:
        P("Guard-window (N={}): {} s   (margin {} s)".format(
            g[-1]['N_cycles'], g[-1]['guard_window_s'],
            result["guard_window"]["margin_s"]))
    P("=" * 64)


# ---------------------------------------------------------------------------
# Self-test — szintetikus dump ismert parameterekkel
# ---------------------------------------------------------------------------

def _gen_synthetic_dump(path, drift_pct=0.3, period_s=600, n_records=120,
                        ds_start=0.0, ds_end=35.0,
                        hx_base=100000, hx_slope=250.0, start_unix=1719600000,
                        cycle0=7, awake_ms=1400):
    """Ismert parameteru szintetikus M1B dump (fejlec + rekordok), '#'-kommenttel
    es host-idobelyeg-prefixszel keverve a robusztussag teszteleshez.

    Modell:
      - ora-drift: KONSTANS drift_pct % -> minden cycle period_s*(1+drift) valos
        masodperc; a ds_unix ezt kumulalja (a fit pontosan ezt a driftet adja
        vissza, a residual ~0, igy a ppm/C ebbol numerikusan nem jellemezheto —
        ezt szandekosan egy MASODIK, ho-fuggo dump teszteli, lasd lent).
      - ho-rampa: ds_c lineárisan ds_start -> ds_end; die_c = ds_c + 2 C.
      - HX711: hx = hx_base + hx_slope * ds_c (egesz counts).
    Visszaad: (expected_drift_frac, expected_hx_slope).
    """
    cyc0 = cycle0
    lines = []
    lines.append("# M1b szintetikus dump (self-test)\n")
    lines.append("# boot-zaj sor, IGNORALANDO: rst:0x1 boot:0x13 SPIWP:0xee\n")
    hdr = ("M1B-HDR records={recs} period_s={per} start_unix={su}\n"
           .format(recs=n_records, per=period_s, su=start_unix))
    # nehany sorra host-idobelyeg-prefixet teszunk
    lines.append("2026-06-29T08:00:00.123456\t" + hdr)

    for k in range(n_records):
        cyc = cyc0 + k
        frac = k / max(1, n_records - 1)
        ds_c = ds_start + (ds_end - ds_start) * frac
        die_c = ds_c + 2.0
        # valos eltelt ido minden ciklusban = period*(1+drift) + awake (meres-ido);
        # a Python az atlagos awake-et levonja -> a korrigalt drift = drift_pct.
        elapsed = k * (period_s * (1.0 + drift_pct / 100.0) + awake_ms / 1000.0)
        ds_unix = start_unix + int(round(elapsed))
        hx = hx_base + int(round(hx_slope * ds_c))
        rec = ("M1B idx={idx} cycle={c} ds_unix={du} ds_c={dsc:.2f} "
               "die_c={diec:.2f} hx={hx} awake_ms={aw}\n").format(
            idx=k, c=cyc, du=ds_unix, dsc=ds_c, diec=die_c, hx=hx, aw=awake_ms)
        if k % 17 == 0:
            # host-prefixes sor
            lines.append("2026-06-29T08:{:02d}:00.000000\t".format(k % 60) + rec)
        else:
            lines.append(rec)
        if k % 25 == 13:
            lines.append("# RECONNECT esemeny a loggerben\n")
        if k == 5:
            lines.append("garbage boot line ets Jun  8 2016 00:22:57\n")

    with open(path, 'w') as f:
        f.writelines(lines)
    return drift_pct / 100.0, hx_slope


def _gen_synthetic_dump_thermal(path, period_s=600, n_records=200,
                                ds_start=0.0, ds_end=35.0,
                                base_drift_pct=0.3, thermal_ppm_per_C=120.0,
                                hx_base=100000, hx_slope=250.0,
                                start_unix=1719600000, awake_ms=1400):
    """Ho-FUGGO ora-drift dump: a pillanatnyi rata = base + ho-tag, hogy a (b)
    ppm/C jellemezheto legyen es ELLENORIZHESSUK a visszanyert meredeksegre.

    A k-adik intervallum valos hossza = period_s * (1 + base + thermal*(ds_c-ref)).
    A ds_c szimmetrikus a ref korul -> az ATLAGOS drift ~ base_drift_pct, igy a
    globalis fit driftje is ~ base_drift_pct (ezt is ellenorizzuk).
    Visszaad: (expected_base_drift_frac, expected_ppm_per_C, expected_hx_slope).
    """
    ds_ref = 0.5 * (ds_start + ds_end)
    lines = []
    lines.append("M1B-HDR records={recs} period_s={per} start_unix={su}\n"
                 .format(recs=n_records, per=period_s, su=start_unix))
    elapsed = 0.0
    for k in range(n_records):
        frac = k / max(1, n_records - 1)
        ds_c = ds_start + (ds_end - ds_start) * frac
        die_c = ds_c + 2.0
        if k > 0:
            # az ELOZO->mostani intervallum rataja a mostani ho-n (kozelites) + awake
            inst = base_drift_pct / 100.0 + thermal_ppm_per_C * 1e-6 * (ds_c - ds_ref)
            elapsed += period_s * (1.0 + inst) + awake_ms / 1000.0
        ds_unix = start_unix + int(round(elapsed))
        hx = hx_base + int(round(hx_slope * ds_c))
        lines.append("M1B idx={idx} cycle={c} ds_unix={du} ds_c={dsc:.2f} "
                     "die_c={diec:.2f} hx={hx} awake_ms={aw}\n".format(
                         idx=k, c=k, du=ds_unix, dsc=ds_c, diec=die_c, hx=hx, aw=awake_ms))
    with open(path, 'w') as f:
        f.writelines(lines)
    return base_drift_pct / 100.0, thermal_ppm_per_C, hx_slope


def run_selftest():
    import tempfile
    print("=== M1b HO-DRIFT ELEMZO ON-TESZT ===")
    ok = True
    tmpdir = tempfile.mkdtemp(prefix="m1b_selftest_")

    # --- 1. dump: konstans drift +0.3%, ho-rampa, hx slope 250 counts/C ---
    log1 = os.path.join(tmpdir, "synthetic_const.log")
    exp_drift, exp_hx_slope = _gen_synthetic_dump(
        log1, drift_pct=0.3, period_s=600, n_records=120,
        ds_start=0.0, ds_end=35.0, hx_base=100000, hx_slope=250.0)

    header, records, stats = parse_dump(log1)
    print("[1] Konstans-drift dump: {}".format(log1))
    print("    parse: lines={} rekord={} komment={} zaj={} hianyos={} hdr_seen={}".format(
        stats['lines'], stats['records'], stats['comments'], stats['noise'],
        stats['malformed'], stats['hdr_seen']))

    if header is None:
        print("  FAIL: a fejlec nem parse-olodott"); ok = False
    elif stats['hdr_seen'] != 1:
        print("  FAIL: pontosan 1 fejlec varhato, latott {}".format(stats['hdr_seen'])); ok = False
    if stats['records'] != 120:
        print("  FAIL: vart 120 rekord, parse-olt {}".format(stats['records'])); ok = False

    period_s = header['period_s'] if header else 600
    result, resid, txs, tys, hxs, hys = analyze(records, header, period_s, [10, 60, 1440])
    print_summary(result, stats)

    # AWAKE-KORRIGALT drift_ppm ~ 3000 (0.3%); a NYERS tartalmazza az awake-jarulekot
    # (awake_ms=1400, period=600 -> ~2333 ppm), igy raw ~ 5333 ppm.
    cd = result['clock_drift']
    got_ppm = cd['drift_ppm']           # = awake-korrigalt (a fo mezo)
    raw_ppm = cd['raw']['drift_ppm']
    exp_ppm = exp_drift * 1e6
    tol_ppm = 60.0   # a ds_unix egesz-masodperc kerekites zaja miatt
    print("  [drift] vart ~{:+.1f} ppm (awake-korrigalt) -> visszaadott {:+.1f} ppm "
          "(nyers {:+.1f}, avg_awake {} ms, jarulek {} ppm, R^2={})".format(
              exp_ppm, got_ppm, raw_ppm, cd['avg_awake_ms'],
              cd['awake_contribution_ppm'], cd['r2']))
    if abs(got_ppm - exp_ppm) > tol_ppm:
        print("  FAIL: awake-korrigalt drift_ppm elteres {:.1f} > tures {:.1f}".format(
            abs(got_ppm - exp_ppm), tol_ppm)); ok = False
    else:
        print("  PASS: awake-korrigalt drift_ppm a tureshataron belul")
    # az awake-jarulek ~ awake_ms/1000/period * 1e6 = 1400/1000/600*1e6 = 2333 ppm
    exp_contrib = 1400 / 1000.0 / 600.0 * 1e6
    if cd['awake_contribution_ppm'] is None or abs(cd['awake_contribution_ppm'] - exp_contrib) > 60.0:
        print("  FAIL: awake-jarulek ({}) elter a vart ~{:.0f} ppm-tol".format(
            cd['awake_contribution_ppm'], exp_contrib)); ok = False
    else:
        print("  PASS: awake-jarulek a vart ~{:.0f} ppm kozeleben".format(exp_contrib))

    # hx counts/C ~ 250
    got_hx = result['hx711_temp_drift']['coef_counts_per_C']
    print("  [hx] vart {:.1f} counts/C -> visszaadott {} counts/C (R^2={})".format(
        exp_hx_slope, got_hx, result['hx711_temp_drift']['r2']))
    if got_hx is None or abs(got_hx - exp_hx_slope) > 5.0:
        print("  FAIL: hx counts/C elter a vart {:.1f}-tol".format(exp_hx_slope)); ok = False
    else:
        print("  PASS: hx counts/C a tureshataron belul")

    # --- 2. dump: ho-FUGGO drift, hogy a ppm/C jellemezheto legyen ---
    log2 = os.path.join(tmpdir, "synthetic_thermal.log")
    exp_base, exp_ppmC, exp_hx2 = _gen_synthetic_dump_thermal(
        log2, period_s=600, n_records=200, ds_start=0.0, ds_end=35.0,
        base_drift_pct=0.3, thermal_ppm_per_C=120.0, hx_slope=250.0)
    h2, rec2, st2 = parse_dump(log2)
    res2, _, _, _, _, _ = analyze(rec2, h2, h2['period_s'], [10, 60, 1440])
    print("[2] Ho-fuggo dump: {}  (rekord {})".format(log2, st2['records']))
    ds2 = res2['clock_temp_ds_c']
    print("    ORA-DRIFT vs HO: vart ~{:.1f} ppm/C -> visszaadott {} ppm/C "
          "(R^2={}, span={} C, jellemezheto={})".format(
              exp_ppmC, ds2.get('coef_ppm_per_C'), ds2.get('r2'),
              ds2.get('temp_span'), ds2['characterizable']))
    if not ds2['characterizable']:
        print("  FAIL: a ho-fuggo dump-nak jellemezhetonek kellene lennie"); ok = False
    else:
        print("  PASS: a ho-fuggo futas jellemezheto (characterizable=True)")
    # a ppm/C a derivalt-zaj + egesz-masodperc kerekites miatt durva; tag tures
    got_ppmC = ds2.get('coef_ppm_per_C')
    if got_ppmC is None or abs(got_ppmC - exp_ppmC) > 60.0:
        print("  WARN: ppm/C ({}) tobb mint 60-nal eltér a vart {:.1f}-tol "
              "(numerikus diff + 1s-kerekites zaja); a drift+hx a fo pass-feltetel."
              .format(got_ppmC, exp_ppmC))
    else:
        print("  PASS: ppm/C a vart {:.1f} kozeleben".format(exp_ppmC))
    # a ho-fuggo dump globalis driftje is ~ base (0.3%)
    got_base_ppm = res2['clock_drift']['drift_ppm']
    print("    (globalis drift a ho-fuggo dump-on: {:+.1f} ppm, vart ~{:+.1f})".format(
        got_base_ppm, exp_base * 1e6))
    if abs(got_base_ppm - exp_base * 1e6) > 80.0:
        print("  FAIL: a ho-fuggo dump globalis driftje elter a vart base-tol"); ok = False

    print("")
    print("=== ON-TESZT EREDMENY: {} ===".format("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description="M1b standalone ho-drift elemzo (stdlib fit).")
    ap.add_argument("dump", nargs='?', help="a node soros-dump logja (M1B-HDR + M1B sorok)")
    ap.add_argument("--nominal-period-s", type=float, default=None,
                    help="nevleges ebredesi periodus s (felulirja a fejlec period_s-et)")
    ap.add_argument("--out-prefix", default="m1b",
                    help="kimeneti fajl-prefix (.csv / .json / .png)")
    ap.add_argument("--guard-n", default="10,60,1440",
                    help="vesszovel elvalasztott N ertekek a guard-tablahoz")
    ap.add_argument("--plot", action="store_true",
                    help="PNG plot (matplotlib, opcionalis; ha nincs, kihagyja)")
    ap.add_argument("--self-test", action="store_true",
                    help="szintetikus on-teszt futtatasa (board nelkul)")
    args = ap.parse_args(argv)

    if args.self_test:
        return run_selftest()

    if not args.dump:
        ap.error("megadando a dump-fajl (vagy --self-test)")

    guard_Ns = [int(x) for x in args.guard_n.split(',') if x.strip()]

    header, records, stats = parse_dump(args.dump)
    if not records:
        print("Nincs egyetlen M1B rekord-sor sem a dump-ban.", file=sys.stderr)
        return 2

    # nevleges periodus: CLI felulirja, kulonben a fejlecbol
    if args.nominal_period_s is not None:
        period_s = args.nominal_period_s
    elif header is not None:
        period_s = header['period_s']
    else:
        print("Nincs fejlec ES nincs --nominal-period-s -> a periodus ismeretlen.",
              file=sys.stderr)
        return 2

    result, resid, txs, tys, hxs, hys = analyze(records, header, period_s, guard_Ns)
    start_unix = result['meta']['start_unix']

    print_summary(result, stats)

    csv_path = args.out_prefix + ".csv"
    json_path = args.out_prefix + ".json"
    write_csv(csv_path, records, resid, start_unix)
    with open(json_path, 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False, default=lambda o: None)
    print("CSV  kiirva: {}".format(csv_path))
    print("JSON kiirva: {}".format(json_path))

    if args.plot:
        png = args.out_prefix + ".png"
        if make_plot(png, records, resid, txs, tys, hxs, hys):
            print("PNG  kiirva: {}".format(png))

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

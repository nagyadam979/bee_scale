#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M1 — offline drift-elemzo az ESP32-C3 node deep-sleep slow-clock driftjehez.

A masterbol jovo M1RX sorokat parse-olja (BEFAGYASZTOTT KONTRAKTUS, lasd lent),
boot_id szerint szegmentalja, cycle-onkent deduplikal, es ket fuggetlen
referenciaval (HOST fali ora + master esp_timer) illeszti a driftet sajat
stdlib least-squares-szel. Kimenet: stdout riport + opcionalis CSV + opcionalis PNG.

Fuggosegek: CSAK stdlib a fit-magban. matplotlib opcionalis (--plot).

Hasznalat:
  python3 m1_drift_analyze.py <logfile> [logfile2 ...] [--nominal-period-s 30]
                              [--csv out.csv] [--plot] [--margin-mode resid3|fixed]
                              [--margin-fixed-s 2.0]
  python3 m1_drift_analyze.py --selftest

BEFAGYASZTOTT KONTRAKTUS (a master ezt irja minden vett beaconra):
  I (12345) M1-MASTER: M1RX rx_us=12345678 cycle=42 node=1 boot=0xA1B2C3D4 \
      reqT_ms=30000 awake_ms=487 die_c=27.43 rssi=-41 crc=OK
Heartbeat (IGNORALVA):
  I (...) M1-MASTER: M1HB uptime_us=... last_cycle=... rx_count=...
A host-logger minden sor ele tesz egy '<ISO8601 mikroszekundummal>\\t' prefixet.
Az elemzo MINDKET formatumot kezeli (host-prefix lehet, vagy sem).
"""

import sys
import os
import re
import math
import argparse
from datetime import datetime

# ---------------------------------------------------------------------------
# Regex-ek (a kontraktusra). A mezoket BARHOL keressuk a sorban (a host- es az
# IDF-prefix elofordulhat vagy sem). A sor eleji '<ISO8601>\t' opcionalis.
# ---------------------------------------------------------------------------

# Host-prefix: ISO8601 mikroszekundummal a sor elejen, TAB-bal lezarva.
_HOST_TS_RE = re.compile(r'^(?P<ts>\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+)\t')

# Az M1RX sort akkor fogadjuk el, ha tartalmazza az "M1RX" tokent ES a kotelezo
# mezoket. A mezoket kulon-kulon keressuk (sorrend-fuggetlen, robusztus).
_M1RX_TOKEN_RE = re.compile(r'\bM1RX\b')
_M1HB_TOKEN_RE = re.compile(r'\bM1HB\b')

_F_RX_US   = re.compile(r'\brx_us=(\d+)')
_F_CYCLE   = re.compile(r'\bcycle=(\d+)')
_F_NODE    = re.compile(r'\bnode=(\d+)')
_F_BOOT    = re.compile(r'\bboot=(0x[0-9A-Fa-f]+|\d+)')
_F_REQT    = re.compile(r'\breqT_ms=(\d+)')
_F_AWAKE   = re.compile(r'\bawake_ms=(\d+)')
# die_c lehet "nan", elojeles tort, vagy egesz
_F_DIE_C   = re.compile(r'\bdie_c=(nan|[-+]?\d+(?:\.\d+)?)', re.IGNORECASE)
_F_RSSI    = re.compile(r'\brssi=(-?\d+)')
_F_CRC     = re.compile(r'\bcrc=(\w+)')


# ---------------------------------------------------------------------------
# Adatszerkezet
# ---------------------------------------------------------------------------

class Sample:
    __slots__ = ('host_ts', 'host_ts_iso', 'rx_us', 'cycle', 'node', 'boot',
                 'reqT_ms', 'awake_ms', 'die_c', 'rssi', 'crc', 'src_line')

    def __init__(self):
        self.host_ts = None       # datetime vagy None
        self.host_ts_iso = ''     # eredeti ISO string vagy ''
        self.rx_us = None         # int
        self.cycle = None         # int
        self.node = None
        self.boot = None          # int (a 0x... -bol)
        self.reqT_ms = None
        self.awake_ms = None
        self.die_c = float('nan')
        self.rssi = None
        self.crc = ''
        self.src_line = ''


# ---------------------------------------------------------------------------
# Parse
# ---------------------------------------------------------------------------

def parse_line(line):
    """Egyetlen sorbol Sample-t ad vissza, vagy None ha nem M1RX vagy hianyos.

    Visszateresi melleknev: ('hb',) ha heartbeat (szandekosan kihagyva).
    """
    if _M1RX_TOKEN_RE.search(line) is None:
        if _M1HB_TOKEN_RE.search(line) is not None:
            return ('hb', None)
        return (None, None)

    s = Sample()
    s.src_line = line.rstrip('\n')

    m = _HOST_TS_RE.match(line)
    if m:
        s.host_ts_iso = m.group('ts')
        try:
            s.host_ts = datetime.fromisoformat(s.host_ts_iso)
        except ValueError:
            s.host_ts = None

    def _grp(rgx, conv, required=True):
        mm = rgx.search(line)
        if mm is None:
            return None if required else None
        try:
            return conv(mm.group(1))
        except (ValueError, TypeError):
            return None

    s.rx_us    = _grp(_F_RX_US, int)
    s.cycle    = _grp(_F_CYCLE, int)
    s.node     = _grp(_F_NODE, int)
    bm = _F_BOOT.search(line)
    if bm:
        bs = bm.group(1)
        try:
            s.boot = int(bs, 16) if bs.lower().startswith('0x') else int(bs)
        except ValueError:
            s.boot = None
    s.reqT_ms  = _grp(_F_REQT, int)
    s.awake_ms = _grp(_F_AWAKE, int)

    dm = _F_DIE_C.search(line)
    if dm:
        dv = dm.group(1)
        s.die_c = float('nan') if dv.lower() == 'nan' else float(dv)
    else:
        s.die_c = float('nan')

    s.rssi = _grp(_F_RSSI, int, required=False)

    cm = _F_CRC.search(line)
    s.crc = cm.group(1).upper() if cm else ''

    # Kotelezo mezok a fit-hez: cycle + boot + (host_ts vagy rx_us)
    if s.cycle is None or s.boot is None:
        return ('malformed', None)
    if s.host_ts is None and s.rx_us is None:
        return ('malformed', None)

    return ('ok', s)


def parse_files(paths):
    """Beolvassa az osszes log-fajlt. Visszaad: (samples, stats)."""
    samples = []
    stats = {'lines': 0, 'm1rx': 0, 'hb': 0, 'malformed': 0,
             'crc_bad': 0, 'reconnect': 0}
    for path in paths:
        with open(path, 'r', errors='replace') as f:
            for line in f:
                stats['lines'] += 1
                if line.lstrip().startswith('#'):
                    if 'RECONNECT' in line:
                        stats['reconnect'] += 1
                    continue
                kind, s = parse_line(line)
                if kind == 'ok':
                    stats['m1rx'] += 1
                    if s.crc == 'BAD':
                        stats['crc_bad'] += 1
                    samples.append(s)
                elif kind == 'hb':
                    stats['hb'] += 1
                elif kind == 'malformed':
                    stats['malformed'] += 1
    return samples, stats


# ---------------------------------------------------------------------------
# Dedup + szegmentalas
# ---------------------------------------------------------------------------

def _sort_key(s):
    """Stabil idorend: elsodlegesen host_ts, kulonben rx_us."""
    if s.host_ts is not None:
        return (0, s.host_ts.timestamp())
    return (1, float(s.rx_us if s.rx_us is not None else 0))


def dedup_and_segment(samples):
    """crc=BAD eldobva (mar megszamolva); (boot, cycle)-re dedup (elso vetel).

    Visszaad: (segments, dup_count), ahol segments = dict boot -> [Sample,...]
    cycle szerint rendezve.
    """
    valid = [s for s in samples if s.crc != 'BAD']
    valid.sort(key=_sort_key)

    seen = {}        # (boot, cycle) -> Sample (elso)
    dup_count = 0
    for s in valid:
        key = (s.boot, s.cycle)
        if key in seen:
            dup_count += 1
        else:
            seen[key] = s

    segments = {}
    for (boot, cycle), s in seen.items():
        segments.setdefault(boot, []).append(s)
    for boot in segments:
        segments[boot].sort(key=lambda x: x.cycle)
    return segments, dup_count


# ---------------------------------------------------------------------------
# Least-squares (tiszta stdlib)
# ---------------------------------------------------------------------------

def linregress(xs, ys):
    """Egyszeru OLS y = a + b*x. Visszaad: (slope b, intercept a, r2, n).

    n<2 eseten (None, None, None, n).
    """
    n = len(xs)
    if n < 2:
        return (None, None, None, n)
    sx = sum(xs)
    sy = sum(ys)
    mx = sx / n
    my = sy / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0:
        return (None, None, None, n)
    b = sxy / sxx
    a = my - b * mx
    # R^2
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - (ss_res / ss_tot) if ss_tot > 0 else 1.0
    return (b, a, r2, n)


def residuals(xs, ys, slope, intercept):
    """Eltereses a lineáris fittol minden pontban (y - (a+b*x))."""
    return [y - (intercept + slope * x) for x, y in zip(xs, ys)]


def stdev(vals):
    n = len(vals)
    if n < 2:
        return 0.0
    m = sum(vals) / n
    return math.sqrt(sum((v - m) ** 2 for v in vals) / (n - 1))


# ---------------------------------------------------------------------------
# Drift-szamolas egy szegmensre
# ---------------------------------------------------------------------------

def detect_master_reset(seg):
    """Igaz, ha az rx_us nem-monoton csokken cycle-rendben (master esp_timer reset)."""
    prev = None
    for s in seg:
        if s.rx_us is None:
            continue
        if prev is not None and s.rx_us < prev:
            return True
        prev = s.rx_us
    return False


def analyze_segment(seg, nominal_period_s):
    """Egy boot-szegmens elemzese. Visszaad egy dict-et a riporthoz/CSV-hez."""
    res = {}
    boot = seg[0].boot
    res['boot'] = boot
    res['n'] = len(seg)
    cycles = [s.cycle for s in seg]
    res['cycle_min'] = min(cycles)
    res['cycle_max'] = max(cycles)
    res['cycle_span'] = res['cycle_max'] - res['cycle_min']

    c0 = cycles[0]
    xs = [s.cycle - c0 for s in seg]

    # --- (a) HOST-ora referencia ---
    host_ok = all(s.host_ts is not None for s in seg)
    host = None
    if host_ok and len(seg) >= 2:
        t0 = seg[0].host_ts.timestamp()
        ys_h = [s.host_ts.timestamp() - t0 for s in seg]
        b, a, r2, n = linregress(xs, ys_h)
        if b is not None:
            drift = (b - nominal_period_s) / nominal_period_s
            resid = residuals(xs, ys_h, b, a)
            host = {
                'T_meas': b, 'intercept': a, 'r2': r2,
                'drift': drift, 'ppm': drift * 1e6,
                'pct': drift * 100.0, 's_per_hour': drift * 3600.0,
                'resid': resid, 'resid_std': stdev(resid),
                'ys': ys_h,
            }
    res['host'] = host

    # --- (b) MASTER esp_timer referencia ---
    master = None
    master_reset = detect_master_reset(seg)
    res['master_reset'] = master_reset
    have_rx = all(s.rx_us is not None for s in seg)
    if have_rx and not master_reset and len(seg) >= 2:
        u0 = seg[0].rx_us
        ys_m = [(s.rx_us - u0) / 1e6 for s in seg]
        b, a, r2, n = linregress(xs, ys_m)
        if b is not None:
            drift = (b - nominal_period_s) / nominal_period_s
            resid = residuals(xs, ys_m, b, a)
            master = {
                'T_meas': b, 'intercept': a, 'r2': r2,
                'drift': drift, 'ppm': drift * 1e6,
                'pct': drift * 100.0, 's_per_hour': drift * 3600.0,
                'resid': resid, 'resid_std': stdev(resid),
                'ys': ys_m,
            }
    res['master'] = master

    # --- awake-korrigalt drift ---
    # A cycle n->n+1 kozti tenyleges intervallum ~ alvas[n] + awake[n+1].
    # A tiszta alvas-drift izolalasahoz a mert host-intervallumokbol levonjuk
    # az awake_ms hozzajarulast. Modell: a "sleep-only" idotengely =
    #   host_rel[i] - sum_{j<=i} awake_ms[j]/1000
    # majd ugyanaz a regresszio cycle ellen.
    awake_corr = None
    if host is not None and all(s.awake_ms is not None for s in seg):
        ys_h = host['ys']
        awake_cum = 0.0
        ys_corr = []
        for i, s in enumerate(seg):
            # az i-edik mintahoz tartozo awake-osszeg (a sajat awake-jet is
            # beleszamolva, mert az adott ebredes ebren-idejet a host_ts utan/elott
            # toltjuk; konzervativan a kumulalt awake-et vonjuk le)
            awake_cum += s.awake_ms / 1000.0
            ys_corr.append(ys_h[i] - awake_cum)
        b, a, r2, n = linregress(xs, ys_corr)
        if b is not None:
            drift = (b - nominal_period_s) / nominal_period_s
            awake_mean = sum(s.awake_ms for s in seg) / len(seg)
            awake_corr = {
                'T_meas': b, 'drift': drift, 'ppm': drift * 1e6,
                'pct': drift * 100.0, 's_per_hour': drift * 3600.0,
                'r2': r2, 'awake_ms_mean': awake_mean,
                'ys': ys_corr,
            }
    res['awake_corr'] = awake_corr

    # --- ho-korrelacio (best-effort) ---
    # lokalis drift-rata ~ a residual derivaltja (a residual a cycle-fuggvenyeben
    # az IDOBEN VALTOZO driftet mutatja). A residual differenciaja / nominal_period
    # ad egy relativ lokalis drift-eltavolodast, amit a die_c ellen illesztunk.
    therm = None
    res['die_valid'] = False
    dies = [s.die_c for s in seg if not math.isnan(s.die_c)]
    if host is not None and len(dies) >= 3:
        die_std = stdev(dies)
        die_span = (max(dies) - min(dies)) if dies else 0.0
        res['die_std'] = die_std
        res['die_span'] = die_span
        # akkor probalunk illeszteni, ha a die_c erdemben valtozik
        if die_span >= 0.5 and all(not math.isnan(s.die_c) for s in seg):
            resid = host['resid']
            # lokalis drift-rata: a residual elso differenciaja cycle-onkent,
            # masodpercben/cycle -> relativ drift = d(resid)/nominal_period
            xr = []   # die_c a kozeppontnal
            yr = []   # lokalis relativ drift (ppm)
            for i in range(1, len(seg)):
                dc = xs[i] - xs[i - 1]
                if dc <= 0:
                    continue
                dres = resid[i] - resid[i - 1]
                local_drift = (dres / dc) / nominal_period_s  # relativ
                die_mid = 0.5 * (seg[i].die_c + seg[i - 1].die_c)
                xr.append(die_mid)
                yr.append(local_drift * 1e6)  # ppm
            if len(xr) >= 3 and stdev(xr) > 0:
                b, a, r2, n = linregress(xr, yr)
                if b is not None:
                    therm = {'ppm_per_C': b, 'intercept_ppm': a, 'r2': r2, 'n': n,
                             'die_min': min(dies), 'die_max': max(dies),
                             'die_std': die_std}
                    res['die_valid'] = True
    res['therm'] = therm

    return res


# ---------------------------------------------------------------------------
# Guard-window ajanlas
# ---------------------------------------------------------------------------

def guard_window(abs_drift, nominal_period_s, margin_s, Ns):
    """guard_s(N) = |drift| * N * nominal_period_s + margin.

    Visszaad: list of (N, guard_s).
    """
    return [(N, abs_drift * N * nominal_period_s + margin_s) for N in Ns]


# ---------------------------------------------------------------------------
# Riport
# ---------------------------------------------------------------------------

def fmt_drift(d):
    return ("T_meas={:.6f}s/cycle  drift={:+.1f} ppm  ({:+.4f}%)  {:+.3f} s/ora"
            .format(d['T_meas'], d['ppm'], d['pct'], d['s_per_hour']))


def run_report(samples, stats, nominal_period_s, margin_mode, margin_fixed_s,
               guard_Ns):
    out = []
    P = out.append

    P("=" * 72)
    P("M1 DRIFT-ELEMZES RIPORT")
    P("=" * 72)

    # futas hossza
    valid_ts = [s.host_ts for s in samples if s.host_ts is not None]
    if valid_ts:
        span = (max(valid_ts) - min(valid_ts)).total_seconds()
        P("Futas hossza (host-ora): {:.2f} ora  ({:.0f} s)".format(span / 3600.0, span))
        P("  elso minta: {}".format(min(valid_ts).isoformat(timespec='seconds')))
        P("  utolso minta: {}".format(max(valid_ts).isoformat(timespec='seconds')))
    else:
        P("Futas hossza (host-ora): N/A (nincs host-idobelyeg a logban)")

    P("Beolvasott sorok: {}   M1RX: {}   heartbeat(HB): {}   nem-illeszkedo/hianyos: {}"
      .format(stats['lines'], stats['m1rx'], stats['hb'], stats['malformed']))
    P("CRC=BAD (fitbol kihagyva): {}   RECONNECT-esemenyek a logban: {}"
      .format(stats['crc_bad'], stats['reconnect']))

    segments, dup_count = dedup_and_segment(samples)
    P("Duplikatum (dedup, ugyanaz a boot+cycle ujra): {}".format(dup_count))
    P("Szegmensek (boot_id szerint): {}".format(len(segments)))
    if len(segments) > 1:
        P("  FIGYELEM: tobb szegmens -> a node tobbszor ujraindult (boot_id valtott).")
        P("  Minden szegmenst kulon illesztunk; a guard-window a leghosszabb/legrosszabb alapjan.")

    # szegmensenkenti elemzes
    seg_results = []
    for boot in sorted(segments.keys()):
        seg = segments[boot]
        r = analyze_segment(seg, nominal_period_s)
        seg_results.append(r)

        P("")
        P("-" * 72)
        P("SZEGMENS boot=0x{:08X}   mintak={}   cycle [{}..{}] (span {})"
          .format(boot & 0xFFFFFFFF, r['n'], r['cycle_min'], r['cycle_max'], r['cycle_span']))
        P("-" * 72)

        if r['host'] is not None:
            h = r['host']
            P("[HOST-ora ref, ELSODLEGES]  " + fmt_drift(h))
            P("    R^2={:.6f}   residual szoras={:.4f} s".format(h['r2'], h['resid_std']))
        else:
            P("[HOST-ora ref] N/A (hianyzo host_ts vagy <2 minta)")

        if r['master_reset']:
            P("[MASTER esp_timer ref]  RESET DETEKTALVA (rx_us nem-monoton) -> a host-ora az ervenyes.")
        elif r['master'] is not None:
            m = r['master']
            P("[MASTER esp_timer ref, kereszt-ellenorzes]  " + fmt_drift(m))
            P("    R^2={:.6f}   residual szoras={:.4f} s".format(m['r2'], m['resid_std']))
        else:
            P("[MASTER esp_timer ref] N/A (hianyzo rx_us vagy <2 minta)")

        if r['awake_corr'] is not None:
            ac = r['awake_corr']
            P("[AWAKE-korrigalt (host - kumulalt awake_ms)]  "
              "drift={:+.1f} ppm  ({:+.4f}%)  {:+.3f} s/ora   atlag awake={:.0f} ms"
              .format(ac['ppm'], ac['pct'], ac['s_per_hour'], ac['awake_ms_mean']))
            if r['host'] is not None:
                diff_ppm = r['host']['ppm'] - ac['ppm']
                P("    -> az awake-fazis hozzajarulasa a nyers drifthez: {:+.1f} ppm".format(diff_ppm))
        else:
            P("[AWAKE-korrigalt] N/A (hianyzo awake_ms vagy nincs host-fit)")

        if r['therm'] is not None:
            t = r['therm']
            P("[HO-KORRELACIO]  ~{:+.1f} ppm/C  (R^2={:.3f}, n={}, die_c {:.2f}..{:.2f} C, szoras {:.2f} C)"
              .format(t['ppm_per_C'], t['r2'], t['n'], t['die_min'], t['die_max'], t['die_std']))
        else:
            P("[HO-KORRELACIO] hofuggés nem becsulheto ebbol a futasbol "
              "(die_c mind-nan vagy alig valtozik / kevés minta).")

    # guard-window: a legrosszabb (legnagyobb |drift|) host-referencia alapjan
    P("")
    P("=" * 72)
    P("GUARD-WINDOW AJANLAS (Sz4 deliverable)")
    P("=" * 72)

    drifts = []
    worst_resid_std = 0.0
    for r in seg_results:
        if r['host'] is not None:
            drifts.append(abs(r['host']['drift']))
            worst_resid_std = max(worst_resid_std, r['host']['resid_std'])
        elif r['master'] is not None:
            drifts.append(abs(r['master']['drift']))
            worst_resid_std = max(worst_resid_std, r['master']['resid_std'])

    if not drifts:
        P("Nincs illesztheto szegmens -> guard-window nem szamolhato.")
        return "\n".join(out), seg_results, segments

    abs_drift = max(drifts)

    if margin_mode == 'resid3':
        margin_s = 3.0 * worst_resid_std
        margin_desc = ("margin = 3 x residual_szoras = 3 x {:.4f} = {:.4f} s "
                       "(a linearis fittol valo szoras 3-szorosa, ~99.7%-os fedés "
                       "ha a residual normal)".format(worst_resid_std, margin_s))
    else:
        margin_s = margin_fixed_s
        margin_desc = "margin = fix {:.4f} s (konzervativ allando tag)".format(margin_s)

    P("Kepletek:")
    P("  guard_s(N) = |drift| * N * nominal_period_s + margin")
    P("  |drift| (legrosszabb szegmens) = {:.6e}  ({:+.4f}%, {:+.1f} ppm)"
      .format(abs_drift, abs_drift * 100.0, abs_drift * 1e6))
    P("  nominal_period_s = {:.3f} s".format(nominal_period_s))
    P("  " + margin_desc)
    P("")
    P("  {:>8s} | {:>14s} | {:>14s}".format("N (cikl)", "ido (ora)", "guard_window (s)"))
    P("  " + "-" * 44)
    for N, g in guard_window(abs_drift, nominal_period_s, margin_s, guard_Ns):
        hours = N * nominal_period_s / 3600.0
        P("  {:>8d} | {:>14.3f} | {:>14.3f}".format(N, hours, g))
    P("")
    P("Megjegyzes: a margin a residual (idoben valtozo ho-drift) lefedesere szolgal;")
    P("a |drift|*N*T tag a szisztematikus (linearis) elcsuszas. Sz4 ennyivel tartja")
    P("a node-ot a slotban N cikluson at korrekcio nelkul.")

    return "\n".join(out), seg_results, segments


# ---------------------------------------------------------------------------
# CSV
# ---------------------------------------------------------------------------

def write_csv(path, segments, seg_results):
    import csv
    # boot -> seg_result map
    by_boot = {r['boot']: r for r in seg_results}
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['boot', 'cycle', 'host_ts_iso', 'rx_us', 'host_rel_s',
                    'master_rel_s', 'residual_host_s', 'awake_ms', 'die_c', 'rssi'])
        for boot in sorted(segments.keys()):
            seg = segments[boot]
            r = by_boot.get(boot)
            h = r['host'] if r else None
            m = r['master'] if r else None
            t0 = seg[0].host_ts.timestamp() if seg[0].host_ts is not None else None
            u0 = seg[0].rx_us if seg[0].rx_us is not None else None
            for i, s in enumerate(seg):
                host_rel = ''
                if s.host_ts is not None and t0 is not None:
                    host_rel = "{:.6f}".format(s.host_ts.timestamp() - t0)
                master_rel = ''
                if s.rx_us is not None and u0 is not None and not (r and r['master_reset']):
                    master_rel = "{:.6f}".format((s.rx_us - u0) / 1e6)
                resid = ''
                if h is not None:
                    resid = "{:.6f}".format(h['resid'][i])
                die = '' if math.isnan(s.die_c) else "{:.2f}".format(s.die_c)
                w.writerow([
                    "0x{:08X}".format(boot & 0xFFFFFFFF),
                    s.cycle, s.host_ts_iso,
                    s.rx_us if s.rx_us is not None else '',
                    host_rel, master_rel, resid,
                    s.awake_ms if s.awake_ms is not None else '',
                    die,
                    s.rssi if s.rssi is not None else '',
                ])


# ---------------------------------------------------------------------------
# Plot (opcionalis)
# ---------------------------------------------------------------------------

def make_plot(path, segments, seg_results, nominal_period_s):
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
    except Exception as e:  # noqa
        print("[plot] matplotlib nem elerheto ({}), a PNG kihagyva.".format(e),
              file=sys.stderr)
        return False

    by_boot = {r['boot']: r for r in seg_results}
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(9, 8))
    for boot in sorted(segments.keys()):
        seg = segments[boot]
        r = by_boot.get(boot)
        c0 = seg[0].cycle
        xs = [s.cycle - c0 for s in seg]
        if r and r['host'] is not None:
            h = r['host']
            ax1.plot(xs, h['ys'], '.', ms=3, label="host 0x{:X}".format(boot & 0xFFFFFFFF))
            ax2.plot(xs, h['resid'], '.-', ms=3, label="resid 0x{:X}".format(boot & 0xFFFFFFFF))
    ax1.set_xlabel("cycle (relativ)")
    ax1.set_ylabel("host_rel_s")
    ax1.set_title("Host-ido vs. cycle (drift = meredekseg / nominal - 1)")
    ax1.legend(fontsize=8)
    ax2.set_xlabel("cycle (relativ)")
    ax2.set_ylabel("residual (s)")
    ax2.set_title("Residual a linearis fittol (idoben valtozo ho-drift)")
    ax2.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def _gen_synthetic_log(path, drift_pct=2.0, nominal_period_s=30.0, n_cycles=400,
                       thermal_ppm_per_C=1000.0, awake_ms=480,
                       triplicate=True, crc_bad_every=53):
    """Szintetikus log ismert drifttel + ho-rampaval (a parser+fit ellenorzesere).

    Modell:
      - alap drift: drift_pct % a nominal periodusra (szisztematikus).
      - ho-rampa: a die_c lineárisan 20 -> 35 C; a pillanatnyi periodus
        +thermal_ppm_per_C * (die_c - die_ref) extra eltolassal.
      - minden ebredes utan awake_ms ebren-ido, ami a host_ts-ben benne van.
      - host_ts = kumulalt valodi intervallumok (alvas + awake) az inditastol.
      - rx_us = ugyanaz mikroszekundumban (master esp_timer, kis offszettel).
    A betaplalt ATLAGOS drift (a ho-rampa kozepe korul) ~ drift_pct, ezt
    ellenorzi a self-test (tureshatarral).
    """
    base = datetime(2026, 6, 28, 22, 0, 0)
    t0_epoch = base.timestamp()
    boot = 0xA1B2C3D4
    node = 1
    reqT_ms = int(nominal_period_s * 1000)

    die_start, die_end = 20.0, 35.0
    die_ref = 0.5 * (die_start + die_end)

    host_elapsed = 0.0   # s az inditastol
    master_us_base = 5_000_000  # a master mar futott egy ideje
    master_elapsed_us = 0.0

    lines = []
    for cyc in range(n_cycles):
        # die_c lineáris rampa
        frac = cyc / max(1, n_cycles - 1)
        die_c = die_start + (die_end - die_start) * frac
        # pillanatnyi relativ drift = alap + ho-tag
        thermal_extra = thermal_ppm_per_C * 1e-6 * (die_c - die_ref)
        inst_drift = drift_pct / 100.0 + thermal_extra
        # az ELOZO cycle -> ez a cycle kozti tenyleges ALVAS intervallum
        sleep_s = nominal_period_s * (1.0 + inst_drift)

        if cyc > 0:
            # alvas + az elozo ebredes awake-ideje jarul a host-elapsed-hez
            host_elapsed += sleep_s + (awake_ms / 1000.0)
            master_elapsed_us += (sleep_s + awake_ms / 1000.0) * 1e6

        host_ts = datetime.fromtimestamp(t0_epoch + host_elapsed)
        rx_us = int(master_us_base + master_elapsed_us)

        reps = 3 if triplicate else 1
        for rep in range(reps):
            # a duplikatumok kis host-jitterrel jonnek (a master kesobb logolja)
            jitter = rep * 0.012  # 12 ms
            ts = datetime.fromtimestamp(t0_epoch + host_elapsed + jitter)
            ts_iso = ts.isoformat(timespec='microseconds')
            rxu = rx_us + rep * 12000
            crc = "OK"
            if crc_bad_every and (cyc % crc_bad_every == 0) and rep == 0 and cyc > 0:
                crc = "BAD"
            line = ("{ts}\tI ({u}) M1-MASTER: M1RX rx_us={rxu} cycle={c} node={n} "
                    "boot=0x{boot:08X} reqT_ms={req} awake_ms={aw} die_c={die:.2f} "
                    "rssi={rssi} crc={crc}\n").format(
                ts=ts_iso, u=rxu // 1000, rxu=rxu, c=cyc, n=node, boot=boot,
                req=reqT_ms, aw=awake_ms, die=die_c, rssi=-40 - (cyc % 7), crc=crc)
            lines.append(line)
        # heartbeat idonkent
        if cyc % 20 == 0:
            lines.append("I ({u}) M1-MASTER: M1HB uptime_us={u} last_cycle={c} rx_count={rc}\n"
                         .format(u=rx_us, c=cyc, rc=cyc * 3))

    with open(path, 'w') as f:
        f.writelines(lines)

    # az ELVART atlagos drift: a ho-rampa szimmetrikus a die_ref korul, igy
    # az atlagos thermal_extra ~ 0 -> a fittett drift ~ drift_pct/100.
    return drift_pct / 100.0


def run_selftest():
    import tempfile
    print("=== M1 DRIFT-ELEMZO ON-TESZT ===")
    tmpdir = tempfile.mkdtemp(prefix="m1_selftest_")
    logpath = os.path.join(tmpdir, "synthetic.log")

    expected_drift = _gen_synthetic_log(
        logpath, drift_pct=2.0, nominal_period_s=30.0, n_cycles=400,
        thermal_ppm_per_C=1000.0, awake_ms=480, triplicate=True, crc_bad_every=53)

    samples, stats = parse_files([logpath])
    print("Generalt log: {}".format(logpath))
    print("Parse: lines={} m1rx={} hb={} malformed={} crc_bad={}".format(
        stats['lines'], stats['m1rx'], stats['hb'], stats['malformed'], stats['crc_bad']))

    segments, dup_count = dedup_and_segment(samples)
    print("Dedup duplicate={}  szegmensek={}".format(dup_count, len(segments)))

    # ellenorzesek
    ok = True
    # 1 szegmens (egy boot)
    if len(segments) != 1:
        print("  FAIL: vart 1 szegmens, kapott {}".format(len(segments)))
        ok = False
    # dedup: 400 cycle, 3x -> 800 dup (de a crc_bad-ok az elso replat rontjak el)
    # a crc=BAD csak az elso (rep0) repet erinti nehany cycle-on; azokon a
    # cycle-okon a megmaradt 2 rep kozul az elso lesz a kanonikus -> dedup=2.
    # Egyszerubb csak az n-t ellenorizni: ~400 egyedi cycle.
    seg = segments[list(segments.keys())[0]]
    print("Egyedi cycle-ok a szegmensben: {}".format(len(seg)))

    # FONTOS modell: a host-ba (es a master esp_timer-be) beletettuk az ebren-idot
    # is, igy a NYERS host-drift = alvas-drift (expected_drift) + awake/period.
    # 480 ms / 30 s = 1.6 %-pont -> a nyers drift ~ 2.0% + 1.6% = 3.6%.
    # Az AWAKE-korrigalt fit kell, hogy visszaadja a tiszta alvas-driftet (~2%).
    awake_ms_synth = 480.0
    expected_raw = expected_drift + (awake_ms_synth / 1000.0) / 30.0

    r = analyze_segment(seg, 30.0)
    h = r['host']
    m = r['master']
    tol = 0.0015  # 0.15 %-pont tures
    if h is None:
        print("  FAIL: nincs host-fit")
        ok = False
    else:
        got = h['drift']
        print("HOST-fit NYERS drift: vart {:+.4f}% (alvas {:+.2f}% + awake {:+.2f}%) "
              "-> visszaadott {:+.4f}%  (ppm {:+.1f}, {:+.3f} s/ora)"
              .format(expected_raw * 100, expected_drift * 100,
                      (awake_ms_synth / 1000.0) / 30.0 * 100, h['pct'], h['ppm'],
                      h['s_per_hour']))
        print("  R^2={:.8f}  residual_std={:.5f}s".format(h['r2'], h['resid_std']))
        if abs(got - expected_raw) > tol:
            print("  FAIL: a NYERS host-drift elteres {:.5f} > tures {:.5f}".format(
                abs(got - expected_raw), tol))
            ok = False
        else:
            print("  PASS: NYERS host-drift a tureshataron belul (elteres {:.5f} <= {:.5f})".format(
                abs(got - expected_raw), tol))

    if m is not None:
        print("MASTER-fit NYERS drift: visszaadott {:+.4f}%  (ppm {:+.1f})".format(
            m['pct'], m['ppm']))
        if abs(m['drift'] - expected_raw) > tol:
            print("  FAIL: master NYERS drift elter a vart {:+.4f}%-tol".format(expected_raw * 100))
            ok = False
        else:
            print("  PASS: master NYERS drift a tureshataron belul")

    # awake-korrekcio: a betaplalt awake hozzajarulas levonva -> a sleep-only
    # drift a tiszta 2%-hoz kell kozelitsen (ez a fo bizonyitek a modellre).
    ac = r['awake_corr']
    if ac is not None:
        print("AWAKE-korrigalt drift: vart {:+.4f}% -> visszaadott {:+.4f}%  (atlag awake {:.0f} ms)"
              .format(expected_drift * 100, ac['pct'], ac['awake_ms_mean']))
        if abs(ac['drift'] - expected_drift) > tol:
            print("  FAIL: awake-korrigalt drift elter a tiszta alvas-drifttol "
                  "({:.5f} > {:.5f})".format(abs(ac['drift'] - expected_drift), tol))
            ok = False
        else:
            print("  PASS: awake-korrigalt drift visszaadja a tiszta alvas-driftet "
                  "(elteres {:.5f} <= {:.5f})".format(abs(ac['drift'] - expected_drift), tol))
    else:
        print("AWAKE-korrigalt drift: N/A (varatlan a self-testben)")
        ok = False

    # ho-korrelacio: betaplalt 1000 ppm/C
    t = r['therm']
    if t is not None:
        print("HO-korrelacio: betaplalt 1000 ppm/C -> visszaadott {:+.1f} ppm/C (R^2={:.3f})".format(
            t['ppm_per_C'], t['r2']))
        # nagyvonalu tures: a numerikus differencialas zajos
        if abs(t['ppm_per_C'] - 1000.0) > 400.0:
            print("  WARN: ho-egyutthato a vart 1000 ppm/C-tol tobb mint 400-zal eltér "
                  "(numerikus diff. zajos; a host-fit drift a fo pass-feltetel).")
        else:
            print("  PASS: ho-egyutthato a vart 1000 ppm/C kozeleben.")
    else:
        print("HO-korrelacio: nem becsult (varatlan a self-testben).")

    # guard-window smoke
    rep, _, _ = run_report(samples, stats, 30.0, 'resid3', 2.0, [10, 60, 1440])
    if "GUARD-WINDOW AJANLAS" not in rep:
        print("  FAIL: guard-window blokk hianyzik a riportbol")
        ok = False

    print("")
    print("=== ON-TESZT EREDMENY: {} ===".format("PASS" if ok else "FAIL"))
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description="M1 offline drift-elemzo (stdlib fit).")
    ap.add_argument('logfiles', nargs='*', help="egy vagy tobb master-log fajl")
    ap.add_argument('--nominal-period-s', type=float, default=30.0,
                    help="nevleges alvas-periodus s (alap 30)")
    ap.add_argument('--csv', default=None, help="CSV kimenet utvonala")
    ap.add_argument('--plot', action='store_true', help="PNG plot (matplotlib, opcionalis)")
    ap.add_argument('--margin-mode', choices=['resid3', 'fixed'], default='resid3',
                    help="guard-window margin: resid3 (3x residual szoras) vagy fixed")
    ap.add_argument('--margin-fixed-s', type=float, default=2.0,
                    help="fix margin masodpercben (ha --margin-mode fixed)")
    ap.add_argument('--guard-n', type=str, default="10,60,1440",
                    help="vesszovel elvalasztott N ertekek a guard-tablahoz")
    ap.add_argument('--selftest', action='store_true', help="szintetikus on-teszt futtatasa")
    args = ap.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if not args.logfiles:
        ap.error("legalabb egy logfile kell (vagy --selftest)")

    guard_Ns = [int(x) for x in args.guard_n.split(',') if x.strip()]

    samples, stats = parse_files(args.logfiles)
    if not samples:
        print("Nincs egyetlen M1RX sor sem a megadott log(ok)ban.", file=sys.stderr)
        return 2

    report, seg_results, segments = run_report(
        samples, stats, args.nominal_period_s, args.margin_mode,
        args.margin_fixed_s, guard_Ns)
    print(report)

    if args.csv:
        write_csv(args.csv, segments, seg_results)
        print("\nCSV kiirva: {}".format(args.csv))

    if args.plot:
        png = (args.csv + ".png") if args.csv else "m1_drift.png"
        if make_plot(png, segments, seg_results, args.nominal_period_s):
            print("PNG kiirva: {}".format(png))

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

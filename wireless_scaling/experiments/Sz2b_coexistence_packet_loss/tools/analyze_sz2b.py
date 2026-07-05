#!/usr/bin/env python3
"""
analyze_sz2b.py — az Sz2b bench kiértékelése a MASTER logjából (ground truth).

Bemenet: a serial_logger.py által írt master-log (host-időbélyeges sorok).
Kimenet: node-onkénti kézbesítési statisztika (NEW/DUP/miss, try-eloszlás, gap-események),
WiFi-állapot összefoglaló (disconnect/reconnect, gateway-forgalom sikerarány), master-reboot
detektálás (banner-sorokból), és a PASS-feltételek gépi ellenőrzése.

Használat:
  python3 analyze_sz2b.py <master.log> [--since HH:MM:SS] [--until HH:MM:SS]

A --since/--until a host-időbélyegre szűr (pl. a flash-átmenet perceinek kizárására).
FIGYELEM: éjfél-átfordulást nem kezel (a bench-futások egy napon belüliek).
"""

import argparse
import re
import sys
from collections import defaultdict

RX_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d\.\d\d\d) .*SZ2B RX node=(?P<node>\d+) seq=(?P<seq>\d+) "
    r"try=(?P<try>\d+) kind=(?P<kind>NEW|DUP) gap=(?P<gap>\d+) rssi=(?P<rssi>-?\d+)")
STAT_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d\.\d\d\d) .*SZ2B STAT node=(?P<node>\d+) (?P<kv>.*)$")
MSTAT_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d\.\d\d\d) .*SZ2B MSTAT (?P<kv>.*)$")
WIFI_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d\.\d\d\d) .*SZ2B WIFI (?P<what>CONNECTED|GOT_IP|DISCONNECT|RECONNECT)(?P<rest>.*)$")
BANNER_RE = re.compile(
    r"^(?P<ts>\d\d:\d\d:\d\d\.\d\d\d) .*=== Sz2b MASTER")


def kv_parse(s):
    return dict(kv.split("=", 1) for kv in s.split() if "=" in kv)


def in_window(ts, since, until):
    if since and ts < since:
        return False
    if until and ts > until:
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile")
    ap.add_argument("--since", default=None, help="HH:MM:SS — ablak kezdete")
    ap.add_argument("--until", default=None, help="HH:MM:SS — ablak vége")
    args = ap.parse_args()

    nodes = defaultdict(lambda: {
        "new": 0, "dup": 0, "first_seq": None, "last_seq": None,
        "miss_events": [],          # (ts, seq, gap)
        "tries": defaultdict(int),  # try -> count (csak NEW; a kézbesítő próba sorszáma)
        "rssi_sum": 0, "rssi_n": 0, "rssi_min": 127, "rssi_max": -128,
        "ooo": 0,                   # out-of-order NEW (seq visszaugrás NEW-ként — nem várt)
    })
    banners = []
    wifi_events = defaultdict(list)   # what -> [ts]
    last_stat = {}
    last_mstat = None
    first_ts = last_ts = None

    with open(args.logfile, errors="replace") as f:
        for line in f:
            m = BANNER_RE.match(line)
            if m and in_window(m["ts"], args.since, args.until):
                banners.append(m["ts"])
                continue
            m = WIFI_RE.match(line)
            if m and in_window(m["ts"], args.since, args.until):
                wifi_events[m["what"]].append((m["ts"], m["rest"].strip()))
                continue
            m = MSTAT_RE.match(line)
            if m and in_window(m["ts"], args.since, args.until):
                last_mstat = (m["ts"], kv_parse(m["kv"]))
                continue
            m = STAT_RE.match(line)
            if m and in_window(m["ts"], args.since, args.until):
                last_stat[int(m["node"])] = (m["ts"], kv_parse(m["kv"]))
                continue
            m = RX_RE.match(line)
            if not m or not in_window(m["ts"], args.since, args.until):
                continue
            ts = m["ts"]
            first_ts = first_ts or ts
            last_ts = ts
            n = nodes[int(m["node"])]
            seq, gap, rssi = int(m["seq"]), int(m["gap"]), int(m["rssi"])
            if m["kind"] == "NEW":
                n["new"] += 1
                n["tries"][int(m["try"])] += 1
                if n["first_seq"] is None:
                    n["first_seq"] = seq
                if n["last_seq"] is not None and seq < n["last_seq"]:
                    n["ooo"] += 1
                n["last_seq"] = seq
                if gap:
                    n["miss_events"].append((ts, seq, gap))
            else:
                n["dup"] += 1
            n["rssi_sum"] += rssi
            n["rssi_n"] += 1
            n["rssi_min"] = min(n["rssi_min"], rssi)
            n["rssi_max"] = max(n["rssi_max"], rssi)

    print(f"=== Sz2b kiértékelés: {args.logfile} "
          f"[{args.since or 'eleje'} .. {args.until or 'vége'}] ===")
    print(f"RX-ablak: {first_ts} .. {last_ts}")
    if banners:
        print(f"master-boot bannerek az ablakban: {len(banners)} @ {', '.join(banners)}")

    for node_id in sorted(nodes):
        n = nodes[node_id]
        total_miss = sum(g for _, _, g in n["miss_events"])
        tries_total = sum(n["tries"].values())
        try_gt1 = sum(c for t, c in n["tries"].items() if t > 1)
        try_dist = " ".join(f"try{t}={c}" for t, c in sorted(n["tries"].items()))
        rssi_avg = n["rssi_sum"] / n["rssi_n"] if n["rssi_n"] else 0
        print(f"\n--- node {node_id} ---")
        print(f"  NEW={n['new']} DUP={n['dup']} ooo={n['ooo']} "
              f"seq: {n['first_seq']}..{n['last_seq']}")
        print(f"  MISS összesen: {total_miss} "
              f"({len(n['miss_events'])} gap-esemény)")
        for ts, seq, gap in n["miss_events"]:
            print(f"    {ts}  seq={seq} elött {gap} kimaradt")
        print(f"  try-eloszlás: {try_dist}  |  try>1: {try_gt1}/{tries_total} "
              f"({100.0*try_gt1/tries_total if tries_total else 0:.2f}%)")
        print(f"  RSSI: átlag {rssi_avg:.1f} dBm  (min {n['rssi_min']}, max {n['rssi_max']})")
        if node_id in last_stat:
            ts, kv = last_stat[node_id]
            print(f"  utolsó STAT ({ts}): miss={kv.get('miss')} dup={kv.get('dup')} "
                  f"ack={kv.get('ack')} ackfail={kv.get('ackfail')} resets={kv.get('resets')}")

    print("\n--- WiFi / koegzisztencia ---")
    for what in ("CONNECTED", "GOT_IP", "DISCONNECT", "RECONNECT"):
        evs = wifi_events.get(what, [])
        print(f"  {what}: {len(evs)}"
              + (f"  (utolsó: {evs[-1][0]} {evs[-1][1]})" if evs else ""))
    if last_mstat:
        ts, kv = last_mstat
        print(f"  utolsó MSTAT ({ts}): wifi={kv.get('wifi')} ch={kv.get('ch')} "
              f"ap_rssi={kv.get('ap_rssi')} q_drop={kv.get('q_drop')} "
              f"len_bad={kv.get('len_bad')} ver_bad={kv.get('ver_bad')} "
              f"disconn={kv.get('disconn')} got_ip={kv.get('got_ip')} "
              f"gw_ok={kv.get('gw_ok')} gw_fail={kv.get('gw_fail')} gw_skip={kv.get('gw_skip')}")
        gw_ok = int(kv.get("gw_ok", 0) or 0)
        gw_fail = int(kv.get("gw_fail", 0) or 0)
        if gw_ok + gw_fail:
            print(f"  gateway-forgalom sikerarány: {100.0*gw_ok/(gw_ok+gw_fail):.2f}% "
                  f"({gw_ok}/{gw_ok+gw_fail})")


if __name__ == "__main__":
    main()

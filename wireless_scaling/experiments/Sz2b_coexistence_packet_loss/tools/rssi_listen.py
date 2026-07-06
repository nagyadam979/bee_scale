#!/usr/bin/env python3
"""
rssi_listen.py — az rssi_probe UDP-broadcast beaconjeinek hallgatása (:5005).

MIRE JÓ: tápról futó probe-nál nincs serial; a telnet-hez pedig kell az IP. A probe
másodpercenként broadcastolja a PROBE-sorát (benne az IP-jével), így ez a script
serial nélkül is megtalálja és időbélyeggel logolja.

A Macnek a dlink-EDA4 hálózaton kell lennie!

Használat:
    python3 rssi_listen.py                      # csak stdout
    python3 rssi_listen.py -o logs/rssi_s3.log  # stdout + fájl
Távolság-jelölés mérés közben: üss Entert és írd be a távot (pl. "3m") — a sorba kerül.
(A jelölés stdin-ről megy, a vétel közben is él.)
"""

import argparse
import datetime
import select
import socket
import sys

PORT = 5005


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", help="log-fájl (append)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", PORT))

    out = open(args.out, "a") if args.out else None
    print(f"# hallgatás UDP :{PORT} — Ctrl-C a kilépés; táv-jelöléshez írj be pl. '3m' + Enter",
          file=sys.stderr)
    mark = ""
    try:
        while True:
            rl, _, _ = select.select([sock, sys.stdin], [], [])
            if sys.stdin in rl:
                text = sys.stdin.readline().strip()
                if text:
                    mark = text
                    note = f"# MARK dist={mark}"
                    print(note)
                    if out:
                        out.write(note + "\n")
            if sock in rl:
                data, addr = sock.recvfrom(512)
                ts = datetime.datetime.now().strftime("%H:%M:%S")
                line = f"{ts} {addr[0]} {data.decode(errors='replace').strip()}"
                if mark:
                    line += f" dist={mark}"
                print(line, flush=True)
                if out:
                    out.write(line + "\n")
                    out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()


if __name__ == "__main__":
    main()

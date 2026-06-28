#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M1 — robusztus ejszakai soros logger a master M1RX/M1HB sorainak rogzitesere.

Minden beolvasott sor ele tesz egy ISO8601 (mikroszekundumos) HOST-idobelyeget +
TAB, majd a nyers sor. Ir a fajlba (append, flush + idonkenti fsync) ES az
stdout-ra. A portnyitas/olvasas kulso `while True` hurokban try/except-tel:
USB-megszakadas / master-reset eseten '# RECONNECT <ts>' sort logol, var ~2 s,
es a VEGTELENSEGIG ujraprobalja ugyanabba a fajlba. Ctrl-C -> tiszta kilepes.

Fuggoseg: CSAK pyserial (`pip install pyserial`).

Hasznalat:
  python3 log_master.py [--port /dev/cu.usbserial-0001] [--baud 115200] [--out FILE]
Alap port: /dev/cu.usbserial-0001 (a master, CP2102 UART0). Alap baud: 115200.
Alap out: m1_capture_YYYYMMDD_HHMMSS.log a futtatas konyvtaraban.
"""

import sys
import os
import time
import argparse
from datetime import datetime

try:
    import serial  # pyserial
except ImportError:
    print("HIBA: a pyserial nincs telepitve. Telepites: pip install pyserial",
          file=sys.stderr)
    sys.exit(1)


RECONNECT_WAIT_S = 2.0      # ujracsatlakozas elotti varakozas
FSYNC_EVERY_S = 5.0         # ennyi masodpercenkent fsync (tapkimaradas-vedelem)
READ_TIMEOUT_S = 1.0        # soros olvasasi timeout (hogy a hurok ne ragadjon be)


def now_iso():
    return datetime.now().isoformat(timespec='microseconds')


def write_and_flush(fh, text, do_fsync=False):
    fh.write(text)
    fh.flush()
    if do_fsync:
        try:
            os.fsync(fh.fileno())
        except OSError:
            pass


def main(argv):
    ap = argparse.ArgumentParser(description="M1 robusztus ejszakai soros logger (pyserial).")
    ap.add_argument('--port', default='/dev/cu.usbserial-0001',
                    help="soros port (alap: /dev/cu.usbserial-0001, a master)")
    ap.add_argument('--baud', type=int, default=115200, help="baud (alap 115200)")
    ap.add_argument('--out', default=None,
                    help="kimeneti log fajl (alap: m1_capture_YYYYMMDD_HHMMSS.log)")
    args = ap.parse_args(argv)

    if args.out is None:
        args.out = "m1_capture_{}.log".format(datetime.now().strftime("%Y%m%d_%H%M%S"))

    header = (
        "# M1 master-log capture\n"
        "# parancs   : {cmd}\n"
        "# port      : {port}  @ {baud} baud\n"
        "# out-fajl  : {out}\n"
        "# kezdes    : {start}\n"
        "# formatum  : <ISO8601 host-ts>\\t<nyers master sor>\n"
        "# (a '# RECONNECT <ts>' sorok USB-megszakadast/master-resetet jeloznek)\n"
    ).format(
        cmd=" ".join([sys.executable, os.path.basename(__file__)] + argv),
        port=args.port, baud=args.baud, out=args.out, start=now_iso(),
    )

    # append modban nyitjuk (ujraindulaskor is ugyanabba a fajlba folytatunk)
    fh = open(args.out, 'a', buffering=1)  # line-buffered
    write_and_flush(fh, header, do_fsync=True)
    sys.stdout.write(header)
    sys.stdout.flush()

    last_fsync = time.monotonic()
    line_count = 0

    try:
        while True:  # KULSO ujracsatlakozasi hurok (a vegtelensegig)
            ser = None
            try:
                # No-auto-reset on open: a CP2102/CH340 a DTR/RTS vonalakon RESETELI a master
                # ESP-t portnyitaskor (-> esp_timer 0-ra ugrik + ~1 s vetel-kieses + ROM-boot
                # bináris zaj a logban). A portot NYITATLANUL hozzuk letre, a DTR/RTS-t False-ra
                # (deasszertalt: EN magas, IO0 magas = normal futas, NINCS reset/download-mod),
                # csak utana open(). Igy a master folyamatosan fut az ejszaka (monoton esp_timer).
                ser = serial.Serial()
                ser.port = args.port
                ser.baudrate = args.baud
                ser.timeout = READ_TIMEOUT_S
                ser.dsrdtr = False
                ser.rtscts = False
                try:
                    ser.dtr = False
                    ser.rts = False
                except (OSError, ValueError):
                    pass  # nem minden platform engedi open elott; az open utani beallitas a lenti
                ser.open()
                try:
                    ser.dtr = False
                    ser.rts = False
                except (OSError, ValueError):
                    pass
                msg = "# CONNECTED {} {}\n".format(now_iso(), args.port)
                write_and_flush(fh, msg, do_fsync=True)
                sys.stdout.write(msg)
                sys.stdout.flush()

                while True:  # BELSO olvasasi hurok
                    try:
                        raw = ser.readline()
                    except (serial.SerialException, OSError) as e:
                        raise e  # a kulso except kezeli (reconnect)
                    if not raw:
                        # timeout, nem jott sor -> idonkenti fsync, majd tovabb
                        nowm = time.monotonic()
                        if nowm - last_fsync >= FSYNC_EVERY_S:
                            try:
                                os.fsync(fh.fileno())
                            except OSError:
                                pass
                            last_fsync = nowm
                        continue

                    try:
                        text = raw.decode('utf-8', errors='replace').rstrip('\r\n')
                    except Exception:
                        text = repr(raw)

                    stamped = "{}\t{}\n".format(now_iso(), text)
                    fh.write(stamped)
                    fh.flush()
                    sys.stdout.write(stamped)
                    sys.stdout.flush()

                    line_count += 1
                    nowm = time.monotonic()
                    if nowm - last_fsync >= FSYNC_EVERY_S:
                        try:
                            os.fsync(fh.fileno())
                        except OSError:
                            pass
                        last_fsync = nowm

            except (serial.SerialException, OSError) as e:
                msg = "# RECONNECT {} port={} err={}\n".format(now_iso(), args.port, e)
                write_and_flush(fh, msg, do_fsync=True)
                sys.stdout.write(msg)
                sys.stdout.flush()
                time.sleep(RECONNECT_WAIT_S)
                # vissza a kulso hurok elejere -> ujra megprobaljuk a portnyitast
            finally:
                if ser is not None:
                    try:
                        ser.close()
                    except Exception:
                        pass

    except KeyboardInterrupt:
        msg = "# STOP {} (Ctrl-C, osszesen {} sor)\n".format(now_iso(), line_count)
        write_and_flush(fh, msg, do_fsync=True)
        sys.stdout.write("\n" + msg)
        sys.stdout.flush()
    finally:
        try:
            os.fsync(fh.fileno())
        except OSError:
            pass
        fh.close()

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""
serial_logger.py — resiliens háttér-logger USB-Serial-JTAG (CDC) portokhoz.

Miért kell: a C3/S3 natív USB-Serial-JTAG konzol CDC-eszköz — a node deep sleepnél
el is tűnik a /dev alól, majd ébredéskor újra enumerál. Ez a logger ezért:
  - végtelen ciklusban ÚJRANYITJA a portot, ha eltűnik/hibázik (a node awake-ablakait
    így hézagmentesen összegyűjti egy fájlba);
  - DTR/RTS-t LOW-ban tartja nyitáskor (nem rántja a boardot resetbe/bootloaderbe);
  - minden sort host-időbélyeggel ír a logfájlba (azonnali flush, tail -f barát);
  - a port-nyitás/vesztés eseményeit is a logba írja ([LOGGER] sorok).

Használat:
  python3 serial_logger.py <port> <logfile>
Leállítás: SIGTERM/SIGINT (kill), vagy a session vége.
FIGYELEM: flasheléshez a portot el kell engedni -> a flash_catch.sh előtt ezt a
loggert le kell állítani az adott portra (a macOS soros port exkluzív nyitású).
"""

import sys
import time
import datetime

import serial  # pyserial (a PlatformIO penv-jében kész)


def ts() -> str:
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def main() -> None:
    if len(sys.argv) != 3:
        print("usage: serial_logger.py <port> <logfile>", file=sys.stderr)
        sys.exit(2)
    port, logfile = sys.argv[1], sys.argv[2]

    with open(logfile, "a", buffering=1) as log:
        log.write(f"{ts()} [LOGGER] indul: {port}\n")
        while True:
            ser = None
            try:
                # RESET-MENTES nyitás (az esp-idf-monitor --no-reset receptje): a chip
                # USB-Serial-JTAG-je a (DTR=0, RTS=1) átmenetre resetel. Nyitás ELŐTT
                # mindkét vonal ASSERT (az (1,1) sosem resetel; a macOS driver amúgy is
                # ezt csinálja nyitáskor), nyitás UTÁN először az RTS-t, majd a DTR-t
                # engedjük el — így sosem megyünk át a reset-állapoton.
                ser = serial.Serial()
                ser.port = port
                ser.baudrate = 115200
                ser.timeout = 1.0
                ser.dtr = True
                ser.rts = True
                ser.open()
                ser.rts = False   # RTS elengedés ELŐSZÖR
                ser.dtr = False   # DTR utána -> nincs (DTR=0, RTS=1) átmenet
                log.write(f"{ts()} [LOGGER] port nyitva\n")
                buf = b""
                while True:
                    chunk = ser.read(4096)
                    if chunk:
                        buf += chunk
                        while b"\n" in buf:
                            line, buf = buf.split(b"\n", 1)
                            text = line.decode("utf-8", errors="replace").rstrip("\r")
                            log.write(f"{ts()} {text}\n")
            except (serial.SerialException, OSError) as e:
                # Csak az ÁTMENETET logoljuk (nyitva volt -> eltűnt); a sikertelen
                # újranyitási próbálkozásokat nem (deep sleep alatt percekig nincs port).
                if ser is not None and ser.is_open:
                    log.write(f"{ts()} [LOGGER] port eltunt: {e.__class__.__name__}\n")
                try:
                    if ser is not None:
                        ser.close()
                except Exception:
                    pass
                time.sleep(0.5)


if __name__ == "__main__":
    main()

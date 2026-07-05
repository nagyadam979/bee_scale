#!/usr/bin/env python3
"""
catch_bootloader.py — alvó C3 node elkapása: a port felbukkanásakor AZONNAL (~20 ms)
megnyitja, és beadja az esptool USBJTAGSerialReset DTR/RTS szekvenciáját -> a chip a ROM
download módban ragad (ott nem alszik el), utána a flash kényelmesen, verseny nélkül mehet.

Miért kell: a pio/esptool hidegindítása ~2-5 s, a node awake-ablaka ~1-1,5 s -> a sima
upload-retry ciklus rendre lecsúszik róla. Ez a script már fut, amikor a port megjelenik,
így az ablakon belül végez.

Használat: catch_bootloader.py <port> [timeout_s]
Kilépés: 0 = bootloaderben, 1 = timeout.
"""

import os
import sys
import time

import serial  # pyserial (a PlatformIO penv-jében kész)


def main() -> None:
    port = sys.argv[1]
    timeout_s = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0

    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not os.path.exists(port):
            time.sleep(0.02)
            continue
        ser = None
        try:
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = 115200
            ser.dtr = False
            ser.rts = False
            ser.open()
            # esptool USBJTAGSerialReset szekvencia (reset.py-ből, bit-hűen):
            ser.setRTS(False)
            ser.setDTR(False)          # Idle
            time.sleep(0.1)
            ser.setDTR(True)           # Set IO0 (boot strap)
            ser.setRTS(False)
            time.sleep(0.1)
            ser.setRTS(True)           # Reset — (1,1)-en át, nem (0,0)-n
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.2)
            ser.close()
            print("BOOTLOADER OK")
            sys.exit(0)
        except (serial.SerialException, OSError):
            # a port épp eltűnt a kezünk alól (elaludt) — próbáljuk a következő ablakban
            try:
                if ser is not None:
                    ser.close()
            except Exception:
                pass
            time.sleep(0.05)
    print("TIMEOUT: a port nem jelent meg / nem sikerult elkapni")
    sys.exit(1)


if __name__ == "__main__":
    main()

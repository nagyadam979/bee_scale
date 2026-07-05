#!/usr/bin/env python3
"""
usb_reset.py — SZÁNDÉKOS chip-reset egy C3/S3 USB-Serial-JTAG porton (4. fázis: master-reboot
negatív teszt). Az Sz6-ban feltárt mechanizmust használja fegyverként: a (DTR=0, RTS=1)
átmenet a chipet reseteli (rst:0x15 / USB_UART_CHIP_RESET).

FIGYELEM: a portot fogó serial_logger.py-t ELŐBB le kell állítani (exkluzív nyitás),
a reset után újraindítani.

Használat: python3 usb_reset.py <port>
"""

import sys
import time

import serial


def main():
    if len(sys.argv) != 2:
        print("usage: usb_reset.py <port>", file=sys.stderr)
        sys.exit(2)
    ser = serial.Serial()
    ser.port = sys.argv[1]
    ser.baudrate = 115200
    # Nyitás mindkét vonallal ASSERT-ben (nem resetel), majd a resetelő átmenet.
    ser.dtr = True
    ser.rts = True
    ser.open()
    ser.dtr = False   # (DTR=0, RTS=1) -> reset assert
    time.sleep(0.1)
    ser.rts = False   # reset elenged -> a chip újraindul
    ser.close()
    print(f"reset kiadva: {sys.argv[1]}")


if __name__ == "__main__":
    main()

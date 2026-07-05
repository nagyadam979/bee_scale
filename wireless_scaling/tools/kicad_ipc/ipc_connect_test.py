#!/usr/bin/env python3
"""Minimalis KiCad IPC API csatlakozas-teszt.

Cel: igazolni, hogy a `kipy` csomag a futo KiCad 9.0+ peldany IPC-szerverehez
csatlakozik, es alap infot vissza tud kerdezni.

Elofeltetelek:
  - Fut a KiCad GUI (9.0+).
  - Preferences -> Plugins -> "Enable IPC API server" bekapcsolva.
  - A kicad-python (import: kipy) telepitve ugyanabba a Python-kornyezetbe.

Futtatas:
  python3 ipc_connect_test.py
"""

import sys

from kipy import KiCad
from kipy.errors import ApiError, ConnectionError as KiCadConnectionError
from kipy.proto.common.types import DocumentType


def main() -> int:
    try:
        # Alapertelmezett socket-uton csatlakozik a futo KiCad IPC-szerverhez.
        kicad = KiCad(client_name="bee_scale-ipc-test")

        # 1) Kapcsolat elohuzasa – ha ez nem dob, a socket el.
        kicad.ping()

        # 2) KiCad verzio az API-n at.
        version = kicad.get_version()

        # 3) IPC API verzio.
        api_version = kicad.get_api_version()

    except (ApiError, KiCadConnectionError) as exc:
        print("[HIBA] Nem sikerult csatlakozni a KiCad IPC-szerverhez.")
        print(f"       Reszletek: {exc}")
        print()
        print("Ellenorizd:")
        print("  1. Fut-e a KiCad GUI (9.0+)?")
        print("  2. Preferences -> Plugins -> 'Enable IPC API server' be van-e kapcsolva?")
        print("  3. Az IPC bekapcsolasa utan ujra kell inditani a KiCad-et.")
        return 1
    except Exception as exc:  # noqa: BLE001 - diagnosztikahoz mindent elkapunk
        print(f"[HIBA] Varatlan hiba: {type(exc).__name__}: {exc}")
        return 1

    print("=== KiCad IPC csatlakozas OK ===")
    print(f"KiCad verzio (API):   {version}")
    print(f"IPC API verzio:       {api_version}")

    # Nyitott PCB dokumentumok lekerdezese. Opcionalis: kicad-python 0.7.1 +
    # KiCad 10 kombinacioban a GetOpenDocuments handler hianyozhat ("no handler
    # available"), ami nem a csatlakozas hibaja -- ezert kulon, toleransan kezeljuk.
    try:
        open_pcbs = kicad.get_open_documents(DocumentType.DOCTYPE_PCB)
        print(f"Nyitott PCB doc:      {len(open_pcbs)} db")
        for doc in open_pcbs:
            print(f"  - {doc.board_filename or doc}")
    except ApiError as exc:
        print(f"Nyitott PCB doc:      (nem lekerdezheto ezen a KiCad-verzion: {exc})")

    return 0


if __name__ == "__main__":
    sys.exit(main())

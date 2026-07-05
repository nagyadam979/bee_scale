#!/usr/bin/env python3
"""Meretaranyos 3D board-blokkok generalasa (build123d STEP) es rakotese a mar
felrakott node-modul footprintekre, a futo KiCad PCB Editor-ba (kipy IPC).

A footprinteket a make_node_modules.py hozta letre. Ez a script NEM rakja ujra
oket (a pozicioidat megorzi): beolvassa a meglevo footprinteket, mindegyikhez
hozzarendel egy STEP-modellt (kontur-meret x becsult magassag), es update_items-szel
frissiti. A 3D nezetben (Alt+3) a modulok igy meretaranyos blokkokent latszanak.

A magassagok BECSULTEK (tolomeros bemeres utan finomitando):
  ESP32-C3 SuperMini  4,5 mm (USB-C dominal)
  HX711-M             3,0 mm
  TP4056-PROT-USB-C   4,3 mm
  TPS63000-M          4,0 mm (2R2 induktor dominal)

Futtatas:
  python3 attach_module_models.py
"""

import glob
import os
import sys

MODEL_DIR = os.path.join(os.path.dirname(__file__), "..", "..",
                         "modeling", "PCB_modeling", "3dmodels")
MODEL_DIR = os.path.abspath(MODEL_DIR)

# value_field -> (step-fajlnev, Sz[X] mm, H[Y] mm, magassag[Z] mm)
MODELS = {
    "ESP32-C3 SuperMini": ("esp32c3.step", 18.0, 22.5, 4.5),
    "HX711-M":            ("hx711m.step",  16.0, 24.0, 3.0),
    "TP4056-PROT-USBC":   ("tp4056.step",  17.3, 29.0, 4.3),
    "TPS63000-M":         ("tps63000.step",16.0, 24.0, 4.0),
}


def generate_step_models():
    """A hianyzo STEP-blokkok legyartasa build123d-vel (box a z=0..H tartomanyban)."""
    from build123d import Box, Pos, export_step
    os.makedirs(MODEL_DIR, exist_ok=True)
    for fname, w, l, h in MODELS.values():
        path = os.path.join(MODEL_DIR, fname)
        # Box(X=w, Y=l, Z=h), origora centralva -> +h/2-vel a board fole emelve.
        solid = Pos(0, 0, h / 2) * Box(w, l, h)
        export_step(solid, path)
        print(f"  STEP: {fname}  ({w} x {l} x {h} mm)")


def find_pcbnew_socket():
    socks = sorted(glob.glob("/tmp/kicad/api-*.sock"))
    return ("ipc://" + socks[0]) if socks else None


def main() -> int:
    print(f"3D modellek konyvtara: {MODEL_DIR}")
    print("STEP-blokkok generalasa...")
    generate_step_models()

    from kipy import KiCad
    from kipy.errors import ApiError, ConnectionError as KiCadConnectionError
    from kipy.board_types import Footprint3DModel
    from kipy.geometry import Vector3D

    sock = find_pcbnew_socket()
    if not sock:
        print("[HIBA] Nincs pcbnew socket -- nyiss board-ot a PCB Editor-ban.")
        return 1

    try:
        k = KiCad(socket_path=sock, client_name="bee_scale-attach-3d")
        board = k.get_board()
        print(f"board: {board.name}")

        fps = board.get_footprints()
        to_update = []
        for fp in fps:
            value = fp.value_field.text.value
            if value not in MODELS:
                print(f"  (kihagyva: {value} -- nincs hozza modell)")
                continue
            fname = MODELS[value][0]
            path = os.path.join(MODEL_DIR, fname)

            m = Footprint3DModel()
            m.filename = path
            m.scale = Vector3D.from_xyz(1.0, 1.0, 1.0)   # STEP mm-ben, 1:1
            m.rotation = Vector3D.from_xyz(0.0, 0.0, 0.0)
            m.offset = Vector3D.from_xyz(0.0, 0.0, 0.0)
            m.visible = True
            m.opacity = 1.0

            # A meglevo modellek cserejе (idempotencia).
            del fp.definition.models[:]
            fp.definition.add_item(m)
            to_update.append(fp)

        if to_update:
            board.update_items(to_update)

        # Visszaolvasas.
        print(f"\nfrissitett footprintek: {len(to_update)}")
        for fp in board.get_footprints():
            models = fp.definition.models
            nm = len(models)
            fn = os.path.basename(models[0].filename) if nm else "-"
            print(f"  - {fp.value_field.text.value:22} modell:{nm} ({fn})")

    except (ApiError, KiCadConnectionError) as exc:
        print(f"[HIBA] KiCad IPC: {exc}")
        return 1

    print("\n3D modellek rakotve. Nezd: View -> 3D Viewer (Alt+3).")
    print("Ha nem frissul, zard be es nyisd ujra a 3D Viewer-t.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

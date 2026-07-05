#!/usr/bin/env python3
"""Meretaranyos 3D board-blokkok (FURATOKKAL) generalasa es rakotese a mar
felrakott node-modul footprintekre, a futo KiCad PCB Editor-ba (kipy IPC).

A modul-meretek es a furatok a KOZOS definiciobol jonnek (node_modules_def.py),
igy a 3D-furatok garantaltan szinkronban a hordozo-PCB padjaival.

A 3D blokkba MINDEN furat bekerul (solder ES mount is), hogy a modul valos
raszterlyukas kepet adja. (A hordozo-PCB-re a make_node_modules.py csak a solder
furatokat teszi -- a mount furatok oda nem kerulnek.)

A footprinteket NEM rakja ujra (a felhasznalo pakolasat megorzi): a meglevo
footprinteket update_items-szel frissiti a 3D modell rakotesehez.

Futtatas:
  python3 attach_module_models.py
"""

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from node_modules_def import MODULES  # noqa: E402

MODEL_DIR = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "..", "..", "modeling", "PCB_modeling", "3dmodels"))


def generate_step_models():
    """STEP-blokk modulonkent: box a z=0..h tartomanyban, MINDEN furat kivagva."""
    from build123d import Box, Cylinder, Pos, export_step
    os.makedirs(MODEL_DIR, exist_ok=True)
    for value, mod in MODULES.items():
        w, l, h = mod["w"], mod["l"], mod["h"]
        solid = Pos(0, 0, h / 2) * Box(w, l, h)
        for (_nm, x, y, drill, _pad, _kind) in mod["holes"]:
            # a furatot boven a blokk fole/ala nyujtjuk, hogy tisztan atvagjon
            solid -= Pos(x, y, h / 2) * Cylinder(radius=drill / 2, height=h + 2)
        path = os.path.join(MODEL_DIR, mod["step"])
        export_step(solid, path)
        nh = len(mod["holes"])
        print(f"  STEP: {mod['step']:14} ({w} x {l} x {h} mm, {nh} furat)")


def find_pcbnew_socket():
    socks = sorted(glob.glob("/tmp/kicad/api-*.sock"))
    return ("ipc://" + socks[0]) if socks else None


def main() -> int:
    print(f"3D modellek konyvtara: {MODEL_DIR}")
    print("STEP-blokkok generalasa (furatokkal)...")
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

        to_update = []
        for fp in board.get_footprints():
            value = fp.value_field.text.value
            if value not in MODULES:
                print(f"  (kihagyva: {value} -- nincs hozza definicio)")
                continue
            path = os.path.join(MODEL_DIR, MODULES[value]["step"])

            m = Footprint3DModel()
            m.filename = path
            m.scale = Vector3D.from_xyz(1.0, 1.0, 1.0)   # STEP mm-ben, 1:1
            m.rotation = Vector3D.from_xyz(0.0, 0.0, 0.0)
            m.offset = Vector3D.from_xyz(0.0, 0.0, 0.0)
            m.visible = True
            m.opacity = 1.0

            # Idempotencia: a meglevo 3D modelleket ki kell szurni a tenyleges
            # elem-listabol (a .models property csak masolatot ad vissza).
            d = fp.definition
            d._unwrapped_items = [it for it in d.items
                                  if not isinstance(it, Footprint3DModel)]
            d.add_item(m)
            to_update.append(fp)

        if to_update:
            board.update_items(to_update)

        print(f"\nfrissitett footprintek: {len(to_update)}")
        for fp in board.get_footprints():
            models = fp.definition.models
            fn = os.path.basename(models[0].filename) if models else "-"
            print(f"  - {fp.value_field.text.value:22} modell:{len(models)} ({fn})")

    except (ApiError, KiCadConnectionError) as exc:
        print(f"[HIBA] KiCad IPC: {exc}")
        return 1

    print("\n3D modellek rakotve (furatokkal). Nezd: View -> 3D Viewer (Alt+3).")
    print("Ha nem frissul, zard be es nyisd ujra a 3D Viewer-t.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

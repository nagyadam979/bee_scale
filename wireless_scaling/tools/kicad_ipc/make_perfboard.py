#!/usr/bin/env python3
"""Szokasos labkozolt (2,54 mm) furatos proba-panel (perfboard) generalasa a
futo KiCad PCB Editor nyitott board-jara, az IPC API-n (kipy) keresztul.

A rács egyetlen FootprintInstance-be kerul (sok PTH pad), a tabla-kontur pedig
negy BoardSegment az Edge.Cuts retegen. Idempotens: indulaskor letorli a board
osszes footprintjet es rajzolt alakzatat, majd ujraepiti a racsot.

Elofeltetel: fut a KiCad 9/10, IPC API bekapcsolva, ES nyitva van egy board a
PCB Editor-ban (a dokumentum-parancsok csak nyitott szerkesztovel elnek).

FONTOS tanulsag (miert kell a reteg): a FootprintInstance.layer-t be KELL
allitani (BL_F_Cu) -- kulonben a create_items latszolag sikerul (visszaad egy
elemet), de a KiCad csendben eldobja a footprintet.

Futtatas:
  python3 make_perfboard.py                # default 60x40 mm, 2,54 mm raszter
  python3 make_perfboard.py --w 50 --h 70  # egyedi meret
"""

import argparse
import sys

from kipy import KiCad
from kipy.errors import ApiError, ConnectionError as KiCadConnectionError
from kipy.board_types import (
    FootprintInstance, Pad, PadType, PadStackType, PadStackShape,
    DrillShape, BoardLayer, BoardSegment,
)
from kipy.geometry import Vector2


def make_tht_pad(number: int, x_mm: float, y_mm: float,
                 pad_d_mm: float, drill_d_mm: float) -> Pad:
    """Egy kor alaku, atmeno furatos (PTH) pad a megadott (footprint-relativ)
    pozicioban."""
    pad = Pad()
    pad.number = str(number)
    pad.pad_type = PadType.PT_PTH
    pad.position = Vector2.from_xy_mm(x_mm, y_mm)

    ps = pad.padstack
    ps.type = PadStackType.PST_NORMAL          # felveszi a BL_F_Cu copper layer-t

    fcu = ps.copper_layer(BoardLayer.BL_F_Cu)
    fcu.shape = PadStackShape.PSS_CIRCLE
    fcu.size = Vector2.from_xy_mm(pad_d_mm, pad_d_mm)

    ps.drill.shape = DrillShape.DS_CIRCLE
    ps.drill.diameter = Vector2.from_xy_mm(drill_d_mm, drill_d_mm)
    ps.drill.start_layer = BoardLayer.BL_F_Cu
    ps.drill.end_layer = BoardLayer.BL_B_Cu
    return pad


def make_edge(x1, y1, x2, y2) -> BoardSegment:
    """Egy szakasz az Edge.Cuts (tabla-kontur) retegen."""
    seg = BoardSegment()
    seg.start = Vector2.from_xy_mm(x1, y1)
    seg.end = Vector2.from_xy_mm(x2, y2)
    seg.layer = BoardLayer.BL_Edge_Cuts
    return seg


def build_perfboard(board, w_mm, h_mm, pitch_mm, pad_d_mm, drill_d_mm, margin_mm):
    # Rács pontszam: hany pad fer el a margokon belul.
    usable_x = w_mm - 2 * margin_mm
    usable_y = h_mm - 2 * margin_mm
    cols = int(usable_x // pitch_mm) + 1
    rows = int(usable_y // pitch_mm) + 1

    # Kozepre igazitott racs origója (bal felso pad kozeppontja).
    grid_w = (cols - 1) * pitch_mm
    grid_h = (rows - 1) * pitch_mm
    x0 = (w_mm - grid_w) / 2
    y0 = (h_mm - grid_h) / 2

    fp = FootprintInstance()
    fp.reference_field.text.value = "PERF1"
    fp.value_field.text.value = f"Perfboard_{pitch_mm}mm"
    fp.layer = BoardLayer.BL_F_Cu               # KOTELEZO (lasd modul-doc)
    fp.position = Vector2.from_xy_mm(0.0, 0.0)  # fp origo = board origo

    n = 1
    for iy in range(rows):
        for ix in range(cols):
            x = x0 + ix * pitch_mm
            y = y0 + iy * pitch_mm
            fp.definition.add_item(make_tht_pad(n, x, y, pad_d_mm, drill_d_mm))
            n += 1

    edges = [
        make_edge(0, 0, w_mm, 0),
        make_edge(w_mm, 0, w_mm, h_mm),
        make_edge(w_mm, h_mm, 0, h_mm),
        make_edge(0, h_mm, 0, 0),
    ]

    created = board.create_items(fp)
    created += board.create_items(edges)
    return cols, rows, cols * rows, created


def main() -> int:
    ap = argparse.ArgumentParser(description="Perfboard generalas KiCad IPC-n at")
    ap.add_argument("--w", type=float, default=60.0, help="tabla szelesseg [mm]")
    ap.add_argument("--h", type=float, default=40.0, help="tabla magassag [mm]")
    ap.add_argument("--pitch", type=float, default=2.54, help="raszter [mm]")
    ap.add_argument("--pad", type=float, default=1.8, help="pad atmero [mm]")
    ap.add_argument("--drill", type=float, default=1.0, help="furat atmero [mm]")
    ap.add_argument("--margin", type=float, default=3.0, help="szel-margo [mm]")
    args = ap.parse_args()

    try:
        k = KiCad(client_name="bee_scale-perfboard")
        board = k.get_board()

        # Idempotencia: tiszta lap.
        for group in (board.get_footprints(), board.get_shapes()):
            if group:
                board.remove_items(group)

        cols, rows, npads, created = build_perfboard(
            board, args.w, args.h, args.pitch, args.pad, args.drill, args.margin)

    except (ApiError, KiCadConnectionError) as exc:
        print(f"[HIBA] KiCad IPC: {exc}")
        print("Ellenorizd: fut-e a KiCad, be van-e kapcsolva az API, ES nyitva van-e egy board a PCB Editor-ban.")
        return 1

    print("=== Perfboard kesz ===")
    print(f"Tabla:      {args.w} x {args.h} mm")
    print(f"Raszter:    {args.pitch} mm | pad {args.pad} mm | furat {args.drill} mm")
    print(f"Racs:       {cols} x {rows} = {npads} furat")
    print(f"Letrehozva: {len(created)} board-elem (1 footprint + 4 kontur-szakasz)")
    print("A KiCad PCB Editor-ban most lathato. Mentes: Cmd+S a KiCad-ben.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

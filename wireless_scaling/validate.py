"""Gyors validátor: minden part 1 összefüggő solid + befoglaló méretek kiírása.
Futtatás: python validate.py
"""
import enclosure as e

parts = {
    "housing": e.build_housing(),
    "bottom_lid": e.build_bottom_lid(),
    "battery_door": e.build_battery_door(),
}
ok = True
for name, p in parts.items():
    n = len(p.solids())
    bb = p.bounding_box()
    sz = bb.size
    flag = "OK " if n == 1 else "!! "
    if n != 1:
        ok = False
    print(f"{flag}{name:14s} solids={n}  bbox=({sz.X:.1f} x {sz.Y:.1f} x {sz.Z:.1f})")
print("\nMINDEN 1 SOLID" if ok else "\n!!! VAN NEM-ÖSSZEFÜGGŐ PART !!!")

"""
layout.py — ALKATRÉSZ-PAKOLÁS vizualizáció a dobozon belül
==========================================================
Futtatás:  python layout.py   → a HÁZ (átlátszó) + alsó fedél + akku-ajtó + az alkatrész-
makettek az ocp-vscode viewerbe (port 3939). Headless is fut (a show try/except-ben van).

CÉL: lásd, hogyan férnek el / hol ütköznek az alkatrészek a házban, és PAKOLD őket a lenti
COMPONENTS táblában (csak a `pos` / `axis` mezőket állítgasd). Ha jó egy elrendezés, vidd át a
pozíciókat az enclosure.py-ba (pcb_nests, bat-strip, usb_ports, cella).

MÉRETEK: a HANDOFF.md / MEMORY.md spec-jeiből (befoglaló dobozok). ⚠️ A `h` (magasság = komponens-
burok a panel felett) BECSÜLT — a tényleges alkatrészen mérd le (forrasztott header/terminál sokat
számít, pl. a HX711 csavaros sorkapcsa magas). A `pos` a makett KÖZÉPPONTJA (x,y,z).

KOORDINÁTÁK (= enclosure.py): X rövid (60), Y hosszú (80, ide a holder), Z fel; alj z=0.
Az elektronika a −X térfélben (x<6), a 18650 a +X stripben (x>6), a cella a tető pocketjében.
"""
import enclosure as e
from build123d import Box, Cylinder, Pos, Rot, Align, Compound

C, MIN, MAX = Align.CENTER, Align.MIN, Align.MAX

# Hasznos referenciaszintek az enclosure.py-ból (a defaultok ezekhez igazítva):
PART_X      = e.inner_x / 2 - e.bat_w            # válaszfal belső lapja (elektronika x < PART_X)
STRIP_CX    = (PART_X + e.partition_th + e.inner_x / 2) / 2  # a 18650-strip HASZNOS közepe (válaszfal után)
SEAT_Z      = e.pcb_seat_z                       # a tető alá lógatott panelok FELSŐ lapja
POCKET_FLR  = e.housing_h - e.cell_recess_d      # a cella-pocket feneke (a cella alja ide ül)

# =========================================================================================
#  A L K A T R É S Z - T Á B L A   —   I T T   P A K O L J
# =========================================================================================
# Mezők:
#   name : felirat a viewerben
#   shape: "box" vagy "cyl"
#   size : box → (sx, sy, sz);  cyl → (átmérő, hossz)
#   pos  : a makett KÖZÉPPONTJA (x, y, z) [mm]
#   axis : csak cyl-nél: "x" / "y" / "z" (a henger tengelye)
#   color: viewer-szín
#
# A z-pozíciók defaultja a jelenlegi enclosure-elrendezésből (panelok a tető alá lógatva,
# 18650 az alján a stripben, cella a tető pocketjében). A panel-magasságok BECSÜLTEK.
PCB_TH = 1.6   # [mm] FR4 alaplap-vastagság — MINDEGYIK panelnél ezzel számolunk (komponens-burok nélkül)

COMPONENTS = [
    # --- ELEKTRONIKA (a −X térfélben, a tető alá lógatva: a panel FELSŐ lapja SEAT_Z-n) ---
    # A panelok csak az ALAPLAP-vastagsággal (PCB_TH). A komponensek (header, USB-C, sorkapocs)
    # NINCSENEK belemodellezve — ha kell a kilógásukkal számolni, külön add hozzá.
    # HX711 zöld generic: 34×21 panel
    dict(name="HX711",     shape="box", size=(34.0, 21.0, PCB_TH),
         pos=(-12.0, 0.0,  SEAT_Z - PCB_TH / 2),               color="#2ecc71"),
    # ESP32-C3 SuperMini: 24×18 panel
    dict(name="SuperMini", shape="box", size=(24.0, 18.0, PCB_TH),
         pos=(-14.0, 21.0, SEAT_Z - PCB_TH / 2),               color="#3498db"),
    # TP4056 USB-C: 29×17.3 panel
    dict(name="TP4056",    shape="box", size=(29.0, 17.3, PCB_TH),
         pos=(-14.0, -21.0, SEAT_Z - PCB_TH / 2),              color="#e67e22"),
    # DS18B20 TO-92 (~4.6×4.6×3.9) — diszkrét alkatrész, nem panel; placeholder a −X fal mellett
    dict(name="DS18B20",   shape="box", size=(4.6, 4.6, 3.9),
         pos=(-27.0, 0.0, 3.0),                                color="#e74c3c"),

    # --- AKKU (a +X stripben, a TETŐ ALÁ lógatva — NEM a bottom_lid-en!) ---
    # A holder a tető plató aljára kerül (felső lapja az inner_z-n), így z[18.3,36] sávban van,
    # a sarok-csavar-bossok (z[0,14]) FÖLÖTT → nincs ütközés a csavarokkal.
    # ⚠️ A nyitott/behelyező oldala KIFELÉ (a +X akku-ajtó felé) néz → a cellát a száj felől
    #    told be. (Box-makett: az orientációt a tényleges holdernél a száj +X felé fordítva értsd.)
    # 18650 holder befoglaló: 21.5 (X) × 74.5 (Y) × 17.7 (Z); felső lap az inner_z-n:
    dict(name="18650_holder", shape="box", size=(21.5, 74.5, 17.7),
         pos=(STRIP_CX, 0.0, e.inner_z - 17.7 / 2),            color="#9b59b6"),
    # 18650 cella: Ø18.5 × 65, a holderben fekve (∥ Y), ugyanazon a magasságon:
    dict(name="18650_cell",   shape="cyl", size=(18.5, 65.0),
         pos=(STRIP_CX, 0.0, e.inner_z - 17.7 / 2), axis="y",  color="#8e44ad"),

    # --- MÉRŐCELLA (a tető pocketjében; az alja a pocket fenekén, a dóm felfelé) ---
    # Egyszerűsített befoglaló 34×34×7.8 (a dóm/perem-tagolás nélkül)
    dict(name="load_cell", shape="box", size=(34.0, 34.0, 7.8),
         pos=(0.0, 0.0, POCKET_FLR + 7.8 / 2),                 color="#f1c40f"),
]


def _build_component(c):
    if c["shape"] == "box":
        sx, sy, sz = c["size"]
        solid = Box(sx, sy, sz, align=(C, C, C))
    else:  # cyl
        dia, length = c["size"]
        solid = Cylinder(dia / 2, length, align=(C, C, C))
        rot = {"x": Rot(0, 90, 0), "y": Rot(90, 0, 0), "z": Rot(0, 0, 0)}[c.get("axis", "z")]
        solid = rot * solid
    return Pos(*c["pos"]) * solid


def main():
    housing = e.build_housing()
    bottom_lid = e.build_bottom_lid()
    battery_door = e.build_battery_door()
    door_assembled = Pos(e.outer_x / 2 - e.bat_recess_d, 0, e.inner_z / 2) * Rot(0, 90, 90) * battery_door

    comps = [(c["name"], c["color"], _build_component(c)) for c in COMPONENTS]

    # Egyszerű ütközés-jelzés a konzolra (befoglaló-átfedés alkatrész-párokra):
    def bb(s):
        b = s.bounding_box(); return (b.min.X, b.min.Y, b.min.Z, b.max.X, b.max.Y, b.max.Z)
    boxes = [(n, bb(s)) for (n, _, s) in comps]
    print("Alkatrész befoglalók (x,y,z mm):")
    for n, _, s in comps:
        bx = s.bounding_box(); sz = bx.size
        print(f"  {n:14s} {sz.X:5.1f} × {sz.Y:5.1f} × {sz.Z:5.1f}  @ z[{bx.min.Z:5.1f},{bx.max.Z:5.1f}]")
    # Szándékosan egymásban lévő párok (nem ütközés): a 18650 cella a holderben.
    IGNORE = {frozenset({"18650_holder", "18650_cell"})}
    print("Lehetséges ÜTKÖZÉSEK (befoglaló-átfedés):")
    hit = False
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            n1, a = boxes[i]; n2, b = boxes[j]
            if frozenset({n1, n2}) in IGNORE:
                continue
            if (a[0] < b[3] and a[3] > b[0] and a[1] < b[4] and a[4] > b[1] and a[2] < b[5] and a[5] > b[2]):
                print(f"  ⚠️  {n1}  ×  {n2}"); hit = True
    if not hit:
        print("  nincs befoglaló-átfedés (a 18650 cella↔holder kihagyva).")

    # --- Viewer (ház átlátszón, hogy lásd az alkatrészeket) ---
    try:
        import os
        from ocp_vscode import show, set_port
        if not os.environ.get("OCP_PORT"):
            set_port(3939)
        objs = [housing, bottom_lid, door_assembled] + [s for (_, _, s) in comps]
        names = ["housing", "bottom_lid", "battery_door"] + [n for (n, _, _) in comps]
        colors = ["#9aa0a6", "#4f8cff", "#ff9f43"] + [col for (_, col, _) in comps]
        alphas = [0.25, 0.35, 0.5] + [0.9] * len(comps)
        show(*objs, names=names, colors=colors, alphas=alphas)
        print("[ocp_vscode] layout frissítve.")
    except Exception as ex:
        print(f"[ocp_vscode] viewer nem elérhető (headless?): {ex}")


if __name__ == "__main__":
    main()

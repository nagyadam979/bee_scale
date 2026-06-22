"""
bee_scale — kültéri, elemes IoT szenzoregység (méhkaptár-mérleg node) PARAMETRIKUS HÁZA
=========================================================================================

Anyag: ASA (kültéri, UV-álló). Ez CSAK megjegyzés — a geometriát nem érinti.
Védelmi cél: ~IP65 (eső + por): körbefutó tömítőhornyok, PG7 kábeltömszelencék,
nyitott rés nincs.

ARCHITEKTÚRA-DÖNTÉSEK (átgondolt, dokumentált — szabadon átírható):
-------------------------------------------------------------------
* A doboz ALUL NYITOTT. A teteje tömör, merev plató: ide kerül FELÜLRE a single-point
  mérőcella (HBS-SCL-50, 50 kg). Felül nincs nyílás → felülről nem szivárog be a víz.
* Szerelés ALULRÓL: a FŐ FEDÉL = az alsó lap (bottom_lid). A PCB-tartóoszlopok ezen
  az alsó fedélen állnak (ez a feladat "mounting boss az alján" kitétele). Beépíted az
  elektronikát a fedélre, majd felcsavarozod a házra.
* BELSŐ VÁLASZFAL a +X oldalon leválaszt egy 18650-rekeszt. Ehhez OLDALSÓ AKKU-AJTÓ ad
  hozzáférést (a +X falon), így a cella a fő elektronika bolygatása nélkül cserélhető.
* PG7 tömszelencék az ELÜLSŐ (−Y) falon (mérőcella-kábel, DS18B20-kábel).

KOORDINÁTA-RENDSZER:
  X = bal–jobb (inner_x), Y = elöl–hátul (inner_y), Z = függőleges (inner_z), Z felfelé.
  A ház KÜLSŐ ALJA a z = 0 síkon; a doboz felfelé épül. XY-ban az origóra központosítva.

A 3 nyomtatandó alkatrész:
  1) housing       — falak + tömör tető plató (bordákkal + mérőcella-furatképpel),
                     válaszfal, tömszelence-furatok, akku-ajtó nyílás + betét-bossok,
                     alsó-zárás tömítőhorony + sarok-bossok.
  2) bottom_lid    — alsó fedéllap, körbefutó tömítőhoronyhoz illeszkedő sík felület,
                     4 sarok átmenő csavarfurat, FELFELÉ álló PCB-tartóoszlopok.
  3) battery_door  — akku-ajtó lap, 4 sarok átmenő csavarfurat.

A slicer dolga (NEM itt állítjuk): kitöltés %, rétegmagasság, support.

build123d 0.11.0 (algebra API) + ocp_vscode élő viewer.
"""

from build123d import (
    Box, Cylinder, Pos, Rot, Align, Compound,
    export_stl, export_step,
)
from pathlib import Path

# =========================================================================================
#  M I N D E N   M É R E T   I T T   —   jól kommentált, hangolható paraméterek
# =========================================================================================

# ---- Illesztési hézag az összeillő részeknél ----
fit_gap          = 0.3    # [mm] összeillő alkatrészek közti rés (fedél/ajtó vs. ház)

# ---- Falvastagság / belméret ----
wall             = 3.0    # [mm] oldalfal-vastagság
top_th           = 5.0    # [mm] FELSŐ plató vastagsága — vastagabb a falnál (terhelést visz!)
inner_x          = 100.0  # [mm] belső szélesség  (bal–jobb)
inner_y          = 80.0   # [mm] belső mélység    (elöl–hátul)
inner_z          = 55.0   # [mm] belső magasság   (függőleges, tiszta belmagasság)

# ---- Heat-set menetes betétek (sárgaréz, M5) ----
# M5 sárgaréz heat-set betéthez kezdő furat ~6.4 mm (insert_dia). Az adatlap szerint hangold.
insert_dia       = 6.4    # [mm] heat-set betét furatátmérő (M5 sárgaréz)
insert_depth     = 9.0    # [mm] betétfurat mélység
screw_clear_dia  = 5.5    # [mm] M5 átmenő (clearance) furat a fedélben/ajtóban
boss_dia         = insert_dia + 2 * 2.6   # [mm] csavar-boss külső átmérő (elég falvastagság a betét köré)

# ---- Sarok csavar-bossok (fő fedél + akku-ajtó közös rendszere) ----
boss_h           = 22.0   # [mm] alsó-fedél sarok-bossok magassága (a z=0 síktól felfelé)
corner_inset     = 9.0    # [mm] sarok-boss középpont behúzása a BELSŐ faltól

# ---- FELSŐ PLATÓ: mérőcella (HBS-SCL-50, 50 kg) FELÜLRE szerelve ----------------------
# !!! PLACEHOLDER FURATKÉP !!! A HBS-SCL-50 pontos furatosztását MÉG NEM ismerjük.
# Amint megvan az adatlap, CSAK az alábbi 3 értéket írd át. A minta a plató közepére esik.
# Tipikus single-point cella: 2 furat a "fix" végen + 2 a "terhelt" végen → 2x2 téglalap.
cell_hole_spacing_x = 40.0   # [mm] PLACEHOLDER — furatosztás X (bal–jobb)
cell_hole_spacing_y = 15.0   # [mm] PLACEHOLDER — furatosztás Y (elöl–hátul)
cell_hole_dia       = 5.5    # [mm] PLACEHOLDER — M5 átmenő furat a cella rögzítéséhez
# A furatkép = (x,y) eltolások a plató közepétől. Alapból 2x2 téglalap a fenti osztásból.
# Ha a celládnak más a mintája (pl. 1 sor 2 furat), CSAK ezt a listát írd át:
cell_hole_pattern = [
    (-cell_hole_spacing_x / 2, -cell_hole_spacing_y / 2),
    ( cell_hole_spacing_x / 2, -cell_hole_spacing_y / 2),
    (-cell_hole_spacing_x / 2,  cell_hole_spacing_y / 2),
    ( cell_hole_spacing_x / 2,  cell_hole_spacing_y / 2),
]
# Megemelt rögzítő pad a furatkép körül (merevség + sík felfekvés a cellának):
cell_pad_margin   = 12.0  # [mm] a pad pereme a szélső furatokon túl
cell_pad_h        = 3.0   # [mm] a pad kiemelkedése a plató felett

# ---- Terhelést hordó MEREVÍTŐ BORDÁK a plató ALATT (~100 kg függőleges terhelés) ------
# Placeholder rács; a PCB-pozíciók ismeretében finomítsd, hogy ne ütközzön panellel.
rib_th           = 3.0    # [mm] borda vastagság
rib_drop         = 12.0   # [mm] mennyire lóg le a borda a tető alól a belső térbe
rib_x_positions  = [-25.0, 25.0]   # [mm] X-irányú bordák Y-pozíciói (a tető alatt végigfutnak X-ben)
rib_y_positions  = [-20.0, 20.0]   # [mm] Y-irányú bordák X-pozíciói

# ---- Körbefutó tömítőhorony (szilikon tömítéshez) — alsó zárás ÉS akku-ajtó ----------
gasket_groove_w  = 2.5    # [mm] tömítőhorony szélesség
gasket_groove_d  = 2.0    # [mm] tömítőhorony mélység

# ---- PG7 kábeltömszelencék (elülső −Y fal) -------------------------------------------
gland_hole_dia   = 12.5   # [mm] PG7 tömszelence furat
gland_count      = 3      # [db] tömszelence-furatok száma
gland_z          = 18.0   # [mm] furatok magassága a z=0 alaptól
gland_spacing    = 20.0   # [mm] furatok közti X-osztás (a −Y fal közepére rendezve)

# ---- 18650 akku-rekesz (+X oldal, válaszfallal leválasztva) ---------------------------
bat_w            = 26.0   # [mm] az akku-rekesz X-szélessége (a belső +X faltól befelé)
                          #      hasznos belső szélesség ≈ bat_w − wall (a válaszfal levonva)
# Akku-ajtó NYÍLÁS a +X falon (a rekeszbe néz):
bat_open_y       = 50.0   # [mm] ajtónyílás szélessége Y-ban
bat_open_z       = 38.0   # [mm] ajtónyílás magassága Z-ben
bat_door_overlap = 8.0    # [mm] az ajtólap túlnyúlása a nyíláson (tömítés-felfekvés + boss-ok)
door_th          = 4.0    # [mm] akku-ajtó lapvastagság
bat_boss_inset   = 5.0    # [mm] akku-ajtó sarok-boss behúzása az ajtó szélétől

# ---- PCB tartóoszlopok az ALSÓ FEDÉLEN (PLACEHOLDER pozíciók) -------------------------
# Heat-set betét M3 standoffhoz (kisebb, mint az M5 burkolatcsavar).
pcb_insert_dia    = 4.0   # [mm] M3 heat-set betét furat
pcb_insert_depth  = 6.0   # [mm] betétfurat mélység a standoffban
pcb_boss_dia      = 7.0   # [mm] standoff külső átmérő
pcb_boss_h        = 6.0   # [mm] standoff magasság (emeli a panelt a fedél fölé)
# PLACEHOLDER panel-furatképek (a fedél belsejére, z=0 sík fölött). Írd át a tényleges
# panelméretek szerint. Origó = doboz közepe; az elektronika a −X / közép térfélen ül,
# az akku-rekesz a +X oldalon van (oda ne tegyél PCB-bosst).
#   ESP32-C3 SuperMini furatkép ~ 22.5 x 18 mm  (placeholder)
#   HX711-M panel furatkép      ~ 28 x 14 mm     (placeholder)
pcb_boss_positions = [
    # ESP32-C3 SuperMini (4 sarok) — bal hátsó sarok köré
    (-38.0,  22.0), (-15.5,  22.0), (-38.0,   4.0), (-15.5,   4.0),
    # HX711-M (4 sarok) — bal elülső sarok köré
    (-36.0, -26.0), (-8.0,  -26.0), (-36.0, -12.0), (-8.0,  -12.0),
]

# =========================================================================================
#  S Z Á R M A Z T A T O T T   ( ne állítsd kézzel )
# =========================================================================================
outer_x   = inner_x + 2 * wall          # külső szélesség
outer_y   = inner_y + 2 * wall          # külső mélység
housing_h = inner_z + top_th            # ház teljes magassága (alja nyitott, teteje plató)
eps       = 0.5                         # [mm] boolean-tisztasághoz extra túllógás

# A sarok-boss középpontok (a belső sarkoktól behúzva) — fő fedél rögzítéséhez:
corner_positions = [
    ( inner_x / 2 - corner_inset,  inner_y / 2 - corner_inset),
    (-inner_x / 2 + corner_inset,  inner_y / 2 - corner_inset),
    ( inner_x / 2 - corner_inset, -inner_y / 2 + corner_inset),
    (-inner_x / 2 + corner_inset, -inner_y / 2 + corner_inset),
]

# Tömítőhorony középvonalának fél-kiterjedése (a falközépen fut körbe):
groove_half_x = inner_x / 2 + wall / 2
groove_half_y = inner_y / 2 + wall / 2

C = Align.CENTER
MIN = Align.MIN
MAX = Align.MAX


# =========================================================================================
#  1)  F Ő   H Á Z   ( housing )
# =========================================================================================
def build_housing():
    # --- Alapdoboz: külső tömör test, alja z=0-n, teteje z=housing_h ---
    outer = Box(outer_x, outer_y, housing_h, align=(C, C, MIN))

    # --- Belső üreg: alul NYITOTT (z=−eps..inner_z), felül marad a top_th plató ---
    cavity = Pos(0, 0, -eps) * Box(inner_x, inner_y, inner_z + eps, align=(C, C, MIN))
    housing = outer - cavity

    # --- Belső VÁLASZFAL: a +X oldalon leválasztja a 18650-rekeszt ---
    # A válaszfal belső (−X felőli) lapja x = inner_x/2 − bat_w-nél; vastagsága 'wall'.
    part_x = inner_x / 2 - bat_w
    partition = Pos(part_x, 0, 0) * Box(wall, inner_y, inner_z, align=(MIN, C, MIN))
    housing += partition

    # --- Merevítő BORDÁK a plató alatt (lelógnak a tetőből a belső térbe) ---
    # X-irányú bordák (Y-pozíciókon), a tető alól rib_drop mélyen:
    for y in rib_x_positions:
        housing += Pos(0, y, inner_z) * Box(inner_x, rib_th, rib_drop, align=(C, C, MAX))
    # Y-irányú bordák (X-pozíciókon) — csak az elektronikai térfélen, az akku-rekeszt kihagyva:
    for x in rib_y_positions:
        if x < part_x:  # ne lógjon bele az akku-rekeszbe
            housing += Pos(x, 0, inner_z) * Box(rib_th, inner_y, rib_drop, align=(C, C, MAX))

    # --- FELSŐ PLATÓ: megemelt mérőcella-pad + furatkép (PLACEHOLDER) ---
    xs = [p[0] for p in cell_hole_pattern]
    ys = [p[1] for p in cell_hole_pattern]
    pad_x = (max(xs) - min(xs)) + 2 * cell_pad_margin
    pad_y = (max(ys) - min(ys)) + 2 * cell_pad_margin
    cx = (max(xs) + min(xs)) / 2
    cy = (max(ys) + min(ys)) / 2
    housing += Pos(cx, cy, housing_h) * Box(pad_x, pad_y, cell_pad_h, align=(C, C, MIN))
    # Átmenő furatok a cella rögzítéséhez (a padon + platón át, felülről lefelé):
    for (hx, hy) in cell_hole_pattern:
        housing -= Pos(hx, hy, housing_h + cell_pad_h) * Cylinder(
            cell_hole_dia / 2, top_th + cell_pad_h + 2 * eps, align=(C, C, MAX)
        )

    # --- PG7 tömszelence-furatok az ELÜLSŐ (−Y) falon (vízszintes furatok, tengely Y) ---
    front_y = -(inner_y / 2 + wall / 2)   # a −Y fal középsíkja
    start_x = -(gland_count - 1) * gland_spacing / 2
    for i in range(gland_count):
        gx = start_x + i * gland_spacing
        # Rot(−90,0,0): a henger Z-tengelye → Y-tengely; hossz 2*wall a falon át:
        housing -= Pos(gx, front_y, gland_z) * Rot(-90, 0, 0) * Cylinder(
            gland_hole_dia / 2, 2 * wall + 2 * eps, align=(C, C, C)
        )

    # --- AKKU-AJTÓ nyílás a +X falon (a rekeszbe néz) ---
    face_x = outer_x / 2                  # +X külső fal
    housing -= Pos(face_x, 0, inner_z / 2) * Box(
        2 * wall + 2 * eps, bat_open_y, bat_open_z, align=(C, C, C)
    )

    # --- AKKU-AJTÓ tömítőhorony a +X KÜLSŐ falfelületen (téglalap-gyűrű, befelé maratva) ---
    # A horony a nyílás körül, gasket_groove_w szélesen, gasket_groove_d mélyen (−X felé).
    g_out_y = bat_open_y + 2 * (bat_door_overlap - gasket_groove_w)   # gyűrű külső Y
    g_out_z = bat_open_z + 2 * (bat_door_overlap - gasket_groove_w)   # gyűrű külső Z
    g_in_y  = g_out_y - 2 * gasket_groove_w
    g_in_z  = g_out_z - 2 * gasket_groove_w
    groove_outer = Pos(face_x, 0, inner_z / 2) * Box(2 * gasket_groove_d, g_out_y, g_out_z, align=(MAX, C, C))
    groove_inner = Pos(face_x, 0, inner_z / 2) * Box(2 * gasket_groove_d, g_in_y, g_in_z, align=(MAX, C, C))
    housing -= (groove_outer - groove_inner)

    # --- AKKU-AJTÓ sarok-bossok a +X fal mögött (betétfurat kívülről, −X felé) ---
    by = bat_open_y / 2 + bat_door_overlap - bat_boss_inset
    bz = bat_open_z / 2 + bat_door_overlap - bat_boss_inset
    for sy in (-1, 1):
        for sz in (-1, 1):
            bpos = (inner_x / 2, sy * by, inner_z / 2 + sz * bz)
            # Boss: henger X-tengellyel, a belső +X faltól BEFELÉ a kavitásba (−X).
            # Rot(0,90,0): lokál +Z → +X; az align=MAX a lokál −Z felé húz → globál −X.
            housing += Pos(*bpos) * Rot(0, 90, 0) * Cylinder(boss_dia / 2, boss_h, align=(C, C, MAX))
            # Betétfurat: a +X KÜLSŐ felülettől befelé (−X) a bossba:
            housing -= Pos(outer_x / 2, sy * by, inner_z / 2 + sz * bz) * Rot(0, 90, 0) * Cylinder(
                insert_dia / 2, insert_depth, align=(C, C, MAX)
            )

    # --- FŐ (alsó) ZÁRÁS: körbefutó tömítőhorony az ALSÓ peremfelületen (z=0, felfelé) ---
    groove_out = Box(2 * groove_half_x + gasket_groove_w, 2 * groove_half_y + gasket_groove_w,
                     gasket_groove_d, align=(C, C, MIN))
    groove_in  = Box(2 * groove_half_x - gasket_groove_w, 2 * groove_half_y - gasket_groove_w,
                     gasket_groove_d, align=(C, C, MIN))
    housing -= (groove_out - groove_in)

    # --- FŐ FEDÉL sarok-bossok a belső sarkokban (betétfurat alulról, z=0-tól felfelé) ---
    # A fal túl vékony, hogy a boss egyszerre érje a falat ÉS elkerülje a tömítőhornyot,
    # ezért a bosst SAROK-GUSSET köti a két falhoz — a horony FÖLÖTT (z ≥ gasket_groove_d),
    # így a tömítőcsatorna a z=0 peremen sértetlen marad.
    for (px, py) in corner_positions:
        # Gusset: tömör sarokblokk a bosstól a két belső falig, a horony felett:
        x_wall = (1 if px > 0 else -1) * inner_x / 2
        y_wall = (1 if py > 0 else -1) * inner_y / 2
        gxc, gyc = (px + x_wall) / 2, (py + y_wall) / 2
        glx = abs(x_wall - px) + boss_dia / 2
        gly = abs(y_wall - py) + boss_dia / 2
        housing += Pos(gxc, gyc, gasket_groove_d) * Box(glx, gly, boss_h - gasket_groove_d, align=(C, C, MIN))
        # Maga a boss + betétfurat alulról (z=0-tól):
        housing += Pos(px, py, 0) * Cylinder(boss_dia / 2, boss_h, align=(C, C, MIN))
        housing -= Pos(px, py, 0) * Cylinder(insert_dia / 2, insert_depth, align=(C, C, MIN))

    return housing


# =========================================================================================
#  2)  A L S Ó   F E D É L   ( bottom_lid )  —  egyben a PCB-tartó alaplap
# =========================================================================================
def build_bottom_lid():
    # Lap: a teljes külső alaprajzot lefedi, fit_gap rés a kerület mentén; z ∈ [−lid_th, 0].
    lid_th = wall
    lid = Box(outer_x - 2 * fit_gap, outer_y - 2 * fit_gap, lid_th, align=(C, C, MAX))

    # 4 sarok átmenő csavarfurat (M5) a ház sarok-bossaihoz, alulról:
    for (px, py) in corner_positions:
        lid -= Pos(px, py, 0) * Cylinder(screw_clear_dia / 2, lid_th + 2 * eps, align=(C, C, MAX))

    # FELFELÉ álló PCB-tartóoszlopok (PLACEHOLDER pozíciók) + M3 betétfurat felülről:
    for (px, py) in pcb_boss_positions:
        lid += Pos(px, py, 0) * Cylinder(pcb_boss_dia / 2, pcb_boss_h, align=(C, C, MIN))
        lid -= Pos(px, py, pcb_boss_h) * Cylinder(pcb_insert_dia / 2, pcb_insert_depth + eps, align=(C, C, MAX))

    return lid


# =========================================================================================
#  3)  A K K U - A J T Ó   ( battery_door )  —  lokális XY síkban, laposan nyomtatható
# =========================================================================================
def build_battery_door():
    dw = bat_open_y + 2 * bat_door_overlap - 2 * fit_gap
    dh = bat_open_z + 2 * bat_door_overlap - 2 * fit_gap
    door = Box(dw, dh, door_th, align=(C, C, MIN))

    # 4 sarok átmenő csavarfurat a ház akku-bossaihoz:
    by = bat_open_y / 2 + bat_door_overlap - bat_boss_inset
    bz = bat_open_z / 2 + bat_door_overlap - bat_boss_inset
    for sy in (-1, 1):
        for sz in (-1, 1):
            door -= Pos(sy * by, sz * bz, 0) * Cylinder(screw_clear_dia / 2, door_th + 2 * eps, align=(C, C, MIN))

    return door


# =========================================================================================
#  Ö S S Z E Á L L Í T Á S   +   M E G J E L E N Í T É S   +   E X P O R T
# =========================================================================================
def main():
    housing = build_housing()
    bottom_lid = build_bottom_lid()
    battery_door = build_battery_door()

    # Az akku-ajtó az ASSEMBLY nézethez a +X falra forgatva/helyezve.
    # Rot(0,90,90):  lokál +X → globál +Y (szélesség),  lokál +Y → +Z (magasság),
    #                lokál +Z → +X (vastagság). Így a 4 sarokfurat a ház bossaira esik.
    # A lapot a +X külső falra ültetjük: a door lokál Z=0 lapja (MIN-align) a +X külső
    # falra (outer_x/2) kerül, így a 4 mm vastag ajtó a falra fekszik (rá tömít a horonyra).
    door_assembled = Pos(outer_x / 2, 0, inner_z / 2) * Rot(0, 90, 90) * battery_door

    # --- Élő viewer (ocp_vscode). Headless futásnál ne dőljön el az export miatt. ---
    try:
        import os
        from ocp_vscode import show, set_port
        # Terminálból/watch.py-ból futtatva nincs OCP_PORT env → a VS Code extension
        # alapértelmezett viewer-portjára (3939) lőjük az ábrát.
        if not os.environ.get("OCP_PORT"):
            set_port(3939)
        show(
            housing, bottom_lid, door_assembled,
            names=["housing", "bottom_lid", "battery_door"],
            colors=["#9aa0a6", "#4f8cff", "#ff9f43"],
            alphas=[0.55, 1.0, 1.0],
        )
        print("[ocp_vscode] viewer frissítve.")
    except Exception as e:
        print(f"[ocp_vscode] viewer nem elérhető (headless?): {e}")

    # --- Export: STL (nyomtatás) + STEP (további CAD) minden alkatrészre ---
    out = Path(__file__).parent / "export"
    out.mkdir(exist_ok=True)
    parts = {"housing": housing, "bottom_lid": bottom_lid, "battery_door": battery_door}
    for name, part in parts.items():
        export_stl(part, str(out / f"{name}.stl"))
        export_step(part, str(out / f"{name}.step"))
        print(f"[export] {name}.stl + {name}.step")

    # Teljes összeállítás STEP-ben (CAD-ellenőrzéshez):
    asm = Compound(children=[housing, bottom_lid, door_assembled])
    export_step(asm, str(out / "assembly.step"))
    print("[export] assembly.step")
    print(f"\nKész. Export ide: {out}")


if __name__ == "__main__":
    main()

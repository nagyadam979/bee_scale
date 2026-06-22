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
# KOORDINÁTÁK (1-node layout): a HOSSZÚ él (80) az Y → ide fekszik a 18650 holder (74.5 ∥ Y).
# A RÖVID él (60) az X. A két HOSSZÚ fal (Y mentén futó, ±X-en) hordozza az akku-ajtót (+X)
# és az USB-C-t (−X, szemközt). A két RÖVID fal a ±Y-on van.
wall             = 3.0    # [mm] általános oldalfal-vastagság
top_th           = 7.0    # [mm] FELSŐ plató vastagsága — vastagabb (terhelést visz + cella-pocket süllyed bele)
inner_x          = 60.0   # [mm] belső RÖVID él  (X) — 1-node-ra hangolva
inner_y          = 80.0   # [mm] belső HOSSZÚ él (Y) — a 18650 holder (74.5) fekszik ide
inner_z          = 45.0   # [mm] belső magasság  (Z, tiszta belmagasság)

# ---- Heat-set menetes betétek (sárgaréz, M5) ----
# M5 sárgaréz heat-set betéthez kezdő furat ~6.4 mm (insert_dia). Az adatlap szerint hangold.
insert_dia       = 6.4    # [mm] heat-set betét furatátmérő (M5 sárgaréz)
insert_depth     = 9.0    # [mm] betétfurat mélység
screw_clear_dia  = 5.5    # [mm] M5 átmenő (clearance) furat a fedélben/ajtóban
boss_dia         = insert_dia + 2 * 2.6   # [mm] csavar-boss külső átmérő (elég falvastagság a betét köré)

# ---- Sarok csavar-bossok (fő fedél + akku-ajtó közös rendszere) ----
boss_h           = 22.0   # [mm] alsó-fedél sarok-bossok magassága (a z=0 síktól felfelé)
corner_inset     = 9.0    # [mm] sarok-boss középpont behúzása a BELSŐ faltól

# ---- FELSŐ PLATÓ: NÉGYZETES fél-hidas mérőcella (SC902/YZC ~34×34), BESÜLLYESZTVE ------
# Koncepció: a cella a tető SÜLLYESZTETT pocketjében ül; a kaptár KÖZVETLENÜL a cella
# felfelé álló dómjára fekszik (nincs közbeiktatott teherbevivő elem). A teher útja:
#   kaptár → dóm (direkt kontakt) → cella → fix vég (2× M4) → háztető → állvány.
# A fix perem (flange) a pocket sík fenekén fekszik; a dóm (7.8) a tető fölé áll ki.
cell_size_xy     = 34.0   # [mm] a cella befoglaló négyzete (SC902/YZC) — megerősítendő darabon
cell_clear       = 0.4    # [mm] pocket oldalrés a cella körül (laza, hőtágulás/illesztés)
cell_recess_d    = 2.5    # [mm] ⚠️ PLACEHOLDER/LEMÉRENDŐ: pocket-mélység = a fix perem vastagsága
                          #      (a perem süllyesztve felfekszik; a 7.8 dóm e fölé áll ki ~5 mm)
# 3 furat EGY KÖZÉPVONALON (a pocket közepén végig, X mentén): középen 1 nagy TERHELŐ
# (a dóm/terhelt vég szabad hajlásteréhez, ÁTMENŐ), két oldalán 2 kis FIX (M4 heat-set).
# ⚠️ A furat-középtáv és átmérők NEM kótázottak a datasheeten (datasheets/cell_SC902/) → LEMÉRENDŐK.
cell_fix_spacing = 25.0   # [mm] ⚠️ LEMÉRENDŐ: a 2 fix furat középtávja (a középvonalon)
cell_fix_dia     = 5.7    # [mm] ⚠️ M4 heat-set betét KEZDŐ furat — pontosítsd a betét adatlapról (~5.6–6.0)
cell_fix_depth   = 8.0    # [mm] M4 heat-set betét mélység (a lefelé lógó boss adja a hosszt)
cell_center_dia  = 14.0   # [mm] ⚠️ LEMÉRENDŐ: a nagy közép (terhelő/dóm) ÁTMENŐ furat — a terhelt vég szabadon hajolhat
cell_fix_boss_dia = cell_fix_dia + 2 * 2.6   # [mm] a fix-furat boss külső átmérője (anyag a betét köré)
cell_fix_boss_h   = cell_fix_depth + 3.0     # [mm] a boss lelógása a pocket-fenéktől a kavitásba (betét + tartalék)

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
# Áthaladó kábelek: (1) mérőcella kábel (Ø0.8 × 4/3 ér), (2) DS18B20 kábel.
# ⚠️ A DS18B20 a SIMA TO-92 tranzisztortokos verzió (NEM a vízálló kapszulázott szonda) →
#    a kábel 3 különálló vékony ér (nincs Ø4 mm-es köpeny). PG7 (kábeltartomány ~3–6.5 mm)
#    vékony kábelre lazán tömít → vagy közös köpenybe húzd a 3 eret, vagy kisebb tömszelence
#    (pl. PG5/M8) ehhez az egy furathoz. A gland_hole_dia furatonként külön is állítható.
gland_hole_dia   = 12.5   # [mm] PG7 tömszelence furat
gland_count      = 3      # [db] tömszelence-furatok száma
gland_z          = 18.0   # [mm] furatok magassága a z=0 alaptól
gland_spacing    = 20.0   # [mm] furatok közti X-osztás (a −Y fal közepére rendezve)

# ---- 18650 akku-rekesz (+X oldal, válaszfallal leválasztva) ---------------------------
# Holder (HESTORE/generic single 18650, vezetékkel), WEBRŐL forrásolt befoglaló: 74.5 × 21.5
# × 17.7 mm; a VEZETÉK a holder KÉT ELLENTÉTES VÉGÉN lép ki, nincs szerelőfül.
# → A rekesz X-szélessége a 21.5-höz igazítva (bat_w − wall ≈ hasznos szélesség).
# ⚠️ A holder hossza (74.5) majdnem kitölti az inner_y-t (80) → a végeken csak ~5 mm marad a
#    rugós érintkezőnek + kábelhajlatnak. Ha szűk: növeld inner_y-t, vagy ültesd ferdén.
bat_w            = 24.0   # [mm] az akku-rekesz X-szélessége (a belső +X faltól befelé)
                          #      hasznos belső szélesség ≈ bat_w − wall = 21 mm (holder 21.5 ~befér)
                          #      a holder a +X strip-ben fekszik, hossza (74.5) ∥ Y
bat_cable_dia    = 8.0    # [mm] kábel-átvezető furat a válaszfalon (holder vezetékei → elektronika)
# Akku-ajtó NYÍLÁS a +X falon (a rekeszbe néz). A 2. commit teszi SÜLLYESZTETTÉ + véglegesíti.
bat_open_y       = 50.0   # [mm] ajtónyílás szélessége Y-ban (commit2: ~76, > holder 74.5)
bat_open_z       = 24.0   # [mm] ajtónyílás magassága Z-ben (akku 17.7 + hozzáférés; befér inner_z=45-be)
bat_door_overlap = 8.0    # [mm] az ajtólap túlnyúlása a nyíláson (tömítés-felfekvés + boss-ok)
door_th          = 4.0    # [mm] akku-ajtó lapvastagság
bat_boss_inset   = 5.0    # [mm] akku-ajtó sarok-boss behúzása az ajtó szélétől

# ---- PCB-k rögzítése az ALSÓ FEDÉLEN: SZORÍTÓBORDÁS fészkek (CSAVAR NÉLKÜL) -----------
# DÖNTÉS (web-kutatás alapján): a SuperMini és a TP4056 furat NÉLKÜLI, a HX711 furatképe
# klón-/gyártófüggő és bizonytalan → MINDEGYIK panelt becsúsztatós szorítóbordák közé
# fogjuk, nincs heat-set betét a PCB-khez. A panel felülről becsúszik, a sarok-padokon ül,
# a sarok-oszlopok oldalról fogják. Forrasztás-hézag a pad-magasságból.
pcb_post_sz   = 3.0   # [mm] sarok-szorítóoszlop keresztmetszete (négyzet)
pcb_post_h    = 8.0   # [mm] oszlopmagasság (a panel-él fölé ér, oldalt fog)
pcb_pad_sz    = 3.5   # [mm] sarok-támasztópad mérete (a panel sarka ezen ül)
pcb_pad_h     = 2.5   # [mm] támasztópad magasság = forrasztás-hézag a fedél felett
# Panelfészkek: (név, közép_x, közép_y, szélesség_x, mélység_y) — befoglaló méretek WEBRŐL,
# forrásolva. A POZÍCIÓK PLACEHOLDER: tuningold a tényleges kábelezés/USB-C hozzáférés szerint.
# Az elektronika a −X/közép térfélen ül; az akku-rekesz a +X oldalon (oda ne tegyél fészket).
#   HX711 zöld generic ...... 34 x 21 mm   (sarokfurat-osztás bizonytalan → bordás fogás)
#   ESP32-C3 SuperMini ...... 24 x 18 mm   (USB-C túlnyúlással; furat NINCS)
#   TP4056 USB-C PROT ....... 29 x 17.3 mm (furat NINCS; USB-C hozzáférés kell töltéshez)
# ⚠️ PLACEHOLDER POZÍCIÓK az új 60×80 envelope-ra: az elektronika a −X térfélben (x<part_x),
#    az akku-rekesz a +X strip-ben (x>part_x). A SuperMini és a TP4056 a −X falhoz közel,
#    USB-C élükkel a −X fal felé (oda jön a 2 süllyesztett USB-port). Tuningold a kábelezéshez.
pcb_nests = [
    ("HX711",     -10.0,   0.0, 34.0, 21.0),   # középen, USB-hozzáférés nem kell
    ("SuperMini", -16.0,  26.0, 24.0, 18.0),   # −X falhoz közel, USB-C él −X felé
    ("TP4056",    -14.0, -26.0, 29.0, 17.3),   # −X falhoz közel, USB-C él −X felé
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
    # Kábel-átvezető furat a válaszfalon (a holder vezetékei a rekesz +Y vége felől jutnak
    # át az elektronikához) — PLACEHOLDER pozíció a felső sarokban, tengely X:
    housing -= Pos(part_x + wall / 2, inner_y / 2 - 12, inner_z - 8) * Rot(0, 90, 0) * \
        Cylinder(bat_cable_dia / 2, wall + 2 * eps, align=(C, C, C))

    # --- Merevítő BORDÁK a plató alatt (lelógnak a tetőből a belső térbe) ---
    # X-irányú bordák (Y-pozíciókon), a tető alól rib_drop mélyen:
    for y in rib_x_positions:
        housing += Pos(0, y, inner_z) * Box(inner_x, rib_th, rib_drop, align=(C, C, MAX))
    # Y-irányú bordák (X-pozíciókon) — csak az elektronikai térfélen, az akku-rekeszt kihagyva:
    for x in rib_y_positions:
        if x < part_x:  # ne lógjon bele az akku-rekeszbe
            housing += Pos(x, 0, inner_z) * Box(rib_th, inner_y, rib_drop, align=(C, C, MAX))

    # --- FELSŐ PLATÓ: BESÜLLYESZTETT mérőcella-pocket + 3-furatos képe ---
    # 1) Pocket a tető felső lapjába (a cella fix pereme ide süllyed, a dóm fölé áll ki):
    housing -= Pos(0, 0, housing_h) * Box(
        cell_size_xy + 2 * cell_clear, cell_size_xy + 2 * cell_clear, cell_recess_d + eps,
        align=(C, C, MAX)
    )
    recess_floor_z = housing_h - cell_recess_d   # a pocket sík feneke (a perem felfekvő síkja)
    # 2) Nagy KÖZÉP terhelő furat — ÁTMENŐ, a terhelt vég/dóm szabad hajlásteréhez:
    housing -= Pos(0, 0, housing_h + eps) * Cylinder(
        cell_center_dia / 2, top_th + 2 * eps, align=(C, C, MAX)
    )
    # 3) 2 kis FIX furat a középvonalon (X mentén), M4 heat-set + lefelé lógó boss a mélységhez:
    for sx in (-1, 1):
        fx = sx * cell_fix_spacing / 2
        # boss a pocket-fenéktől LEFELÉ a kavitásba (betét-mélységet ad):
        housing += Pos(fx, 0, recess_floor_z) * Cylinder(
            cell_fix_boss_dia / 2, cell_fix_boss_h, align=(C, C, MAX)
        )
        # heat-set betét furat a pocket-fenéktől lefelé:
        housing -= Pos(fx, 0, recess_floor_z) * Cylinder(
            cell_fix_dia / 2, cell_fix_depth, align=(C, C, MAX)
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

    # SZORÍTÓBORDÁS PCB-fészkek (csavar nélkül): panelenként 4 sarok-szorítóoszlop +
    # 4 sarok-támasztópad. A panel felülről becsúszik, a padokon ül, az oszlopok oldalt fogják.
    for (_name, cx, cy, w, hh) in pcb_nests:
        for sx in (-1, 1):
            for sy in (-1, 1):
                # sarok-szorítóoszlop a panel élén KÍVÜL (a panelt oldalról fogja):
                ox = cx + sx * (w / 2 + fit_gap + pcb_post_sz / 2)
                oy = cy + sy * (hh / 2 + fit_gap + pcb_post_sz / 2)
                lid += Pos(ox, oy, 0) * Box(pcb_post_sz, pcb_post_sz, pcb_post_h, align=(C, C, MIN))
                # sarok-támasztópad a panel sarka ALATT (a panel pereme erre ül):
                px = cx + sx * (w / 2 - pcb_pad_sz / 2)
                py = cy + sy * (hh / 2 - pcb_pad_sz / 2)
                lid += Pos(px, py, 0) * Box(pcb_pad_sz, pcb_pad_sz, pcb_pad_h, align=(C, C, MIN))

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

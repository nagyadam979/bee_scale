"""
bee_scale — kültéri, elemes IoT szenzoregység (méhkaptár-mérleg node) PARAMETRIKUS HÁZA
=========================================================================================

Anyag: ASA (kültéri, UV-álló). Ez CSAK megjegyzés — a geometriát nem érinti.
Védelmi cél: ~IP65 (eső + por): körbefutó tömítőhornyok, PG7 kábeltömszelencék,
nyitott rés nincs.

ARCHITEKTÚRA-DÖNTÉSEK (1-node koncepció, dokumentált — szabadon átírható):
-------------------------------------------------------------------
* 1 node = 1 önálló sarokegység: 1 db NÉGYZETES FÉL-HIDAS mérőcella (SC902/YZC ~34×34) +
  ESP32-C3 SuperMini + HX711 + TP4056 + 18650 (holderrel) + DS18B20. 4 node a kaptár 4 sarka alatt.
* A doboz ALUL NYITOTT. A teteje tömör, merev plató: ide kerül FELÜLRE a cella, BESÜLLYESZTVE
  (pocket). A kaptár KÖZVETLENÜL a cella felfelé álló dómjára fekszik (nincs teherbevivő elem).
  Felül nincs nyitott rés → felülről nem szivárog be a víz.
* Szerelés ALULRÓL: a FŐ FEDÉL = az alsó lap (bottom_lid). A PCB-k szorítóbordás fészkekben
  ülnek a fedélen (csavar nélkül). Beépíted az elektronikát, majd felcsavarozod a házra.
* BELSŐ VÁLASZFAL a +X oldalon leválaszt egy 18650-rekeszt (a holder ∥ Y fekszik). Ehhez
  SÜLLYESZTETT OLDALSÓ AKKU-AJTÓ ad hozzáférést a +X HOSSZÚ falon.
* USB-C: 2 KÜLÖN SÜLLYESZTETT port a −X HOSSZÚ falon (TP4056 töltés + SuperMini), vízálló dugóval.
* A két HOSSZÚ fal (±X) vastagabb (long_wall_th), mert ide süllyednek az ajtó/portok.
* PG7 tömszelencék az ELÜLSŐ (−Y) rövid falon (DS18B20-kábel stb.).

KOORDINÁTA-RENDSZER:
  X = RÖVID él (inner_x=60), Y = HOSSZÚ él (inner_y=80, ide fekszik a holder), Z = függőleges.
  A ház KÜLSŐ ALJA a z = 0 síkon; a doboz felfelé épül. XY-ban az origóra központosítva.

A 3 nyomtatandó alkatrész:
  1) housing       — falak + tömör tető plató (bordákkal + BESÜLLYESZTETT cella-pocket + 3 furat),
                     válaszfal, tömszelence-furatok, SÜLLYESZTETT akku-ajtó (+X) + USB-portok (−X),
                     a tető alá lógatott SZORÍTÓBORDÁS PCB-fészkek, alsó-zárás tömítőhorony + sarok-bossok.
  2) bottom_lid    — SÍK alsó záró fedéllap, körbefutó tömítőhoronyhoz illeszkedő felület,
                     4 sarok átmenő csavarfurat (PCB-fészkek MÁR NEM itt vannak).
  3) battery_door  — süllyesztett akku-ajtó lap, 4 sarok csavarfurat + tömítőhorony az ajtóban.

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
# MINIMALIZÁLT FALAK (a felhasználó végzi a statikai tervezést → itt csak a SPEC-FEATURE
# minimumok maradnak; minden tisztán-merevítő tömeg eltávolítva). Minden érték a spec-szerep
# minimumára húzva, NEM lehet 1 mm, mert egy-egy feature pinneli:
#   * ±Y (wall): a körbefutó TÖMÍTŐHORONY (gasket_groove_w=2.5) → fal ≥ ~3 (különben a horony átvágja).
#   * ±X (long_wall_th): az ajtó/USB REBATE (bat_recess_d=3) + ~1 fenék → ≥ ~4.
#   * tető (top_th): a cella-POCKET (cell_recess_d=2.5) + ~1 fenék → ≥ ~3.5.
wall             = 3.0    # [mm] rövid ±Y falak — a tömítőhorony pinneli (min ~3), nem mehet lejjebb
long_wall_th     = 4.0    # [mm] hosszú ±X falak (ajtó/USB rebate 3 + 1 fenék) — 5→4 vékonyítva
lid_th           = 1.5    # [mm] alsó fedél vastagsága — leválasztva a wall-ról, vékonyítva (statika: user)
partition_th     = 1.5    # [mm] belső válaszfal (akku-rekesz leválasztó) — vékonyítva (csak osztó, nem teherviselő)
top_th           = 3.5    # [mm] FELSŐ plató — a cella-pocket (2.5) + ~1 fenék minimumára (7→3.5). TERHELÉST VISZ → user statika
inner_x          = 60.0   # [mm] belső RÖVID él  (X) — 1-node-ra hangolva
inner_y          = 80.0   # [mm] belső HOSSZÚ él (Y) — a 18650 holder (74.5) fekszik ide
inner_z          = 30.0   # [mm] belső magasság (Z) — TOVÁBB MINIMALIZÁLVA (36→30, user). Az akku-
                          #      rekesz a sarok-bossok TETEJE fölé ült (holder alja = boss_h+holder_gap),
                          #      és az ajtó/nyílás EHHEZ a felső sávhoz igazodik (nem a doboz közepéhez),
                          #      a karima 1 mm-re csökkent → a doboz ~6 mm-rel alacsonyabb. Kötő korlát
                          #      most: batt_z0(9) + 18650 Ø18.5 + felső margó ⇒ ~30. (Lásd akku-szekció.)

# ---- Sarok-rögzítés: M2.5 ÖNMETSZŐ MŰANYAG (nylon) csavar — heat-set betét NÉLKÜL ----
# USER-DÖNTÉS: az M5 sárgaréz heat-set + fém csavar lecserélve FELE átmérőjű/hosszúságú MŰANYAG
# csavarra (M5→M2.5). A nylon csavar KÖZVETLENÜL a műanyag bossba metsz (nincs sárgaréz betét) →
# a bossba PILOT (előfúrt) furat megy. A bossok így feleakkorák: kevesebb anyag, kompaktabb belső
# tér (a külső magasságot ez nem csökkenti — az inner_z változatlan; ha kell, külön vágjuk).
screw_nom_dia    = 2.5    # [mm] M2.5 névleges (az eddigi M5 fele)
screw_clear_dia  = 2.9    # [mm] M2.5 átmenő (clearance) furat a fedélben
pilot_dia        = 2.1    # [mm] ⚠️ önmetsző PILOT furat a bossban — M2.5 nylonhoz ~2.0–2.1 (csavar/anyag-függő, hangold)
pilot_depth      = 6.0    # [mm] pilot mélység a bossban (csavar-behatolás) — ~fele az eddigi 9-nek
boss_dia         = pilot_dia + 2 * 1.85   # [mm] boss külső átmérő (~5.8; elég fal a pilot köré) — ~fele az eddigi 11.6-nak

# ---- Sarok csavar-bossok (fő fedél + akku-ajtó közös rendszere) ----
boss_h           = 7.0    # [mm] alsó-fedél sarok-bossok magassága (z=0-tól) = pilot_depth(6) + ~1 fenék — FELE a 14-nek
corner_inset     = boss_dia / 2 + 2.0  # [mm] sarok-boss közép behúzása a BELSŐ faltól: a boss SZÉLE
                          #      2 mm-re a falsíktól (user: "majdnem a szélére"). =4.9; a tömítőhorony
                          #      belső éle (x=31/y=40.5) még kintebb → a boss nem éri (IP65 sértetlen).

# ---- FELSŐ PLATÓ: NÉGYZETES fél-hidas mérőcella (SC902/YZC ~34×34), BESÜLLYESZTVE ------
# Koncepció: a cella a tető SÜLLYESZTETT pocketjében ül; a kaptár KÖZVETLENÜL a cella dómjára
# fekszik. A pocket FENEKE TÖMÖR (nincs alatta furat → felülről teljesen vízálló).
# RÖGZÍTÉS: a cella saját furatképét NEM használjuk (kóta bizonytalan); helyette 2 db M4 heat-set
# furat a süllyesztésen KÍVÜL, a HOSSZÚ (Y) középvonalon (x=0), a recess pereme mellett 3 mm-re
# — ide egy áthidaló leszorító bracket csavarozható, ami a cellát a pocketben tartja.
cell_size_xy     = 34.0   # [mm] a cella befoglaló négyzete (SC902/YZC) — megerősítendő darabon
cell_clear       = 0.4    # [mm] pocket oldalrés a cella körül (laza, hőtágulás/illesztés)
cell_recess_d    = 1.0    # [mm] pocket-mélység: a cella csak ~1 mm-re süllyed (user). Fenék így 3.5−1.0=2.5 mm (vastagabb)
cell_screw_clearance = 3.0   # [mm] rés a süllyesztés pereme és a rögzítőfurat SZÉLE között ("3mm-re kívülre")
cell_fix_dia     = 5.7    # [mm] ⚠️ M4 heat-set betét KEZDŐ furat — pontosítsd a betét adatlapról (~5.6–6.0)
cell_fix_depth   = 8.0    # [mm] M4 heat-set betét mélység (a lefelé lógó boss adja a hosszt)
cell_fix_boss_dia = cell_fix_dia + 2 * 2.6   # [mm] a fix-furat boss külső átmérője (anyag a betét köré)
cell_fix_boss_h   = cell_fix_depth + 3.0     # [mm] a boss lelógása a tető aljától a kavitásba (betét + tartalék)

# ---- MEREVÍTŐ BORDÁK — ELTÁVOLÍTVA (user: a statikai tervezést ő végzi) ----------------
# A bordák tisztán merevítő (nem spec) szerepűek voltak → kivéve. A pozíció-listák ÜRESEK,
# így a build_housing rib-ciklusai nem adnak hozzá semmit. Ha kellenek, töltsd fel a listákat.
rib_th           = 3.0    # [mm] (unused) borda vastagság
rib_drop         = 12.0   # [mm] (unused) borda lelógás
rib_x_positions  = []     # [mm] X-irányú bordák Y-pozíciói — ÜRES (nincs borda)
rib_y_positions  = []     # [mm] Y-irányú bordák X-pozíciói — ÜRES (nincs borda)

# ---- Körbefutó tömítőhorony (szilikon tömítéshez) — alsó zárás ÉS akku-ajtó ----------
gasket_groove_w  = 2.0    # [mm] tömítőhorony szélesség — 2.5→2.0 (user: egységesen ~2 mm max körbe, ajtó+fő zárás)
gasket_groove_d  = 2.0    # [mm] tömítőhorony mélység

# ---- USB-C portok a −X HOSSZÚ falon (2 KÜLÖN, süllyesztett) ---------------------------
# Töltés/firmware: a TP4056 és a SuperMini USB-C-je külön port. Mindkettő SÜLLYESZTETT:
# a −X külső falba egy rebate-pocket (ide ül a vízálló DUGÓ/sapka, flush), a fenekén pedig
# egy átmenő nyílás a csatlakozóhoz. A dugó: off-the-shelf szilikon bung VAGY kis nyomtatott
# sapka, a recess-méretre — geometriát itt nem modellezünk hozzá (placeholder).
# ⚠️ A POZÍCIÓK (y,z) és MÉRETEK PLACEHOLDER: a tényleges USB-C csatlakozó helye a panelon +
#    a panel-fészek pozíciója határozza meg → a fizikai elrendezésnél pontosítandó.
# Az ÁTMENŐ nyílás a connector VALÓS geometriájához igazítva (STADION / obround alak):
#   USB Type-C plug fém héj: ~8.4 mm SZÉLES × 2.56 mm MAGAS, teljesen lekerekített végek
#   (végsugár = magasság/2 ≈ 1.28 mm). Receptacle belső nyílás ~8.34 × 2.56.
#   Forrás: USB-IF USB Type-C Cable & Connector Spec R2.0 (2019), usb.org.
usb_open_w       = 9.0    # [mm] átmenő STADION-nyílás SZÉLESSÉG (Y) = 8.4 plug + ~0.3/oldal hézag
usb_open_h       = 3.0    # [mm] átmenő STADION-nyílás MAGASSÁG (Z) = 2.56 + hézag; végsugár = h/2 = 1.5
# A DUGÓ/sapka SÜLLYESZTÉSE (recess): az overmold (a kábel tövének műanyag burka) NEM szabványos,
# gyártófüggő → a recess bővebb és ⚠️ PLACEHOLDER (a használt kábel/dugó overmoldjához igazítandó):
usb_recess_w     = 14.0   # [mm] rebate-pocket SZÉLESSÉG (Y) — overmold/dugó-perem ⚠️ PLACEHOLDER
usb_recess_h     = 8.0    # [mm] rebate-pocket MAGASSÁG (Z) ⚠️ PLACEHOLDER
usb_recess_d     = 2.5    # [mm] rebate mélység a −X külső faltól (a dugó ide süllyed, flush)
# (név, y_közép, z_közép) a −X falon — a SuperMini/TP4056 fészkekhez igazítva (y=±28), a port
# Z-szintje a board-szinten (a panelok pcb_seat_z=32 alá lógatva → a csatlakozó ~z=29):
usb_ports = [
    ("TP4056_USB", -11.0, 18.8),    # a TP4056 USB-C connectoráhez (board y=−11, z≈18.8 — layout.py)
    ("SuperMini_USB", 13.0, 18.8),  # a SuperMini USB-C connectoráhez (board y=+13, z≈18.8 — layout.py)
]

# ---- PG7 kábeltömszelencék (elülső −Y fal) — JELENLEG HASZNÁLATON KÍVÜL ----------------
# ⚠️ A −Y fali gland-furatok (user kérésére) ELTÁVOLÍTVA a build_housing-ból. Az alábbi
#    paraméterek megmaradtak, de jelenleg NEM használja semmi. A kábel-bevezetés (cella-kábel,
#    DS18B20) így NYITOTT kérdés — ha újra kell, a build_housing-ban a gland-szekciót vissza kell tenni.
gland_hole_dia   = 12.5   # [mm] (unused) PG7 tömszelence furat
gland_count      = 0      # [db] (unused) tömszelence-furatok száma — 0-ra állítva
gland_z          = 18.0   # [mm] (unused) furatok magassága a z=0 alaptól
gland_spacing    = 20.0   # [mm] (unused) furatok közti X-osztás

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
# Akku-ajtó: SÜLLYESZTETT (flush) a +X hosszú falon. A 18650 a +X strip-ben fekszik (∥ Y);
# az ajtó "kicsit nagyobb mint az akku" → a nyílás > cella, hogy a 18650 kivehető legyen.
# AKKU-REKESZ Z-POZÍCIÓ (user): a holder+aksi a sarok-bossok TETEJE fölé ül holder_gap-pel
# ("csavarfejek pár mm-re az akku tartótól"); az ajtó/nyílás EHHEZ a sávhoz igazodik, nem a
# doboz közepéhez → a magasság minimalizálható.
holder_gap       = 2.0    # [mm] hézag a sarok-boss teteje (boss_h) és az akku-tartó alja közt
batt_z0          = boss_h + holder_gap   # [mm] az akku-rekesz (holder+aksi) ALJA z-ben (=9.0)
# Ajtónyílás: a +X oldal "majdnem teljes egészében ajtó" 1 mm karimával (user). A nyílás a
# HOLDER beraknához (74.5 ∥ Y) és a 18650 OLDALRA húzásához (Ø18.5) van méretezve.
bat_open_y       = 76.0   # [mm] nyílás Y = holder hossz 74.5 + ~1.5 hézag (a holder is bedugható)
# A nyílás LEMEGY majdnem az aljáig (user: "ne maradjon fal", csak ~fél centi perem az alsó
# tömítőhoronynak). Alja = floor_margin(5) + karima(1); teteje a 18650 tetejét fedi (~z=28) a
# kivételhez. A sarok-bossok (z[0,7]) beljebb vannak X-ben (x≤28) → a nyílás (fal x≥30.5) nem éri.
bat_door_overlap = 1.0    # [mm] karima (rebate-váll) a nyílás körül — user: 2→1
bat_floor_margin = 5.0    # [mm] az AJTÓ alja a doboz aljától ("fél centi" perem az alsó horonynak)
bat_open_z0      = bat_floor_margin + bat_door_overlap   # [mm] a nyílás ALJA z-ben (=6.0)
bat_open_z       = (batt_z0 + 18.5 + 0.5) - bat_open_z0  # [mm] nyílás Z: a teteje a 18650 (Ø18.5) tetejét fedi → =22
door_th          = 3.0    # [mm] akku-ajtó lapvastagság = a rebate mélysége (FLUSH a fallal)
bat_recess_d     = door_th  # [mm] rebate (besüllyesztés) mélysége a +X külső faltól → az ajtó síkban van
bat_boss_inset   = 5.0    # [mm] akku-ajtó sarok-boss behúzása a váll szélétől

# ---- PCB-k rögzítése — ⚠️ A HÁZ MÁR NEM ÉPÍT TARTÓT (user döntés) ----------------------
# DÖNTÉS (user): minden komponens EGY közös alaplapra forrasztva; a board-tartást a felhasználó
# maga oldja meg. A korábbi tető-alá-lógatott szorítóoszlop/támasztópad fészkek a build_housing-ból
# ELTÁVOLÍTVA. Az alábbi pcb_* paraméterek + pcb_nests CSAK REFERENCIÁNAK maradtak (layout.py
# SEAT_Z + a makett-pakolás Z-szintjéhez), a ház geometriáját NEM befolyásolják.
pcb_post_sz   = 3.0   # [mm] sarok-szorítóoszlop keresztmetszete (négyzet)
pcb_pad_sz    = 3.5   # [mm] sarok-támasztópad mérete (a panel felső sarka ehhez fekszik)
pcb_seat_z    = 22.0  # [mm] a board FELSŐ lapjának Z-szintje (a tető alá lógatva; bordák z=24/cella-boss z=25 alatt)
pcb_grip      = 6.0   # [mm] a szorítóoszlop a seat ALATT ennyivel fogja a panel élét (vastagság+tér)
# Panelfészkek: (név, közép_x, közép_y, szélesség_x, mélység_y) — befoglaló méretek WEBRŐL.
# ⚠️ POZÍCIÓK PLACEHOLDER: az elektronika a −X/közép térfélen (x<part_x), az akku-rekesz a +X
#    strip-ben. A boardok BELJEBB tolva (Y-ban szétosztva), hogy ne lógjanak a sarkokba/cellába:
#   HX711 ...... 34×21 (középen, USB nem kell) | SuperMini 24×18 + TP4056 29×17.3 (USB-él −X felé)
pcb_nests = [
    ("HX711",     -12.0,   0.0, 34.0, 21.0),   # középen (y=0); X-ben is maximálisan középen (a +X felet az akku foglalja)
    ("SuperMini", -14.0,  21.0, 24.0, 18.4),   # SZOROSAN a HX711 fölött, USB-C él −X felé (24×18.4 user-mért)
    ("TP4056",    -14.0, -21.0, 18.0, 14.0),   # SZOROSAN a HX711 alatt, USB-C él −X felé (18×14 user-mért)
]

# =========================================================================================
#  S Z Á R M A Z T A T O T T   ( ne állítsd kézzel )
# =========================================================================================
outer_x   = inner_x + 2 * long_wall_th  # külső szélesség (a HOSSZÚ ±X falak vastagabbak)
outer_y   = inner_y + 2 * wall          # külső mélység   (a rövid ±Y falak)
housing_h = inner_z + top_th            # ház teljes magassága (alja nyitott, teteje plató)
bat_cz    = bat_open_z0 + bat_open_z / 2  # akku-nyílás/ajtó Z-KÖZÉP (a rekesz szintjén, NEM inner_z/2)
eps       = 0.5                         # [mm] boolean-tisztasághoz extra túllógás

# A sarok-boss középpontok (a belső sarkoktól behúzva) — fő fedél rögzítéséhez:
corner_positions = [
    ( inner_x / 2 - corner_inset,  inner_y / 2 - corner_inset),
    (-inner_x / 2 + corner_inset,  inner_y / 2 - corner_inset),
    ( inner_x / 2 - corner_inset, -inner_y / 2 + corner_inset),
    (-inner_x / 2 + corner_inset, -inner_y / 2 + corner_inset),
]

# Tömítőhorony középvonalának fél-kiterjedése (a falközépen fut körbe):
groove_half_x = inner_x / 2 + long_wall_th / 2
groove_half_y = inner_y / 2 + wall / 2

C = Align.CENTER
MIN = Align.MIN
MAX = Align.MAX


def stadium_x(depth, width, height):
    """STADION (obround) keresztmetszet a Y–Z síkban, X mentén `depth` hosszan extrudálva.
    width ≥ height; a két vég TELJES körív (sugár = height/2) → ez az USB-C connector valós
    profilja. Origóra központosítva. (Henger X-tengelyre: Rot(0,90,0) — empirikusan ellenőrzött.)"""
    r = height / 2.0
    flat = width - height          # a középső EGYENES szakasz hossza Y-ban (a két körív között)
    cyl_p = Pos(0,  flat / 2, 0) * Rot(0, 90, 0) * Cylinder(r, depth, align=(C, C, C))
    cyl_n = Pos(0, -flat / 2, 0) * Rot(0, 90, 0) * Cylinder(r, depth, align=(C, C, C))
    s = cyl_p + cyl_n
    if flat > 1e-6:
        s = s + Box(depth, flat, height, align=(C, C, C))
    return s


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
    # A válaszfal belső (−X felőli) lapja x = inner_x/2 − bat_w-nél; vastagsága partition_th (vékonyítva).
    part_x = inner_x / 2 - bat_w
    partition = Pos(part_x, 0, 0) * Box(partition_th, inner_y, inner_z, align=(MIN, C, MIN))
    housing += partition
    # Kábel-átvezető furat a válaszfalon (a holder vezetékei a rekesz +Y vége felől jutnak
    # át az elektronikához) — PLACEHOLDER pozíció a felső sarokban, tengely X:
    housing -= Pos(part_x + partition_th / 2, inner_y / 2 - 12, inner_z - 8) * Rot(0, 90, 0) * \
        Cylinder(bat_cable_dia / 2, partition_th + 2 * eps, align=(C, C, C))

    # --- Merevítő BORDÁK a plató alatt (lelógnak a tetőből a belső térbe) ---
    # X-irányú bordák (Y-pozíciókon), a tető alól rib_drop mélyen:
    for y in rib_x_positions:
        housing += Pos(0, y, inner_z) * Box(inner_x, rib_th, rib_drop, align=(C, C, MAX))
    # Y-irányú bordák (X-pozíciókon) — csak az elektronikai térfélen, az akku-rekeszt kihagyva:
    for x in rib_y_positions:
        if x < part_x:  # ne lógjon bele az akku-rekeszbe
            housing += Pos(x, 0, inner_z) * Box(rib_th, inner_y, rib_drop, align=(C, C, MAX))

    # --- FELSŐ PLATÓ: BESÜLLYESZTETT mérőcella-pocket (TÖMÖR FENÉK) + KÜLSŐ rögzítőfuratok ---
    # 1) Pocket a tető felső lapjába (a cella ide süllyed). A pocket FENEKE TÖMÖR: NINCS alatta
    #    furat → felülről teljesen vízálló (a korábbi közép- és fix-furatok eltávolítva).
    housing -= Pos(0, 0, housing_h) * Box(
        cell_size_xy + 2 * cell_clear, cell_size_xy + 2 * cell_clear, cell_recess_d + eps,
        align=(C, C, MAX)
    )
    # 2) 2 db M4 heat-set rögzítőfurat a süllyesztésen KÍVÜL, a HOSSZÚ (Y) középvonalon (x=0),
    #    a recess pereme mellett cell_screw_clearance-re. Felülről BLIND (a boss tömör anyagba zár
    #    → vízálló). Ide egy áthidaló leszorító bracket csavarozható a cella tartásához.
    cell_pocket_half_y = (cell_size_xy + 2 * cell_clear) / 2
    cell_screw_y = cell_pocket_half_y + cell_screw_clearance + cell_fix_dia / 2
    for sy in (-1, 1):
        cy_s = sy * cell_screw_y
        # boss a tető aljától (z=inner_z) LEFELÉ a kavitásba (a blind furat-fenéknek anyag):
        housing += Pos(0, cy_s, inner_z) * Cylinder(
            cell_fix_boss_dia / 2, cell_fix_boss_h, align=(C, C, MAX)
        )
        # M4 heat-set betét furat FELÜLRŐL lefelé (blind, a tetőn át a bossba):
        housing -= Pos(0, cy_s, housing_h) * Cylinder(
            cell_fix_dia / 2, cell_fix_depth + eps, align=(C, C, MAX)
        )

    # --- (A −Y falon korábbi PG7 tömszelence-furatok ELTÁVOLÍTVA, user kérés) ---
    # A kábel-bevezetés módja (cella-kábel, DS18B20) ezzel ÚJRA NYITOTT kérdés → lásd HANDOFF.

    # --- AKKU-AJTÓ: SÜLLYESZTETT (flush) fedő a +X hosszú falon, CSAVAR NÉLKÜL ---
    # (user: a csavarozás kivéve — marad a süllyesztett fedő + a tömítőhorony az ajtóban.)
    # (1) rebate-pocket az ajtónak (bat_recess_d mély, váll-méretű) → az ajtó síkban ül;
    # (2) a rebate-fenéken át a kavitásba nyíló ÁTMENŐ nyílás. NINCS sarok-boss / betétfurat.
    face_x   = outer_x / 2                       # +X külső falsík
    # bat_cz (az ajtó/nyílás Z-középpontja) MODUL-SZINTŰ → a rekesz szintjén, nem inner_z/2.
    reb_y    = bat_open_y + 2 * bat_door_overlap  # rebate (váll) Y-mérete
    reb_z    = bat_open_z + 2 * bat_door_overlap  # rebate (váll) Z-mérete
    # (1) rebate-pocket: a +X külső faltól befelé bat_recess_d mélyen (az ajtó + fit_gap belefér):
    housing -= Pos(face_x, 0, bat_cz) * Box(
        bat_recess_d + eps, reb_y + 2 * fit_gap, reb_z + 2 * fit_gap, align=(MAX, C, C)
    )
    # (2) ÁTMENŐ nyílás a rebate-fenéktől a kavitásba (a teljes hosszú falon át):
    housing -= Pos(face_x, 0, bat_cz) * Box(
        long_wall_th + 2 * eps, bat_open_y, bat_open_z, align=(MAX, C, C)
    )

    # --- USB-C portok a −X HOSSZÚ falon (2 KÜLÖN, süllyesztett dugóhoz) ---
    # Portonként: (1) rebate-pocket a −X külső faltól befelé (+X) usb_recess_d mélyen (a dugó
    # ide süllyed, flush); (2) átmenő nyílás a rebate-fenéktől a kavitásba a csatlakozóhoz.
    usb_face_x = -outer_x / 2                 # −X külső falsík
    for (_name, uy, uz) in usb_ports:
        # (1) rebate-pocket STADION-alakban (a dugó/sapka pereméhez), a −X faltól befelé:
        housing -= Pos(usb_face_x + usb_recess_d / 2, uy, uz) * stadium_x(
            usb_recess_d + eps, usb_recess_w, usb_recess_h
        )
        # (2) átmenő STADION-nyílás a connector VALÓS profiljához (a teljes falon át a kavitásba):
        housing -= Pos(usb_face_x + long_wall_th / 2, uy, uz) * stadium_x(
            long_wall_th + 2 * eps, usb_open_w, usb_open_h
        )

    # --- PCB-fészkek ELTÁVOLÍTVA (user: minden EGY alaplapra forrasztva; a board-tartást a
    #     felhasználó maga oldja meg). A ház NEM épít szorítóoszlopot/támasztópadot.
    #     Az USB-C portok (usb_ports, fent) MARADNAK. A pcb_nests/pcb_* paraméterek a fájlban
    #     maradtak referenciának (layout.py SEAT_Z + pakolás), de itt már semmit nem építenek. ---

    # --- FŐ (alsó) ZÁRÁS: körbefutó tömítőhorony az ALSÓ peremfelületen (z=0, felfelé) ---
    groove_out = Box(2 * groove_half_x + gasket_groove_w, 2 * groove_half_y + gasket_groove_w,
                     gasket_groove_d, align=(C, C, MIN))
    groove_in  = Box(2 * groove_half_x - gasket_groove_w, 2 * groove_half_y - gasket_groove_w,
                     gasket_groove_d, align=(C, C, MIN))
    housing -= (groove_out - groove_in)

    # --- FŐ FEDÉL sarok-bossok a belső sarkokban (pilot furat alulról, z=0-tól felfelé) ---
    # A boss a belső térben áll (a faltól behúzva), ezért GUSSET köti a falhoz — a tömítőhorony
    # FÖLÖTT (z ≥ gasket_groove_d), hogy a z=0 peremen a tömítőcsatorna sértetlen maradjon.
    # ⚠️ A gusset CSAK a ±Y (RÖVID) fal felé nyúlik. A ±X (hosszú) falakban SÜLLYESZTÉS van
    # (akku-ajtó rebate a +X-en, USB recess a −X-en); ha a gusset oda nyúlna, a rebate terébe /
    # az ajtó tömítőhornyába lógna (a user által jelzett ütközés). A ±Y falnál rendes 3 mm tömör
    # anyag van a horony felett → ide kapcsolódik robosztusan (boss+gusset+fal+plató = 1 solid).
    gusset_wall_grip = 0.5   # [mm] a gusset ennyivel ér a belső falsík MÖGÉ (átfedés az 1 solidhoz)
    for (px, py) in corner_positions:
        sy = 1 if py > 0 else -1
        by_far = py - sy * boss_dia / 2                  # boss doboz-közép felőli széle (Y-ban)
        wy     = sy * (inner_y / 2 + gusset_wall_grip)   # a ±Y belső falsík mögé grip-ig
        gyc    = (by_far + wy) / 2
        gly    = abs(wy - by_far)
        # Gusset: a boss szélességében (X=boss_dia), a bosstól a ±Y falig (a horony felett):
        housing += Pos(px, gyc, gasket_groove_d) * Box(boss_dia, gly, boss_h - gasket_groove_d, align=(C, C, MIN))
        # Maga a boss + ÖNMETSZŐ PILOT furat alulról (z=0-tól) — nincs heat-set betét:
        housing += Pos(px, py, 0) * Cylinder(boss_dia / 2, boss_h, align=(C, C, MIN))
        housing -= Pos(px, py, 0) * Cylinder(pilot_dia / 2, pilot_depth, align=(C, C, MIN))

    return housing


# =========================================================================================
#  2)  A L S Ó   F E D É L   ( bottom_lid )  —  sík záró lap (a PCB-fészkek a HOUSING-ban vannak)
# =========================================================================================
def build_bottom_lid():
    # Lap: a teljes külső alaprajzot lefedi, fit_gap rés a kerület mentén; z ∈ [−lid_th, 0].
    # A PCB-fészkek MÁR NEM itt vannak — a housing tartja a panelokat a tető alá lógatva.
    # lid_th vékonyítva (modul-szintű paraméter); a statikát a user végzi.
    lid = Box(outer_x - 2 * fit_gap, outer_y - 2 * fit_gap, lid_th, align=(C, C, MAX))

    # 4 sarok átmenő csavarfurat (M5) a ház sarok-bossaihoz, alulról:
    for (px, py) in corner_positions:
        lid -= Pos(px, py, 0) * Cylinder(screw_clear_dia / 2, lid_th + 2 * eps, align=(C, C, MAX))

    return lid


# =========================================================================================
#  3)  A K K U - A J T Ó   ( battery_door )  —  lokális XY síkban, laposan nyomtatható
# =========================================================================================
def build_battery_door():
    # Az ajtó a rebate-be ül (footprint = rebate − fit_gap körben). Lokál: X=szélesség (→Y a
    # házon), Y=magasság (→Z), Z=vastagság (→X). A lokál Z=0 lap a ház felé néz (rebate-fenék).
    dw = bat_open_y + 2 * bat_door_overlap - 2 * fit_gap
    dh = bat_open_z + 2 * bat_door_overlap - 2 * fit_gap
    door = Box(dw, dh, door_th, align=(C, C, MIN))

    # ⚠️ TÖMÍTŐHORONY ELHAGYVA: a karima (bat_door_overlap=1) túl keskeny a gasket_groove_w=2
    # széles horonyhoz (belógna a nyílásba ÉS túl a peremen). Az ajtó SIMA süllyesztett lap.
    # → Az akku-ajtó TÖMÍTÉSE és RETENCIÓJA NYITOTT (lapos tömítés a rebate-fenékre / O-gyűrű /
    #   pattintó-klips) — külön megoldandó az IP65-höz. (CSAVARFURAT sincs, user korábbi döntés.)

    return door


# =========================================================================================
#  Ö S S Z E Á L L Í T Á S   +   M E G J E L E N Í T É S   +   E X P O R T
# =========================================================================================
def main():
    housing = build_housing()
    bottom_lid = build_bottom_lid()
    battery_door = build_battery_door()

    # Az akku-ajtó az ASSEMBLY nézethez a +X falra forgatva/helyezve, a REBATE-BE süllyesztve.
    # Rot(0,90,90):  lokál +X → globál +Y (szélesség),  lokál +Y → +Z (magasság),
    #                lokál +Z → +X (vastagság). Így a 4 sarokfurat a ház bossaira esik.
    # A door lokál Z=0 lapja a rebate FENEKÉRE (outer_x/2 − bat_recess_d) kerül, és +X felé
    # door_th vastagon a külső falsíkig (outer_x/2) ér → FLUSH a fallal.
    door_assembled = Pos(outer_x / 2 - bat_recess_d, 0, bat_cz) * Rot(0, 90, 90) * battery_door

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

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
    Polygon, extrude, Plane,
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
partition_th     = 2.5    # [mm] belső válaszfal (akku-rekesz leválasztó) — 1.5→2.5 vastagítva (user)
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
corner_inset     = 2.8    # [mm] boss-közép behúzása a BELSŐ faltól (user: "közelebb a falhoz") — 3.0→2.8.
                          #      A boss SZÉLE így ~0.1 mm-re a FALBA ér (x_edge=30.1 / y_edge=40.1), DE a
                          #      tömítőhorony belső éle még kintebb (x=31 / y=40.5) → a horony SÉRTETLEN
                          #      (IP65). KORLÁT: a +Y fal a szűk (horony 40.5) → ez pinneli a ~2.8 minimumot.
side_inset       = corner_inset  # [mm] az OLDAL-KÖZÉP bossok behúzása a faltól (ugyanaz a logika)
# FÉLKÖR (D) MEREVÍTŐ COLLAR a boss körül (a téglalap-gusset helyett, user): boss_collar_dia átmérőjű
# henger a horony FÖLÖTT (z[gd,boss_h]), a falsíkon túli rész levágva, collar_merge-dzsel a falba nyúlva.
boss_collar_dia  = boss_dia + 2 * 1.6   # [mm] collar külső átmérő (~9.0; ~1.6 mm gyűrű a boss köré)
collar_merge     = 0.6    # [mm] a collar lapos oldala ennyivel nyúlik a falsík MÖGÉ (a fallal egyesülés;
                          #      a horony fölött (z≥gd) van → nem éri a hornyot, IP65 marad)

# ---- FELSŐ PLATÓ: NÉGYZETES fél-hidas mérőcella (SC902/YZC ~34×34), BESÜLLYESZTVE ------
# Koncepció: a cella a tető SÜLLYESZTETT pocketjében ül; a kaptár KÖZVETLENÜL a cella dómjára
# fekszik. A pocket FENEKE TÖMÖR (nincs alatta furat → felülről teljesen vízálló).
# RÖGZÍTÉS: a cella saját furatképét NEM használjuk (kóta bizonytalan); helyette 2 db furat a
# süllyesztésen KÍVÜL, a HOSSZÚ (Y) középvonalon (x=0), a recess pereme mellett 3 mm-re — ide egy
# áthidaló leszorító bracket csavarozható. A CSAVAR UGYANAZ mint az alsó sarkokon: M2.5 ÖNMETSZŐ
# MŰANYAG, pilot furat, heat-set betét NÉLKÜL (user: "olyan mint az alján").
cell_size_xy     = 34.0   # [mm] a cella befoglaló négyzete (SC902/YZC) — megerősítendő darabon
cell_clear       = 0.4    # [mm] pocket oldalrés a cella körül (laza, hőtágulás/illesztés)
cell_recess_d    = 1.0    # [mm] pocket-mélység: a cella csak ~1 mm-re süllyed (user). Fenék így 3.5−1.0=2.5 mm (vastagabb)
cell_screw_clearance = 3.0   # [mm] rés a süllyesztés pereme és a rögzítőfurat SZÉLE között ("3mm-re kívülre")
cell_fix_dia     = pilot_dia    # [mm] M2.5 önmetsző PILOT furat (mint az alsó sarkok; volt M4 heat-set 5.7)
cell_fix_depth   = pilot_depth  # [mm] pilot mélység = 6 (volt 8)
cell_fix_boss_dia = boss_dia    # [mm] boss külső átmérő = 5.8, UGYANAZ mint az alsó sarok-boss
cell_fix_boss_h   = cell_fix_depth + 3.0     # [mm] a boss lelógása a tető aljától a kavitásba (pilot + tartalék)
# CELLA-TÁMASZTÁS (user): a fél-híd terhelés alatt LEHAJLIK — a SZÉLE megtámasztva (peremen ül), a
# KÖZEPE szabadon hajlik le egy üregbe. Ehhez: (1) KARIMA — a pocket alatti fenék kiszélesítve
# +flange_margin-nel körbe és lelógatva flange_drop-pal (megvastagított teherviselő talp); (2) DENT
# — a pocket fenekének közepébe egy üreg (a cellánál dent_margin-nel kisebb körbe), ide hajlik a közép.
cell_flange_margin = 4.0   # [mm] a karima ennyivel szélesebb a pocketnél, körbe — 5→4 (user: "vegyél vissza 1mm-t")
cell_flange_drop   = 3.0   # [mm] a karima ennyivel lóg a fenék alá a kavitásba (megvastagítás, user)
cell_dent_margin   = 3.0   # [mm] a dent ennyivel kisebb a pocketnél, körbe ("3-3 mm-el kisebb téglalap")
cell_dent_d        = 3.0   # [mm] a dent (közép-üreg) mélysége a pocket fenekétől lefelé (user)
# CELLA-TÁMASZTÓ OSZLOP — ÁLLÓ "Y" a HOSSZANTI (Y-Z) SÍKBAN, az X-középvonalon (x=0). Teher útja:
# kaptár → cella → platform(dent) → oszlop → talp → doboz alja (lid/talaj). A két ÁG FELÜL a cella-
# ágy (pocket, ±17.4) Y-szélein indul, lefelé a V-csúcsban EGYESÜL, onnan közös szár/talp a dobozaljáig.
# ⚠️ ORIENTÁCIÓ: az ágak az Y-tengely mentén nyílnak szét (NEM X-ben → nem ér az akku-holderhez), és
#    X-ben VÉKONY marad a középvonalon. A holder a +X stripben (x≥7.5) → az oszlop x[−6,6]-ban marad.
cell_col_w_top      = 8.0    # [mm] az ÁG-vastagsága (Y-ban, a szétnyílás irányában) — user min: ≥8
cell_col_w_base     = 12.0   # [mm] a közös TALP/szár szélessége (Y-ban) alul — user min: ≥12
cell_col_junction_z = 14.0   # [mm] a két ág EZEN a z-n EGYESÜL (V-csúcs); ez alatt közös szár/talp
cell_col_thick_x    = 12.0   # [mm] az oszlop VASTAGSÁGA X-ben (a középvonalra centrálva → x[−6,6]).
                             #      KORLÁT: ≤12, hogy NE érjen a +X akku-holderhez (x≥7.5; 1.5 mm hézag)

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
bat_w            = 26.0   # [mm] az akku-rekesz X-szélessége (a belső +X faltól befelé) — 24→26: a
                          #      válaszfal 2 mm-rel HÁTRÉBB (−X), így a vastagabb (2.5) fal mellett is
                          #      marad hézag: hasznos ≈ bat_w − partition_th = 23.5 mm (holder 21.5 befér)
                          #      a holder a +X strip-ben fekszik, hossza (74.5) ∥ Y
bat_cable_dia    = 8.0    # [mm] kábel-átvezető furat a válaszfalon (holder vezetékei → elektronika)
# AKKU-REKESZ Z-POZÍCIÓ: a holder+aksi a sarok-bossok TETEJE fölé ül holder_gap-pel ("csavarfejek
# pár mm-re az akku tartótól"). OLDALSÓ AKKU-AJTÓ NINCS (user: törölve) → a 18650-hez ALULRÓL, a
# bottom_lid felől férsz hozzá; a +X fal tömör. (A bat_open_*/door_* paraméterek törölve.)
holder_gap       = 2.0    # [mm] hézag a sarok-boss teteje (boss_h) és az akku-tartó alja közt
batt_z0          = boss_h + holder_gap   # [mm] az akku-rekesz (holder+aksi) ALJA z-ben (=9.0)

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
eps       = 0.5                         # [mm] boolean-tisztasághoz extra túllógás

# A sarok-boss középpontok (a 4 belső sarokban) — fő fedél rögzítéséhez (L-gusset mindkét falhoz):
corner_positions = [
    ( inner_x / 2 - corner_inset,  inner_y / 2 - corner_inset),
    (-inner_x / 2 + corner_inset,  inner_y / 2 - corner_inset),
    ( inner_x / 2 - corner_inset, -inner_y / 2 + corner_inset),
    (-inner_x / 2 + corner_inset, -inner_y / 2 + corner_inset),
]
# OLDAL-KÖZÉP boss középpontok (mind a 4 fal közepén) — extra fedél-rögzítés (user). Egy-irányú
# gusset a saját falához. (A +X fal közepe az akku-rekesz fölött van — a boss a holder ALATT, lásd lent.)
side_positions = [
    ( inner_x / 2 - side_inset, 0.0),   # +X fal közép
    (-inner_x / 2 + side_inset, 0.0),   # −X fal közép (az USB-portok y=−11/+13 közé, z-ben alá esik)
    (0.0,  inner_y / 2 - side_inset),   # +Y fal közép
    (0.0, -inner_y / 2 + side_inset),   # −Y fal közép
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

    # --- Belső VÁLASZFAL ELTÁVOLÍTVA (user: "vedd ki az elosztó falat, a csavaroktól nem fog
    #     beférni"). A 18650-rekesz nincs fizikailag leválasztva; a holder a +X térfélben marad
    #     (layout STRIP_CX). A part_x csak REFERENCIA-koordináta (a rib-ciklushoz). ---
    part_x = inner_x / 2 - bat_w

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
    # 1b) KARIMA: a pocket alatti fenék KISZÉLESÍTVE (+cell_flange_margin körbe) és LELÓGATVA
    #     (cell_flange_drop) a kavitásba → megvastagított, teherviselő talp a cella alá. A fenék
    #     alja inner_z-ről inner_z−drop-ra kerül (a 44.8×44.8 területen).
    pocket_xy = cell_size_xy + 2 * cell_clear          # a pocket oldala (34.8)
    flange_xy = pocket_xy + 2 * cell_flange_margin     # a karima oldala (44.8)
    housing += Pos(0, 0, inner_z) * Box(flange_xy, flange_xy, cell_flange_drop, align=(C, C, MAX))
    # 1c) DENT: a pocket fenekének KÖZEPÉBE egy üreg (a pocketnél cell_dent_margin-nel kisebb körbe),
    #     a pocket fenekétől lefelé cell_dent_d mélyen → IDE hajlik le a cella KÖZEPE; a SZÉLE a dent
    #     körüli peremen (pocket-fenék) marad megtámasztva. A dent alatt a karimában marad anyag.
    dent_xy = pocket_xy - 2 * cell_dent_margin         # a dent oldala (28.8)
    pocket_floor_z = housing_h - cell_recess_d         # a pocket feneke (32.5)
    housing -= Pos(0, 0, pocket_floor_z) * Box(dent_xy, dent_xy, cell_dent_d + eps, align=(C, C, MAX))
    # 2) 2 db M2.5 önmetsző rögzítőfurat a süllyesztésen KÍVÜL, a HOSSZÚ (Y) középvonalon (x=0),
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
        # M2.5 önmetsző pilot furat FELÜLRŐL lefelé (blind, a tetőn át a bossba):
        housing -= Pos(0, cy_s, housing_h) * Cylinder(
            cell_fix_dia / 2, cell_fix_depth + eps, align=(C, C, MAX)
        )

    # 3) CELLA-TÁMASZTÓ OSZLOP: ÁLLÓ "Y" a HOSSZANTI (Y-Z) SÍKBAN, az X-középvonalon (x=0). A két ÁG
    #    FELÜL a cella-ágy (pocket) Y-szélein (y=±arm_out) indul a dent fenekétől (z=col_top), lefelé a
    #    V-csúcsban (jz) EGYESÜL, onnan közös szár/talp a doboz aljáig (z=0). Az ágak Y-ban nyílnak szét
    #    (NEM X-ben) → X-ben az oszlop ±cell_col_thick_x/2-ben marad (x[−6,6]), nem ér az akku-holderhez.
    #    A profil a Y-Z síkban: az (y,z) körvonal; X-be extrudálva (both=True) a vékony vastagság.
    col_top = (housing_h - cell_recess_d) - cell_dent_d   # az oszlop teteje = a dent feneke (29.5)
    arm_out = (cell_size_xy + 2 * cell_clear) / 2         # a két ág KÜLSŐ széle (Y) = a cella-ágy széle (17.4)
    arm_in  = arm_out - cell_col_w_top                    # az ág belső éle (Y); ág-vastagság = w_top (9.4)
    wb      = cell_col_w_base / 2                         # a közös szár/talp fél-szélessége (Y, =6)
    jz      = cell_col_junction_z                         # a két ág egyesülési z-je (V-csúcs)
    # A körvonal a Y-Z síkban (Plane.YZ lokális koord: (a,b) → globális (0, a, b)):
    col_prof = Plane.YZ * Polygon(
        (-arm_out, col_top), (-wb, jz), (-wb, 0), (wb, 0), (wb, jz),
        (arm_out, col_top), (arm_in, col_top), (0, jz), (-arm_in, col_top),
    )
    # X-be extrudálva, a középvonalra centrálva (both=True) → x[−thick/2, +thick/2]:
    housing += extrude(col_prof, amount=cell_col_thick_x / 2, both=True)

    # --- (A −Y falon korábbi PG7 tömszelence-furatok ELTÁVOLÍTVA, user kérés) ---
    # A kábel-bevezetés módja (cella-kábel, DS18B20) ezzel ÚJRA NYITOTT kérdés → lásd HANDOFF.

    # --- AKKU-AJTÓ ELHAGYVA (user: "hagyjuk a battery doort, úgyse cseréljük sűrűn") ---
    # A +X hosszú fal TÖMÖR. A 18650-hez ALULRÓL (a bottom_lid levételével) férsz hozzá — az alja
    # amúgy is nyitott a lid alatt. Nincs oldalsó rebate/nyílás → a +X fal vízálló, és nem köti a
    # magasságot. (A válaszfal + akku-rekesz MEGMARAD, csak az oldalsó hozzáférés szűnt meg.)

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

    # --- FŐ FEDÉL rögzítő-bossok: 4 SAROK + 4 OLDAL-KÖZÉP (M2.5 önmetsző, pilot furat alulról) ---
    # A boss-okat a falhoz HÚZTUK (corner/side_inset=2.8) → a boss SZÉLE a falsíkba ér. A korábbi
    # téglalap-GUSSET helyett FÉLKÖR (D) COLLAR fut a boss körül: boss_collar_dia henger a horony
    # FÖLÖTT (z[gd,boss_h]), a legközelebbi falsík(ok)on túli része levágva, collar_merge-dzsel a falba
    # nyúlva (egyesülés). A horony csak z[0,gd]-ben van → a collar (z≥gd) szabadon a falba olvadhat,
    # IP65 sértetlen. A kavitásból nézve félkör-dudor; sarkon a két sík metszete (lekerekített buttress).
    gd = gasket_groove_d
    big = 50.0   # [mm] segéd-vágódoboz (a falsíkon túli collar-rész levágásához)

    def add_boss(px, py):
        """Boss + önmetsző pilot furat alulról (z=0-tól), heat-set betét nélkül."""
        b  = Pos(px, py, 0) * Cylinder(boss_dia / 2, boss_h, align=(C, C, MIN))
        h  = Pos(px, py, 0) * Cylinder(pilot_dia / 2, pilot_depth, align=(C, C, MIN))
        return b, h

    def collar(px, py, walls):
        """Félkör (D) merevítő a boss körül a horony felett (z[gd,boss_h]); a `walls`-ban megadott
        falsík(ok)on túli rész levágva, collar_merge-dzsel a falba nyúlva. walls: ('x'/'y', előjel) párok."""
        c = Pos(px, py, gd) * Cylinder(boss_collar_dia / 2, boss_h - gd, align=(C, C, MIN))
        for (ax, sign) in walls:
            if ax == "x":
                plane = sign * (inner_x / 2 + collar_merge)
                cutter = Pos(plane, 0, 0) * Box(big, big, big, align=(MIN if sign > 0 else MAX, C, C))
            else:
                plane = sign * (inner_y / 2 + collar_merge)
                cutter = Pos(0, plane, 0) * Box(big, big, big, align=(C, MIN if sign > 0 else MAX, C))
            c = c - cutter
        return c

    # SAROK-bossok: a collar MINDKÉT falsíkra vágva (a sarkot kitöltő lekerekített buttress):
    for (px, py) in corner_positions:
        sx, sy = (1 if px > 0 else -1), (1 if py > 0 else -1)
        housing += collar(px, py, [("x", sx), ("y", sy)])
        b, h = add_boss(px, py)
        housing += b
        housing -= h

    # OLDAL-KÖZÉP bossok: a collar a LEGKÖZELEBBI EGY falsíkra vágva → tiszta félkör (D):
    for (px, py) in side_positions:
        wall = ("x", 1 if px > 0 else -1) if abs(px) > abs(py) else ("y", 1 if py > 0 else -1)
        housing += collar(px, py, [wall])
        b, h = add_boss(px, py)
        housing += b
        housing -= h

    return housing


# =========================================================================================
#  2)  A L S Ó   F E D É L   ( bottom_lid )  —  sík záró lap (a PCB-fészkek a HOUSING-ban vannak)
# =========================================================================================
def build_bottom_lid():
    # Lap: a teljes külső alaprajzot lefedi, fit_gap rés a kerület mentén; z ∈ [−lid_th, 0].
    # A PCB-fészkek MÁR NEM itt vannak — a housing tartja a panelokat a tető alá lógatva.
    # lid_th vékonyítva (modul-szintű paraméter); a statikát a user végzi.
    lid = Box(outer_x - 2 * fit_gap, outer_y - 2 * fit_gap, lid_th, align=(C, C, MAX))

    # 8 átmenő csavarfurat (M2.5) a ház bossaihoz, alulról: 4 SAROK + 4 OLDAL-KÖZÉP:
    for (px, py) in corner_positions + side_positions:
        lid -= Pos(px, py, 0) * Cylinder(screw_clear_dia / 2, lid_th + 2 * eps, align=(C, C, MAX))

    return lid


# =========================================================================================
#  (A K K U - A J T Ó   T Ö R Ö L V E  — user: "hagyjuk a battery doort". A +X fal tömör,
#   a 18650-hez alulról, a bottom_lid felől férsz hozzá. Csak 2 nyomtatandó part maradt.)
# =========================================================================================


# =========================================================================================
#  Ö S S Z E Á L L Í T Á S   +   M E G J E L E N Í T É S   +   E X P O R T
# =========================================================================================
def main():
    housing = build_housing()
    bottom_lid = build_bottom_lid()

    # --- Élő viewer (ocp_vscode). Headless futásnál ne dőljön el az export miatt. ---
    try:
        import os
        from ocp_vscode import show, set_port
        # Terminálból/watch.py-ból futtatva nincs OCP_PORT env → a VS Code extension
        # alapértelmezett viewer-portjára (3939) lőjük az ábrát.
        if not os.environ.get("OCP_PORT"):
            set_port(3939)
        show(
            housing, bottom_lid,
            names=["housing", "bottom_lid"],
            colors=["#9aa0a6", "#4f8cff"],
            alphas=[0.55, 1.0],
        )
        print("[ocp_vscode] viewer frissítve.")
    except Exception as e:
        print(f"[ocp_vscode] viewer nem elérhető (headless?): {e}")

    # --- Export: STL (nyomtatás) + STEP (további CAD) minden alkatrészre ---
    out = Path(__file__).parent / "export"
    out.mkdir(exist_ok=True)
    parts = {"housing": housing, "bottom_lid": bottom_lid}
    for name, part in parts.items():
        export_stl(part, str(out / f"{name}.stl"))
        export_step(part, str(out / f"{name}.step"))
        print(f"[export] {name}.stl + {name}.step")

    # Teljes összeállítás STEP-ben (CAD-ellenőrzéshez):
    asm = Compound(children=[housing, bottom_lid])
    export_step(asm, str(out / "assembly.step"))
    print("[export] assembly.step")
    print(f"\nKész. Export ide: {out}")


if __name__ == "__main__":
    main()

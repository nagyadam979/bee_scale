"""
preview.py — GYORS VIZUÁLIS ELLENŐRZŐ ESZKÖZ (a "szem")
=======================================================
CÉL: egyetlen paranccsal, STL-EXPORT NÉLKÜL (közvetlenül a build123d testből → gyors) egy PNG-t
generál, amin a ház perspektivikus nézete + a KRITIKUS TÖMÖR KERESZTMETSZETEK láthatók. Ezt KELL
futtatni MINDEN geometriai változtatás után, MIELŐTT bármit késznek mondunk — így nem lehet vakon
mellédolgozni (lásd HANDOFF: "minden látványbeli változás után nézd meg").

Futtatás:
    python preview.py                 # alap: izo + felül + X=0 és X=11 metszet
    python preview.py 7.9             # +extra X-sík metszet 7.9-nél
    python preview.py 0 6 11 -11      # tetszőleges X-síkok (a cella-ágy/oszlop vizsgálatához)

Kimenet: export/preview.png  (ezt Read-eli az agent és nézi a user is). NEM ír STL/STEP-et — az az
enclosure.py dolga, csak amikor tényleg nyomtatni/CAD-elni kell.

Miért tömör METSZET és nem féltér-vetítés: a build123d sík-metszet (intersect + tessellate) VALÓDI
zárt cap-et ad (mint az ocp-vscode Clip), így a lépcsők/ívek/falvastagságok pontosan leolvashatók.
"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

import enclosure as e
from build123d import Plane

TESS_TOL = 0.25      # tessellation tűrés [mm] — kisebb = finomabb mesh, lassabb
CELL = 640           # egy nézet-cella oldalmérete px
SS = 2               # supersampling


# ---------------------------------------------------------------- segéd: betűtípus
def _font(sz):
    try:
        return ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", sz)
    except Exception:
        return ImageFont.load_default()


# ---------------------------------------------------------------- mesh egy testből
def mesh_of(part):
    """build123d test → (tris(n,3,3)) world-koordinátában, STL nélkül."""
    verts, faces = part.tessellate(TESS_TOL)
    V = np.array([(v.X, v.Y, v.Z) for v in verts])
    F = np.array(faces)
    return V[F]   # (n,3,3)


# ---------------------------------------------------------------- perspektív nézet (painter's)
def view_persp(parts, elev, azim, title):
    """parts: [(tris, rgb), ...]. Egy jelenetbe rendezve, flat shading, painter's algoritmus."""
    a, el = np.radians(azim), np.radians(elev)
    Rz = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
    Rx = np.array([[1, 0, 0], [0, np.cos(el), -np.sin(el)], [0, np.sin(el), np.cos(el)]])
    R = Rx @ Rz
    light = np.array([0.35, -0.55, 0.75]); light /= np.linalg.norm(light)

    tris = np.concatenate([t for t, _ in parts])
    base = np.concatenate([np.tile(c, (len(t), 1)) for t, c in parts])
    cam = tris @ R.T
    px, py, depth = cam[:, :, 0], cam[:, :, 2], cam[:, :, 1].mean(1)
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    ln = np.linalg.norm(n, axis=1, keepdims=True); ln[ln == 0] = 1
    bright = np.abs((n / ln) @ light) * 0.72 + 0.28

    W2 = CELL * SS
    cx, cy = (px.min() + px.max()) / 2, (py.min() + py.max()) / 2
    span = max(px.max() - px.min(), py.max() - py.min()) * 1.1
    sc = W2 / span
    sx = (px - cx) * sc + W2 / 2
    sy = W2 / 2 - (py - cy) * sc
    img = Image.new("RGB", (W2, W2), (250, 250, 251))
    dr = ImageDraw.Draw(img)
    for i in np.argsort(depth)[::-1]:
        col = tuple(np.clip(base[i] * bright[i], 0, 255).astype(int))
        dr.polygon([(sx[i, k], sy[i, k]) for k in range(3)], fill=col, outline=(45, 45, 50))
    img = img.resize((CELL, CELL), Image.LANCZOS)
    ImageDraw.Draw(img).text((8, 6), title, fill=(0, 0, 0), font=_font(15))
    return img


# ---------------------------------------------------------------- tömör keresztmetszet
def view_section(housing, xc, ybounds, zbounds):
    """Valódi tömör X-metszet: housing ∩ {x=xc} sík → tessellált Face-ek → 2D (Y,Z) fill."""
    sec = housing.intersect(Plane(origin=(xc, 0, 0), z_dir=(1, 0, 0)))
    faces = list(sec) if hasattr(sec, "__iter__") else [sec]
    Vs, Fs, off = [], [], 0
    for f in faces:
        try:
            vv, ff = f.tessellate(TESS_TOL)
        except Exception:
            continue
        Vs.extend([(v.Y, v.Z) for v in vv])
        Fs.extend([(a + off, b + off, c + off) for (a, b, c) in ff])
        off += len(vv)
    img = Image.new("RGB", (CELL, CELL), (250, 250, 251))
    dr = ImageDraw.Draw(img)
    if Vs:
        V = np.array(Vs)
        (y0, y1), (z0, z1) = ybounds, zbounds
        cy, cz = (y0 + y1) / 2, (z0 + z1) / 2
        span = max(y1 - y0, z1 - z0) * 1.1
        sc = (CELL - 40) / span
        sy = (V[:, 0] - cy) * sc + CELL / 2
        sz = CELL / 2 - (V[:, 1] - cz) * sc
        for (i, j, k) in Fs:
            dr.polygon([(sy[i], sz[i]), (sy[j], sz[j]), (sy[k], sz[k])],
                       fill=(235, 120, 50), outline=(150, 75, 25))
    dr.text((8, 6), f"METSZET  X={xc:g} mm", fill=(0, 0, 0), font=_font(15))
    return img


def main():
    extra = [float(a) for a in sys.argv[1:]] if len(sys.argv) > 1 else None
    x_cuts = extra if extra is not None else [0.0, 11.0]

    housing = e.build_housing()
    lid = e.build_bottom_lid()
    h_tris, l_tris = mesh_of(housing), mesh_of(lid)
    GREY, BLUE = np.array([158, 162, 166]), np.array([80, 140, 255])
    scene = [(h_tris, GREY), (l_tris, BLUE)]

    # közös metszet-skála: a ház teljes Y-Z befoglalója
    bb = housing.bounding_box()
    yb, zb = (bb.min.Y, bb.max.Y), (bb.min.Z, bb.max.Z)

    tiles = [
        view_persp(scene, 25, 35, "Izometrikus"),
        view_persp(scene, 88, 0, "Felulnezet (cella-plato)"),
    ]
    tiles += [view_section(housing, xc, yb, zb) for xc in x_cuts]

    cols = min(len(tiles), 4)
    rows = (len(tiles) + cols - 1) // cols
    pad, hdr = 8, 40
    W = cols * CELL + (cols + 1) * pad
    H = hdr + rows * CELL + (rows + 1) * pad
    canvas = Image.new("RGB", (W, H), (255, 255, 255))
    dr = ImageDraw.Draw(canvas)
    dr.text((pad, 10), f"bee_scale preview  |  bbox {bb.size.X:.1f}x{bb.size.Y:.1f}x{bb.size.Z:.1f} mm"
                       f"  |  metszetek: X={', '.join(f'{x:g}' for x in x_cuts)}",
            fill=(20, 20, 20), font=_font(20))
    for idx, im in enumerate(tiles):
        r, c = divmod(idx, cols)
        canvas.paste(im, (pad + c * (CELL + pad), hdr + pad + r * (CELL + pad)))
    out = e.Path(__file__).parent / "export" / "preview.png"
    out.parent.mkdir(exist_ok=True)
    canvas.save(out)
    print(f"[preview] {out}  ({W}x{H})  metszetek: {x_cuts}")


if __name__ == "__main__":
    main()

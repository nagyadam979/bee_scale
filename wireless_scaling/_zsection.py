"""Célzott Z-metszet: a két bosst (±Y) KÜLÖN, NAGYÍTVA, a -Y-t tükrözve rajzolja,
hogy a nyak-trapéz formája + a +Y/-Y szimmetria egyértelmű legyen."""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from build123d import Plane
import enclosure as E

CELL = 520
TESS_TOL = 0.03

def _font(sz):
    try:
        return ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", sz)
    except Exception:
        return ImageFont.load_default()

def collect(sec):
    faces = list(sec) if hasattr(sec, "__iter__") else [sec]
    Vs, Fs, off = [], [], 0
    for f in faces:
        try:
            vv, ff = f.tessellate(TESS_TOL)
        except Exception:
            continue
        Vs.extend([(v.X, v.Y) for v in vv])
        Fs.extend([(a + off, b + off, c + off) for (a, b, c) in ff])
        off += len(vv)
    return np.array(Vs), Fs

def panel(V, Fs, title, flip_y):
    img = Image.new("RGB", (CELL, CELL), (250, 250, 251))
    dr = ImageDraw.Draw(img)
    HALF = 13.0
    cy = 21.45
    yy = V[:, 1].copy()
    if flip_y:
        yy = -yy
    cx = 0.0
    sc = (CELL - 40) / (2 * HALF)
    sx = (V[:, 0] - cx) * sc + CELL / 2
    sy = CELL / 2 - (yy - cy) * sc
    for (i, j, k) in Fs:
        dr.polygon([(sx[i], sy[i]), (sx[j], sy[j]), (sx[k], sy[k])],
                   fill=(235, 120, 50), outline=(150, 75, 25))
    x0p = (0 - cx) * sc + CELL / 2
    dr.line([(x0p, 0), (x0p, CELL)], fill=(40, 40, 220), width=1)
    yb_cy = CELL / 2
    dr.line([(0, yb_cy), (CELL, yb_cy)], fill=(0, 150, 0), width=1)
    band = 17.9
    yb_band = CELL / 2 - (band - cy) * sc
    dr.line([(0, yb_band), (CELL, yb_band)], fill=(170, 0, 170), width=1)
    r = 4.4 * sc
    dr.ellipse([CELL/2 - r, yb_cy - r, CELL/2 + r, yb_cy + r], outline=(0, 120, 200), width=2)
    dr.text((8, 6), title, fill=(0, 0, 0), font=_font(14))
    dr.text((8, CELL-22), "zold=boss Y  lila=plato-el  kek kor=collar_r 4.4",
            fill=(0, 0, 0), font=_font(11))
    return img

def main():
    Zc = float(sys.argv[1]) if len(sys.argv) > 1 else 23.5
    housing = E.build_housing()
    V, Fs = collect(housing.intersect(Plane(origin=(0, 0, Zc), z_dir=(0, 0, 1))))
    pPos = panel(V, Fs, "+Y boss z=%g" % Zc, flip_y=False)
    pNeg = panel(V, Fs, "-Y boss (tukrozve) z=%g" % Zc, flip_y=True)
    out_img = Image.new("RGB", (CELL * 2 + 10, CELL), (255, 255, 255))
    out_img.paste(pPos, (0, 0))
    out_img.paste(pNeg, (CELL + 10, 0))
    out = "export/boss_neck_zsection.png"
    out_img.save(out)
    print("saved", out)

if __name__ == "__main__":
    main()

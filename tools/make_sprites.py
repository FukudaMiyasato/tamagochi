"""Convierte la hoja de la mascota (assets/mascota.webp, 5x3 expresiones) en:
  - firmware/main/sprites.bin : 15 sprites RGB565 ya mezclados con el fondo de la pantalla
  - public/sprites/NN.png     : los mismos sprites con transparencia, para la web

Cada sprite queda con el cuerpo en la misma posición, para que al cambiar de
expresión la mascota no salte; los adornos (estrellas, zzz, corazones) van
alrededor.

Uso: python tools/make_sprites.py   (requiere pillow, numpy, scipy)
"""
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parent.parent
COUNT = 15
W, H = 176, 150            # lienzo de cada sprite en la pantalla de 240x240
BASE_W = 92                # ancho de la parte baja del vaso en pantalla
BODY_CX, BODY_BOTTOM = 84, 142  # dónde quedan el centro y la base del vaso en el lienzo
BG = (0x0B, 0x10, 0x20)    # fondo de la pantalla (COL_BG en render.c)

sheet = Image.open(ROOT / "assets" / "mascota.webp").convert("RGBA")
rgba_sheet = np.asarray(sheet)
rgb = rgba_sheet[..., :3]
alpha = rgba_sheet[..., 3]

# La imagen ya trae fondo transparente: lo sólido es lo que tiene opacidad.
solid = ndimage.binary_opening(alpha > 60, iterations=1)

# Piezas sueltas: las 15 más grandes son los cuerpos; cada adorno se asigna al
# cuerpo más cercano.
parts, n = ndimage.label(solid)
sizes = ndimage.sum(solid, parts, range(1, n + 1))
boxes = ndimage.find_objects(parts)
order = np.argsort(sizes)[::-1]
bodies = sorted(order[:COUNT] + 1, key=lambda l: (round(boxes[l - 1][0].start / 200), boxes[l - 1][1].start))
assert len(bodies) == COUNT

def box_distance(a, b):
    dy = max(b[0].start - a[0].stop, a[0].start - b[0].stop, 0)
    dx = max(b[1].start - a[1].stop, a[1].start - b[1].stop, 0)
    return (dx * dx + dy * dy) ** 0.5

owner = {int(b): [int(b)] for b in bodies}
for lab in order[COUNT:] + 1:
    if sizes[lab - 1] < 12:
        continue  # motas
    nearest = min(bodies, key=lambda b: box_distance(boxes[lab - 1], boxes[b - 1]))
    owner[int(nearest)].append(int(lab))

# Referencia para alinear: el vaso. Se mide en la parte baja del cuerpo (donde
# nunca hay adornos pegados), no en la caja completa: si un adorno toca el
# cuerpo, la caja crece hacia ese lado y la mascota quedaría descentrada.
def cup(body):
    by, bx = boxes[body - 1]
    rows = parts[by, bx] == body
    low = rows[int(rows.shape[0] * 0.70):int(rows.shape[0] * 0.95)]
    cols = np.nonzero(low.any(axis=0))[0]
    return bx.start + (cols[0] + cols[-1] + 1) / 2, by.stop, cols[-1] - cols[0] + 1

cups = {int(b): cup(b) for b in bodies}
scale = BASE_W / np.median([c[2] for c in cups.values()])
src_w, src_h = round(W / scale), round(H / scale)

out = bytearray()
(ROOT / "public" / "sprites").mkdir(parents=True, exist_ok=True)
clipped = 0
for i, body in enumerate(bodies):
    mask = np.isin(parts, owner[int(body)])
    cx, bottom, _ = cups[int(body)]
    left, top = round(cx - BODY_CX / scale), round(bottom - BODY_BOTTOM / scale)

    # Opacidad original, pero solo de las piezas de esta mascota (un poco
    # ensanchadas para conservar el borde suave).
    near = ndimage.binary_dilation(mask, iterations=3)
    rgba = np.dstack([rgb, np.where(near, alpha, 0)]).astype(np.uint8)
    # Cuenta lo que quede fuera del lienzo, para avisar.
    ys, xs = np.nonzero(mask)
    clipped += int(((xs < left) | (xs >= left + src_w) | (ys < top) | (ys >= top + src_h)).sum())
    cell = Image.fromarray(rgba, "RGBA").crop((left, top, left + src_w, top + src_h))
    sprite = cell.resize((W, H), Image.LANCZOS)
    sprite.save(ROOT / "public" / "sprites" / f"{i:02d}.png")

    flat = Image.new("RGBA", (W, H), BG + (255,))
    flat.alpha_composite(sprite)
    px = np.asarray(flat.convert("RGB")).astype(np.uint16)
    v = ((px[..., 0] & 0xF8) << 8) | ((px[..., 1] & 0xFC) << 3) | (px[..., 2] >> 3)
    out += v.astype(">u2").tobytes()   # big-endian: el orden que espera la pantalla

(ROOT / "firmware" / "main" / "sprites.bin").write_bytes(out)
print(f"{COUNT} sprites de {W}x{H}, escala {scale:.3f}, {len(out)} bytes, {clipped} px de adornos recortados")

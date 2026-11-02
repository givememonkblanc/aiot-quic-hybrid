#!/usr/bin/env python3
"""Dark objects on a bright plate: the detector of the device, in Python.

The frame is reduced to 200x150 as the device decodes it (scale 1/8 of 1600x1200). A pixel
is dark when its luma is below a fraction of the median luma of the frame. Connected dark
regions whose area lies between the limits are objects; larger regions are the background
outside the plate or a fixture, smaller ones are noise. Coordinates are returned in the
units of the device result, 0..255 over the width and the height."""

import numpy as np
from PIL import Image

WIDTH, HEIGHT = 200, 150
DARK_FRACTION = 0.55
AREA_MIN, AREA_MAX = 3, 80


def luma(path):
    return np.asarray(Image.open(path).convert("L").resize((WIDTH, HEIGHT), Image.BILINEAR), dtype=np.float32)


def detect(gray, fraction=DARK_FRACTION, area_min=AREA_MIN, area_max=AREA_MAX):
    """Returns [(x, y, w, h, area)] with x, y the centre, all in 0..255 units."""
    dark = gray < fraction * float(np.median(gray))
    seen = np.zeros_like(dark)
    out = []
    h, w = dark.shape
    for y0 in range(h):
        for x0 in range(w):
            if not dark[y0, x0] or seen[y0, x0]:
                continue
            stack, pts = [(y0, x0)], []
            seen[y0, x0] = True
            while stack:
                y, x = stack.pop()
                pts.append((y, x))
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < h and 0 <= nx < w and dark[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
            if not area_min <= len(pts) <= area_max:
                continue
            ys, xs = zip(*pts)
            out.append(((min(xs) + max(xs)) / 2 * 256 / w, (min(ys) + max(ys)) / 2 * 256 / h,
                        (max(xs) - min(xs) + 1) * 256 / w, (max(ys) - min(ys) + 1) * 256 / h, len(pts)))
    return out

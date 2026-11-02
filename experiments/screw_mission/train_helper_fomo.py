#!/usr/bin/env python3
"""Screw model of the helper (an idle AIoT that the camera asks when a sample is hard): the count model of
train_turntable_fomo2.py at twice the resolution of the camera, on a band around the counting line.

Why (10/10, scene 3): the camera decodes at 1/8 (200x150) into a 20x15 grid over its region; screws closer
than one cell at the line (a cluster of three, a close pair, a pair touching on the rim) give one maximum
and the count head mostly says 1. Recounted at 400x300 they part (evidence_recount_sim.py). The helper gets
the frame over MoQ (offload/jpeg) and has the time of a whole inference for one frame.

- Input: the JPEG decoded at 1/4 (400x300, PIL draft as the camera's 1/8; luma as replay_workers), the band
  BAND of the frame (x 0.42-0.62, y 0.3-0.7: 80x120 pixels), cells of 4 pixels: a 20x30 grid, cells half the
  size of the camera's. Same network ("fast" + count head, train_turntable_fomo2.Net); twice the pixels of
  the camera's input, so about twice its inference time.
- Everything else as train_turntable_fomo2.py (augmentation, count head, selection, int8 export).

usage: train_helper_fomo.py (the arguments of train_turntable_fomo2.py)"""
import json
import sys

import numpy as np
from PIL import Image

import train_turntable_fomo2 as F
import train_turntable_fomo as T

BAND = (0.42, 0.3, 0.62, 0.7)
FRAME = (400, 300)


def luma4(path):
    im = Image.open(path)
    im.draft("RGB", (im.size[0] // 4, im.size[1] // 4))
    if im.size != FRAME:
        im = im.resize(FRAME, Image.BILINEAR)
    rgb = np.asarray(im.convert("RGB"), np.int32)
    r, g, b = rgb[..., 0] >> 3, rgb[..., 1] >> 2, rgb[..., 2] >> 3
    r8, g8, b8 = r * 255 // 31, g * 255 // 63, b * 255 // 31
    return (r8 * 77 + g8 * 150 + b8 * 29) >> 8


def main():
    T.DESIGNS["fast"] = dict(T.DESIGNS["fast"], frame=FRAME)
    T.ROI = BAND
    F.Detector.luma = staticmethod(luma4)
    F.main()
    out = sys.argv[sys.argv.index("--out") + 1]
    p = F.Path(out) / "training.json"
    t = json.loads(p.read_text())
    t.update(design="helper: fast+count at 1/4", frame=list(FRAME), band=list(BAND))
    p.write_text(json.dumps(t, indent=1) + "\n")


if __name__ == "__main__":
    main()

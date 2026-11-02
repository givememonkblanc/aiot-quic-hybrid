#!/usr/bin/env python3
"""Detector of a model peer (emu_camera.py --peer): the camera's detector (replay_workers.Detector, model-c3) on the frames the
relay gives the peer, the 200x150 raster JPEG of the split inference (already at 1/8: no further scaling).

Protocol on stdin/stdout: a 4-byte big-endian length then the JPEG; one JSON line back, {"dets": [[x, y], ...], "us": compute}.
Runs under the Python that has the TFLite interpreter (miniforge), apart from the QUIC process."""
import io
import json
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from replay_workers import Detector  # noqa: E402

MODEL = Path("/home/ryzen395/data/aiot_quic_results/screw_turntable/model-c3/screw_turntable_int8.tflite")


class RasterDetector(Detector):
    @staticmethod
    def luma(data):
        rgb = np.asarray(Image.open(io.BytesIO(data)).convert("RGB"), np.int32)
        r, g, b = rgb[..., 0] >> 3, rgb[..., 1] >> 2, rgb[..., 2] >> 3
        r8, g8, b8 = r * 255 // 31, g * 255 // 63, b * 255 // 31
        return (r8 * 77 + g8 * 150 + b8 * 29) >> 8


def main():
    det = RasterDetector(MODEL)
    inp, out = sys.stdin.buffer, sys.stdout
    while True:
        head = inp.read(4)
        if len(head) < 4:
            return
        (n,) = struct.unpack(">I", head)
        data = inp.read(n)
        t = time.perf_counter()
        try:
            dets = [[int(x), int(y)] for x, y in det(data)][:8]
        except Exception as e:  # a broken frame: no detections, reported
            out.write(json.dumps(dict(dets=[], error=str(e)[:80], us=0)) + "\n")
            out.flush()
            continue
        out.write(json.dumps(dict(dets=dets, us=int((time.perf_counter() - t) * 1e6))) + "\n")
        out.flush()


if __name__ == "__main__":
    main()

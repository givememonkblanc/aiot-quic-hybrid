#!/usr/bin/env python3
"""Freeze an aligned JPEG ladder for device replay; encoding is OFFLINE.
Does not measure live re-encoding cost or create event ground truth.
"""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image


def prepare(source, out):
    out.mkdir(parents=True, exist_ok=False)
    original = source.read_bytes()
    image = Image.open(source).convert('RGB')
    (out / 'high.jpg').write_bytes(original)
    levels = [('high', image.width, image.height, None)]
    for name, width, quality in [('medium', 800, 75), ('low', 400, 65)]:
        width = min(width, image.width)
        height = round(image.height * width / image.width)
        image.resize((width, height), Image.Resampling.LANCZOS).save(
            out / (name + '.jpg'), quality=quality, optimize=False)
        levels.append((name, width, height, quality))
    manifest = dict(schema=1, scope='static identical-source replay, offline encoding',
                    source=str(source.resolve()), representations=[])
    for name, width, height, quality in levels:
        data = (out / (name + '.jpg')).read_bytes()
        manifest['representations'].append(dict(name=name, width=width, height=height,
            quality=quality, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('source', type=Path)
    p.add_argument('out', type=Path)
    a = p.parse_args()
    print(json.dumps(prepare(a.source, a.out), indent=2))

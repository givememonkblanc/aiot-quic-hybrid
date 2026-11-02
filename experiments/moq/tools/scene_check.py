#!/usr/bin/env python3
"""Scene of a run: the recall of the camera's own count (the device counter of serial.log, judged on the
slot template as eval_laptop.py judges it). It does not depend on the method, which changes what is sent,
not what the camera infers; at night the room is lit by a screen whose colour and brightness change
(10/10, of3c: 0.40 to 0.76 from run to run), more than the methods differ.

Split inference (serial [AI_SPLIT], 10/13): the camera infers only some of the frames itself and the helper
the others, so the device counter sees fewer samples than in any other method and its recall says less
about the scene than about the method. For such a run the scene is the recall of the count over everything
inferred, the camera's results and the helper's at their capture times (eval_transport.produced, the
reference of eval_transport.py), through the same fuser and template.
usage: scene_check.py RUN TEMPLATE   -> prints the recall (0 when no rotation is found)"""
import json
import sys
import tempfile
from pathlib import Path

import analyze_rotations as A
from eval_laptop import judge

run, tp = Path(sys.argv[1]), A.Template(Path(sys.argv[2]))
serial = run / "serial.log"
if "[AI_SPLIT]" in serial.read_text(errors="replace"):
    import replay_fuser as R
    from eval_transport import produced
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        f.write("".join(json.dumps(r) + "\n" for r in produced(run)))
    fused, _, _, _ = R.replay(f.name, 1000, 1000, "fit-assoc", False)
    Path(f.name).unlink()
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        f.write("".join(json.dumps(o) + "\n" for o in fused))
    rows, span = A.load(f.name)
    Path(f.name).unlink()
    r = A.analyze(rows, span, tp, 0.16) if rows else None
    print(f"{(r or {}).get('slot_recall') or 0:.3f}")
else:
    d = judge(serial, tp, 0.16)
    print(f"{d.get('slot_recall') or 0:.3f}")

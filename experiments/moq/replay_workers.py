#!/usr/bin/env python3
"""Offline replay of on-device inference policies over a full-rate camera trace.

A trace run (run_moq ... trace) keeps every captured frame that reached the host
(<run>/trace/frames/<seq>.jpg, index.csv: seq, capture_us, bytes, host time) and the serial
log of the device (MOQ_CAPTURE, AI_START, AI_PROFILE, MOQ_TRACK, MOQ_PASSAGE).

1. validate  the host detector (device preprocessing: 1/8 DCT decode, RGB565 luma, ROI, int8
             model, FOMO peaks) against the device's own detections (MOQ_TRACK pts) on the frames
             the device inferred.
2. truth     detect on every trace frame, track the screws frame to frame and take the time at
             which each crosses the counting line (ground-truth crossings).
3. device    match the device's MOQ_PASSAGE events to the crossings; classify each miss.
4. replay    K identical workers with latest-frame admission (K=1,2,3), a periodic policy and a
             mission-aware policy, each feeding the passage counter of the device
             (screw_mission/passage.py) in capture-time order; recall against the crossings.

usage: replay_workers.py RUN_DIR [--model M.tflite] [--seed 1]
Writes RUN_DIR/replay_summary.json."""

import argparse
import bisect
import csv
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "screw_mission"))
import passage  # noqa: E402  the device counter, in Python

MODEL = Path("/home/ryzen395/data/aiot_quic_results/screw_turntable/model-f1/screw_turntable_int8.tflite")
ROI = (300, 300, 700, 700)          # AIOT_AI_ROI_BOX, thousandths
THRESHOLD = 77                      # detection_threshold of screw_model_asset.h
MAX_DET = 24                        # AIOT_AI_RESULT_MAX_DETECTIONS
LINE = 128
COUNTER = dict(line=LINE, direction=1, max_step=48, max_missed=2, predict=True, count_unseen=True, settled=True)


def night_luma(y, roi=None, day=153):
    """AIOT_AI_LUMA_NORM of ai_inference_tflm.cc: the rows of the region moved by the difference of their
    median to the median of the frame, then the region scaled so that its median is `day`, never darker
    (medians: the element n//2 of the sorted values, as hist_median)."""
    roi = roi or ROI
    y = np.asarray(y, np.int32).copy()
    hh, ww = y.shape
    rx, ry = ww * roi[0] // 1000, hh * roi[1] // 1000
    rw, rh = ww * roi[2] // 1000 - rx, hh * roi[3] // 1000 - ry
    med = lambda a: int(np.sort(a, axis=None)[a.size // 2])
    fm = med(y)
    rows = y[ry:ry + rh]
    shift = fm - np.sort(rows, axis=1)[:, ww // 2]
    y[ry:ry + rh] = np.clip(rows + shift[:, None], 0, 255)
    r = y[ry:ry + rh, rx:rx + rw]
    m = med(r)
    gain = day * 256 // m if 0 < m < day else 256
    y[ry:ry + rh, rx:rx + rw] = np.minimum((r * gain) >> 8, 255)
    return y


# ----------------------------------------------------------------------------- detector
class Detector:
    """The device pipeline: the UXGA JPEG decoded at 1/8 (200x150, DC-only), RGB565, luma
    (77R+150G+29B)>>8, the ROI 60..140 x 45..105 sampled nearest, int8 quantized as
    ai_inference_tflm.cc does, FOMO peaks over confidence = (uint8)(score*255) >= 77."""

    def __init__(self, path=MODEL):
        try:
            from ai_edge_litert.interpreter import Interpreter
        except ImportError:
            from tensorflow.lite import Interpreter
        self.it = Interpreter(model_path=str(path), num_threads=1)
        self.it.allocate_tensors()
        self.inp, self.out = self.it.get_input_details()[0], self.it.get_output_details()[0]
        _, self.h, self.w, _ = self.inp["shape"]

    @staticmethod
    def luma(path):
        im = Image.open(path)
        im.draft("RGB", (im.size[0] // 8, im.size[1] // 8))
        rgb = np.asarray(im.convert("RGB"), np.int32)
        r, g, b = rgb[..., 0] >> 3, rgb[..., 1] >> 2, rgb[..., 2] >> 3
        r8, g8, b8 = r * 255 // 31, g * 255 // 63, b * 255 // 31
        return (r8 * 77 + g8 * 150 + b8 * 29) >> 8

    def __call__(self, path):
        y = self.luma(path)
        hh, ww = y.shape
        rx, ry = ww * ROI[0] // 1000, hh * ROI[1] // 1000
        rw, rh = ww * ROI[2] // 1000 - rx, hh * ROI[3] // 1000 - ry
        sy = ry + (np.arange(self.h) * rh) // self.h
        sx = rx + (np.arange(self.w) * rw) // self.w
        crop = y[np.ix_(sy, sx)].astype(np.float32)
        scale, zero = self.inp["quantization"]
        q = np.clip(np.floor(crop / 255.0 / scale + 0.5).astype(np.int32) + zero, -128, 127).astype(np.int8)
        self.it.set_tensor(self.inp["index"], q[None, :, :, None])
        self.it.invoke()
        o = self.it.get_tensor(self.out["index"])[0].astype(np.float32)
        oscale, ozero = self.out["quantization"]
        score = np.clip((o[..., 1] - ozero) * oscale, 0, 1)
        conf = (score.astype(np.float32) * np.float32(255)).astype(np.int32)
        return peaks(conf)


def peaks(conf):
    gh, gw = conf.shape
    x0, y0 = 256 * ROI[0] // 1000, 256 * ROI[1] // 1000
    xw, yh = 256 * ROI[2] // 1000 - x0, 256 * ROI[3] // 1000 - y0
    det = []   # (conf, x, y) sorted by confidence, stable in reading order as insert_fomo_detection
    for y in range(gh):
        for x in range(gw):
            c = conf[y, x]
            if c < THRESHOLD or c == 0:
                continue
            ok = True
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if (dx or dy) and 0 <= nx < gw and 0 <= ny < gh:
                        n = conf[ny, nx]
                        if n > c or ((ny * gw + nx) < (y * gw + x) and n == c):
                            ok = False
            if ok:
                det.append((int(c), x0 + (2 * x + 1) * xw // (2 * gw), y0 + (2 * y + 1) * yh // (2 * gh)))
    out = []
    for d in det:
        i = next((k for k, e in enumerate(out) if d[0] > e[0]), len(out))
        if i < MAX_DET:
            out.insert(i, d)
            del out[MAX_DET:]
    return [(x, y) for _, x, y in out]


# ----------------------------------------------------------------------------- inputs
def log_events(run, tag):
    out = []
    for line in (run / "serial.log").read_text(errors="replace").splitlines():
        k = line.find(f"[{tag}] ")
        if k >= 0:
            try:
                out.append(json.loads(line[k + len(tag) + 3:]))
            except json.JSONDecodeError:
                pass
    return out


def load_trace(run):
    rows = []
    with open(run / "trace" / "index.csv") as f:
        for r in csv.reader(f):
            if r and r[0].isdigit():
                rows.append((int(r[0]), int(r[1]) / 1e6))
    rows.sort(key=lambda r: r[1])
    return rows


# ----------------------------------------------------------------------------- ground truth
def track_truth(frames, det, gate=16, max_missed=4):
    """Nearest-neighbour tracks over every trace frame with a constant-velocity prediction.
    A crossing is a track whose x goes from < LINE to >= LINE; its time and y are interpolated
    between the two observations. Returns crossings and the tracks (lists of (t, x, y))."""
    tracks, done, crossings = [], [], []
    flow = None   # units per second, along +x
    nid = 0
    for seq, t in frames:
        pts = list(det[seq])
        cand = []
        for ti, tr in enumerate(tracks):
            lt, lx, ly = tr["obs"][-1]
            v = tr["v"] if tr["v"] is not None else (flow if flow is not None else 0.0)
            px = lx + v * (t - lt)
            for pi, (x, y) in enumerate(pts):
                d = max(abs(x - px), abs(y - ly))
                if d <= gate + 4 * tr["missed"] and x - lx >= -6:
                    cand.append((d, ti, pi))
        cand.sort()
        used_t, used_p = set(), set()
        for d, ti, pi in cand:
            if ti in used_t or pi in used_p:
                continue
            used_t.add(ti); used_p.add(pi)
            tr = tracks[ti]
            lt, lx, ly = tr["obs"][-1]
            x, y = pts[pi]
            if t > lt:
                v = (x - lx) / (t - lt)
                tr["v"] = v if tr["v"] is None else 0.6 * tr["v"] + 0.4 * v
                flow = v if flow is None else 0.95 * flow + 0.05 * v
            if lx < LINE <= x:
                f = (LINE - lx) / max(x - lx, 1e-9)
                crossings.append(dict(t=lt + f * (t - lt), y=ly + f * (y - ly), track=tr["id"],
                                      before=(lt, lx, ly), after=(t, x, y)))
            tr["obs"].append((t, x, y)); tr["missed"] = 0
        keep = []
        for ti, tr in enumerate(tracks):
            if ti not in used_t:
                tr["missed"] += 1
                if tr["missed"] > max_missed:
                    done.append(tr); continue
            keep.append(tr)
        tracks = keep
        for pi, (x, y) in enumerate(pts):
            if pi not in used_p:
                nid += 1
                tracks.append(dict(id=nid, obs=[(t, x, y)], v=None, missed=0))
    done += tracks
    crossings.sort(key=lambda c: c["t"])
    return crossings, done, flow


def period_estimate(times, lo=30.0, hi=40.0, tol=0.3):
    """Coarse period: the shift that maps the most crossings onto another crossing."""
    times = np.asarray(times)
    best = None
    for p in np.arange(lo, hi, 0.01):
        shifted = times + p
        i = np.clip(np.searchsorted(times, shifted), 1, len(times) - 1)
        d = np.minimum(abs(times[i] - shifted), abs(times[i - 1] - shifted))
        ok = int((d <= tol).sum())
        if best is None or ok > best[1]:
            best = (float(p), ok)
    return best[0]


def periodic_truth(crossings, t0, t1, period, dphi=0.45, dy=10):
    """The screws pass the line in the same order every rotation. The tracked crossings are
    grouped into slots by (phase, y); a slot seen in at least half of the rotations is a screw.
    The period is refitted from the slots (least squares of time on rotation index). For each
    rotation a slot is the tracked crossing when there is one, else it is imputed at the phase
    of the slot (a frame-to-frame tracking or detection miss of the truth itself).
    Returns (truth crossings, period, slots, rotation start)."""
    ref = crossings[0]["t"] - 5.0   # phases away from the wrap: the bursts are ~15 s of each period
    for _ in range(3):
        items = sorted(((c["t"] - ref) % period, c["y"], int((c["t"] - ref) // period), c) for c in crossings)
        slots = []
        for phi, y, k, c in items:
            for sl in slots:
                if abs(phi - sl["phi"]) <= dphi and abs(y - sl["y"]) <= dy and k not in sl["members"]:
                    sl["members"][k] = c
                    n = len(sl["members"])
                    sl["phi"] += (phi - sl["phi"]) / n; sl["y"] += (y - sl["y"]) / n
                    break
            else:
                slots.append(dict(phi=phi, y=y, members={k: c}))
        rotations = int((t1 - ref) // period) + 1
        slots = [sl for sl in slots if len(sl["members"]) >= max(2, rotations // 2)]
        # common period: t = ref + phi_s + k * P
        num = den = 0.0
        for sl in slots:
            ks = np.array(sorted(sl["members"]), float)
            ts = np.array([sl["members"][int(k)]["t"] for k in ks])
            if len(ks) >= 2:
                kc = ks - ks.mean()
                num += (kc * (ts - ts.mean())).sum(); den += (kc * kc).sum()
        if den:
            period = num / den
    for sl in slots:
        sl["phi"] = float(np.mean([c["t"] - ref - k * period for k, c in sl["members"].items()]))
        sl["y"] = float(np.mean([c["y"] for c in sl["members"].values()]))
    truth = []
    kmin = int(np.floor((t0 - ref) / period)) - 1
    for k in range(kmin, int((t1 - ref) // period) + 2):
        for si, sl in enumerate(slots):
            t = ref + sl["phi"] + k * period
            if not t0 <= t <= t1:
                continue
            c = sl["members"].get(k)
            if c is not None:
                truth.append(dict(t=c["t"], y=c["y"], slot=si, rotation=k, observed=True, track=c["track"]))
            else:
                truth.append(dict(t=t, y=sl["y"], slot=si, rotation=k, observed=False, track=None))
    truth.sort(key=lambda c: c["t"])
    return truth, period, slots, ref


# ----------------------------------------------------------------------------- matching
def match_events(events, crossings, before=0.5, after=3.0, ygate=14):
    """events: [(t_capture, x, y)]. An event matches a crossing with t_cross - before <= t_event
    <= t_cross + after and |y| within ygate, one to one, cheapest first."""
    pairs = []
    for ei, (te, _, ye) in enumerate(events):
        for ci, c in enumerate(crossings):
            dt = te - c["t"]
            if -before <= dt <= after and abs(ye - c["y"]) <= ygate:
                pairs.append((abs(ye - c["y"]) / ygate + max(dt, 0) / after, ei, ci))
    pairs.sort()
    ue, uc, m = set(), set(), {}
    for _, ei, ci in pairs:
        if ei in ue or ci in uc:
            continue
        ue.add(ei); uc.add(ci); m[ci] = ei
    return m


def scored(crossings, t0, t1, margin=3.0):
    """The crossings that a counter can be asked for: away from the ends of the trace (the
    counter is settled only after its first results)."""
    return [i for i, c in enumerate(crossings) if t0 + margin <= c["t"] <= t1 - margin]


def recall(events, crossings, idx):
    m = match_events(events, crossings)
    hit = sum(1 for i in idx if i in m)
    inside = set(idx)
    return dict(matched=hit, crossings=len(idx), recall=hit / len(idx) if idx else None,
                events=len(events), unmatched_events=len(events) - len(m),
                matched_outside_window=sum(1 for i in m if i not in inside))


# ----------------------------------------------------------------------------- policies
def run_counter(samples, det, cap):
    """samples: frame seqs that were inferred. Fed in capture order to the device counter."""
    c = passage.Counter(**COUNTER)
    ev = []
    for s in sorted(samples, key=lambda s: cap[s]):
        for _, p in c.update(det[s]):
            ev.append((cap[s], p[0], p[1]))
    return ev


def latest_workers(frames, k, service, rng, start=None):
    """K workers, each free worker takes the newest captured frame; if that frame is already
    taken it waits for the next capture. service(rng) gives the cycle time."""
    times = [t for _, t in frames]
    seqs = [s for s, _ in frames]
    free = [times[0] if start is None else start] * k
    taken, chosen = set(), []
    end = times[-1]
    while True:
        w = int(np.argmin(free))
        now = free[w]
        if now > end:
            break
        i = bisect.bisect_right(times, now) - 1
        if i < 0 or i in taken:
            j = max(i + 1, 0)
            while j < len(times) and j in taken:
                j += 1
            if j >= len(times):
                break
            free[w] = times[j]
            continue
        taken.add(i)
        chosen.append(seqs[i])
        free[w] = now + service(rng)
    return chosen


def periodic(frames, interval, start=None):
    times = [t for _, t in frames]
    seqs = [s for s, _ in frames]
    t = times[0] if start is None else start
    out = []
    while t <= times[-1]:
        i = bisect.bisect_right(times, t) - 1
        if i >= 0 and (not out or out[-1] != seqs[i]):
            out.append(seqs[i])
        t += interval
    return out


def mission_aware(frames, det, cap, service, rng, wait_max=0.6, lead=0.35, lookback=False):
    """K=1. After each result, the counter's tracks give the flow; a track still before the line
    whose predicted position would be just past it (0..flow/2) when a frame is taken counts it on
    that result. The worker may wait up to wait_max for a future frame whose predicted placement
    puts more tracks just past the line than taking the newest frame now, with the cost of the
    wait charged as lead per second (the idle time lowers the sampling rate). With lookback it
    may also take an older frame captured since its previous sample (no idle time; the result
    is older, which the capture-ordered counter does not mind)."""
    times = [t for _, t in frames]
    seqs = [s for s, _ in frames]
    c = passage.Counter(**COUNTER)
    now, last_t, chosen, ev = times[0], None, [], []
    flow_rate = None   # units per second
    while now <= times[-1]:
        i = bisect.bisect_right(times, now) - 1
        if i < 0 or (chosen and seqs[i] == chosen[-1]):
            j = i + 1
            if j >= len(times):
                break
            now = times[j]; continue
        best = (None, -1e9)
        if flow_rate and flow_rate > 0 and last_t is not None:
            cands = [i] + [j for j in range(i + 1, len(times)) if times[j] - now <= wait_max]
            if lookback:
                cands += [j for j in range(i - 1, -1, -1) if times[j] > last_t]
            for j in cands:
                tj = max(times[j], now)
                gain = 0.0
                for tr in c.tracks:
                    if tr is None or tr["counted"]:
                        continue
                    px = tr["x"] + flow_rate * (times[j] - last_t)
                    if tr["x"] < LINE <= px < LINE + max(c.flow, 8):
                        gain += 1.0
                score = gain - lead * (tj - now)
                if score > best[1]:
                    best = (j, score)
            j = best[0]
        else:
            j = i
        start = max(times[j], now)
        s = seqs[j]
        chosen.append(s)
        for _, p in c.update(det[s]):
            ev.append((cap[s], p[0], p[1]))
        if last_t is not None and c.has_flow:
            dt = cap[s] - last_t
            if dt > 0:
                flow_rate = c.flow / max(dt, 1e-3) if flow_rate is None else 0.8 * flow_rate + 0.2 * c.flow / dt
        last_t = cap[s]
        now = start + service(rng)
    return chosen, ev


# ----------------------------------------------------------------------------- main
def position_at(c, tracks_by_uid, t, flow):
    """x, y of the screw of truth crossing c at time t: interpolated along its truth track, else
    extrapolated from the crossing with the mean flow."""
    tr = tracks_by_uid.get(c["track"]) if c["track"] is not None else None
    if tr is not None and tr["obs"][0][0] <= t <= tr["obs"][-1][0]:
        obs = tr["obs"]
        i = max(1, bisect.bisect_left([o[0] for o in obs], t))
        (ta, xa, ya), (tb, xb, yb) = obs[i - 1], obs[min(i, len(obs) - 1)]
        f = 0 if tb == ta else (t - ta) / (tb - ta)
        return xa + f * (xb - xa), ya + f * (yb - ya), True
    return LINE + flow * (t - c["t"]), c["y"], False


def classify_misses(truth, idx, device_events, samples, pts, cap, tracks_by_uid, flow, near=24, dgate=10):
    m = match_events(device_events, truth)
    out = []
    for i in idx:
        if i in m:
            continue
        c = truth[i]
        near_s = []
        for s in samples:
            if s not in cap:
                continue
            x, y, on_track = position_at(c, tracks_by_uid, cap[s], flow)
            if LINE - near <= x < LINE + near:
                seen = any(abs(px - x) <= dgate and abs(py - y) <= dgate for px, py in pts.get(s, []))
                near_s.append(dict(seq=s, dt=round(cap[s] - c["t"], 2), x=round(x), side=1 if x >= LINE else -1, detected=seen))
        if not near_s:
            kind = "sampling_gap"
        elif not any(n["detected"] for n in near_s):
            kind = "sampled_not_detected"
        elif not (any(n["detected"] and n["side"] > 0 for n in near_s)):
            kind = "detected_only_before_line"
        else:
            kind = "detected_not_counted"
        out.append(dict(t=round(c["t"], 2), y=round(c["y"]), slot=c["slot"], truth_observed=c["observed"],
                        kind=kind, samples=near_s))
    return out


def stats(xs):
    xs = np.asarray(xs, float)
    return dict(mean=float(xs.mean()), sd=float(xs.std()), min=float(xs.min()), max=float(xs.max()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=Path)
    ap.add_argument("--model", type=Path, default=MODEL)
    ap.add_argument("--seeds", type=int, default=20)
    ap.add_argument("--screws", type=int, default=19)
    a = ap.parse_args()
    run = a.run
    frames = load_trace(run)
    cap = {s: t for s, t in frames}
    t0, t1 = frames[0][1], frames[-1][1]

    # device side
    dcap = {e["seq"]: e["done_us"] / 1e6 for e in log_events(run, "MOQ_CAPTURE")}
    starts = log_events(run, "AI_START")
    prof = log_events(run, "AI_PROFILE")
    track = {e["seq"]: [tuple(p) for p in e["pts"]] for e in log_events(run, "MOQ_TRACK")}
    passages = log_events(run, "MOQ_PASSAGE")
    for e in starts:
        dcap.setdefault(e["seq"], e["capture_ms"] / 1e3)
    device_samples = [e["seq"] for e in starts]
    durations = np.array([e["duration_us"] / 1e6 for e in prof])
    st = {e["seq"]: e["start_us"] / 1e6 for e in starts}
    dur = {e["seq"]: e["duration_us"] / 1e6 for e in prof}
    ordered = sorted(st, key=st.get)
    overhead = np.array([st[b] - st[a] - dur[a] for a, b in zip(ordered, ordered[1:]) if a in dur])
    cycle = np.array([st[b] - st[a] for a, b in zip(ordered, ordered[1:])])
    staleness = np.array([st[s] - dcap[s] for s in ordered if s in dcap])
    capture_fps = (len(dcap) - 1) / (max(dcap.values()) - min(dcap.values()))

    # 1. validation
    det_model = Detector(a.model)
    det = {s: det_model(run / "trace" / "frames" / f"{s:07d}.jpg") for s, _ in frames}
    pcount = {e["seq"]: e["detections"] for e in prof}
    both = [s for s in pcount if s in det]
    diff = [len(det[s]) - pcount[s] for s in both]
    exact_pts = [s for s in both if s in track and sorted(det[s]) == sorted(track[s])]
    near_pts = 0
    for s in both:
        if s in track and len(det[s]) == len(track[s]):
            rest = list(track[s]); ok = True
            for x, y in det[s]:
                j = next((k for k, (u, v) in enumerate(rest) if abs(u - x) <= 7 and abs(v - y) <= 7), None)
                if j is None:
                    ok = False; break
                rest.pop(j)
            near_pts += ok
    # the counter port: the device's own detections through passage.Counter
    c = passage.Counter(**COUNTER); port = []
    for s in sorted(track, key=lambda s: dcap.get(s, 0)):
        for _, p in c.update(track[s]):
            port.append((s, p[0], p[1]))
    dev = [(e["seq"], e["x"], e["y"]) for e in passages]
    validation = dict(device_inferred=len(pcount), in_trace=len(both),
                      count_exact=sum(d == 0 for d in diff), count_exact_rate=float(np.mean(np.array(diff) == 0)),
                      count_mean_abs_diff=float(np.mean(np.abs(diff))), points_exact=len(exact_pts),
                      points_within_one_cell=near_pts,
                      counter_port_reproduces_device=port == dev, counter_port_events=len(port), device_events=len(dev))

    # 2. ground truth
    crossings, tracks, flow = track_truth(frames, det)
    tracks_by_uid = {tr["id"]: tr for tr in tracks}
    p0 = period_estimate([c["t"] for c in crossings])
    truth, period, slots, ref = periodic_truth(crossings, t0, t1, p0)
    rot = [int(np.floor((c["t"] - ref) / period)) for c in crossings]
    per_rot_tracked = {k: rot.count(k) for k in sorted(set(rot))}
    per_rot_truth = {}
    for c in truth:
        per_rot_truth[c["rotation"]] = per_rot_truth.get(c["rotation"], 0) + 1
    # score the crossings that lie in rotations wholly inside the trace, after a settling margin
    full = [k for k in per_rot_truth
            if ref + k * period + min(sl["phi"] for sl in slots) - 3 >= t0 and ref + k * period + max(sl["phi"] for sl in slots) + 3 <= t1]
    idx = [i for i, c in enumerate(truth) if c["rotation"] in full]
    burst = (min(sl["phi"] for sl in slots), max(sl["phi"] for sl in slots))
    gt = dict(trace_frames=len(frames), trace_fps=(len(frames) - 1) / (t1 - t0), device_capture_fps=capture_fps,
              trace_span_s=t1 - t0, tracked_crossings=len(crossings), coarse_period_s=p0, period_s=period,
              flow_units_per_s=flow, slots=len(slots), screws_physical=a.screws,
              tracked_per_rotation=per_rot_tracked, truth_per_rotation=per_rot_truth,
              truth_crossings=len(truth), truth_imputed=sum(not c["observed"] for c in truth),
              scored_rotations=full, scored_crossings=len(idx), burst_duration_s=burst[1] - burst[0],
              slot_table=[dict(phase=round(sl["phi"], 2), y=round(sl["y"]), seen=len(sl["members"])) for sl in slots])

    # 3. device recall
    dev_events = [(dcap[e["seq"]], e["x"], e["y"]) for e in passages if e["seq"] in dcap]
    in_window = lambda ev: [e for e in ev if ref + min(full) * period + burst[0] - 3 <= e[0] <= ref + max(full) * period + burst[1] + 3]
    device = recall(in_window(dev_events), truth, idx)
    device["events_total"] = len(dev_events)
    device["recall_vs_physical"] = device["matched"] / (a.screws * len(full))
    device["events_per_rotation"] = {k: sum(1 for e in dev_events if 0 <= e[0] - ref - k * period - burst[0] + 3 <= burst[1] - burst[0] + 6)
                                     for k in sorted(per_rot_truth)}
    misses = classify_misses(truth, idx, in_window(dev_events), device_samples, track, dcap, tracks_by_uid, flow)
    kinds = {}
    for mm in misses:
        kinds[mm["kind"]] = kinds.get(mm["kind"], 0) + 1
    device.update(miss_kinds=kinds, misses=misses, inference_cycle_s=stats(cycle), duration_s=stats(durations),
                  overhead_s=float(np.median(overhead)), staleness_s=stats(staleness),
                  inferred_fraction=len(device_samples) / len(dcap))

    # 4. replay
    over = float(np.median(overhead))
    service_draw = lambda rng: float(rng.choice(durations)) + over
    fixed = float(np.median(cycle))
    def score(samples):
        ev = run_counter(samples, det, cap)
        r = recall(in_window(ev), truth, idx)
        return r["recall"], r["unmatched_events"], len(samples)
    policies = {}
    for k in (1, 2, 3, 4):
        rs = []
        for seed in range(a.seeds):
            rng = np.random.default_rng(seed)
            start = t0 + rng.uniform(0, fixed)
            rs.append(score(latest_workers(frames, k, service_draw, rng, start=start)))
        policies[f"latest_K{k}"] = dict(recall=stats([r[0] for r in rs]), false_events=stats([r[1] for r in rs]),
                                        inferences=stats([r[2] for r in rs]))
    for k in (1, 2, 3):
        r = score(latest_workers(frames, k, lambda rng: fixed, None))
        policies[f"latest_K{k}_fixed{fixed:.2f}s"] = dict(recall=r[0], false_events=r[1], inferences=r[2])
    for interval in (fixed, 1.5, 2.0, fixed / 2, fixed / 3):
        rs = [score(periodic(frames, interval, start=t0 + off)) for off in np.linspace(0, interval, 10, endpoint=False)]
        policies[f"periodic_{interval:.2f}s"] = dict(recall=stats([r[0] for r in rs]), false_events=stats([r[1] for r in rs]),
                                                     inferences=stats([r[2] for r in rs]),
                                                     note="K=1 can sustain only interval >= cycle" if interval < fixed else "")
    rs = [score([s for s, _ in frames])]
    policies["all_trace_frames"] = dict(recall=rs[0][0], false_events=rs[0][1], inferences=rs[0][2])
    for name, kw in (("mission_aware_K1_wait", dict(wait_max=0.6, lead=0.35)),
                     ("mission_aware_K1_lookback", dict(wait_max=0.0, lookback=True)),
                     ("mission_aware_K1_wait_lookback", dict(wait_max=0.4, lead=0.35, lookback=True))):
        rs = []
        for seed in range(a.seeds):
            rng = np.random.default_rng(seed)
            chosen, ev = mission_aware(frames, det, cap, service_draw, rng, **kw)
            r = recall(in_window(ev), truth, idx)
            rs.append((r["recall"], r["unmatched_events"], len(chosen)))
        policies[name] = dict(recall=stats([r[0] for r in rs]), false_events=stats([r[1] for r in rs]),
                              inferences=stats([r[2] for r in rs]), params=kw)
    # the device's own samples, host detections where the frame reached the host
    have = [s for s in device_samples if s in det]
    r = score(have)
    policies["device_samples_in_trace"] = dict(recall=r[0], false_events=r[1], inferences=r[2],
                                               note=f"{len(have)}/{len(device_samples)} device-inferred frames are in the trace")

    summary = dict(run=str(run), model=str(a.model), counter=COUNTER, validation=validation, ground_truth=gt,
                   device=device, replay=dict(service="AI_PROFILE duration (resampled) + median start overhead",
                                              overhead_s=over, fixed_cycle_s=fixed, seeds=a.seeds, policies=policies))
    (run / "replay_summary.json").write_text(json.dumps(summary, indent=1, default=float) + "\n")
    brief = dict(validation=validation, gt={k: v for k, v in gt.items() if k != "slot_table"},
                 device={k: v for k, v in device.items() if k != "misses"},
                 policies={k: (v["recall"] if not isinstance(v["recall"], dict) else
                               (round(v["recall"]["mean"], 3), round(v["recall"]["sd"], 3)),
                               v["inferences"] if not isinstance(v["inferences"], dict) else v["inferences"]["mean"])
                           for k, v in policies.items()})
    print(json.dumps(brief, indent=1, default=float))


if __name__ == "__main__":
    main()

"""Passage counter with global association (AIOT_MOQT_PASSAGE_ASSOC=1), the Python reference of
the C code in src/main/moq_passage.c. Integer arithmetic only, as the C code, so both give the same
events. Same configuration and events as screw_mission/passage.py (the ASSOC=0 counter).

Differences from ASSOC=0, each for a failure seen in trace-s2-r1:
- Time, not result count: every update carries the capture time (ms). Velocities are in units per
  second, so a prediction is right whatever the spacing of the results (several workers, stale
  frames, dropped results).
- 2-D prediction: each track has its own (vx, vy). On a turntable the screws keep moving across
  the line (vy ~ 7 units/s at the line, vx 12..20 depending on the radius); ASSOC=0 predicted x
  only, from one shared flow, and took the raw y difference as cost.
- Global assignment: the pairing of tracks and detections of minimum total residual (Hungarian,
  O(n^3), n <= 24) among the pairs inside the gate, instead of the greedy cheapest-pair-first that
  swapped tracks within the adjacent group of screws.
- Coasting: an unmatched track is kept while its predicted travel since it was last seen stays
  within max_step (the gate of one result), not only max_missed results, so that at a high result
  rate a screw that flickers in the detector is not dropped and re-born past the line.
- The events of one update are given in the order in which the objects crossed (estimated from the
  travel past the line and the speed), not in the order of the tracks.
- A track born past the line is counted (count_unseen) when it lies less than the travel of the
  flow since the previous result past the line.
The gate sizes are those of ASSOC=0: residual <= max_step/2 + max_step/4 per missed result, and no
more than max_step/4 backwards."""

TRACKS = 24
EVENTS = 8    # MOQ_PASSAGE_EVENTS: events described per update
POINTS = 24   # detections per update (AIOT_AI_RESULT_MAX_DETECTIONS)
BIG = 1 << 20


def cdiv(a, b):
    """C integer division (truncation toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def hungarian(cost, n, m):
    """Minimum-cost assignment of n rows to m columns, n <= m (e-maxx, potentials). Returns for
    each row its column. Integer costs; the C code is the same loop."""
    INF = 1 << 30
    u = [0] * (n + 1); v = [0] * (m + 1); p = [0] * (m + 1); way = [0] * (m + 1)
    for i in range(1, n + 1):
        p[0] = i; j0 = 0
        minv = [INF] * (m + 1); used = [False] * (m + 1)
        while True:
            used[j0] = True
            i0 = p[j0]; delta = INF; j1 = 0
            for j in range(1, m + 1):
                if not used[j]:
                    cur = cost[i0 - 1][j - 1] - u[i0] - v[j]
                    if cur < minv[j]:
                        minv[j] = cur; way[j] = j0
                    if minv[j] < delta:
                        delta = minv[j]; j1 = j
            for j in range(m + 1):
                if used[j]:
                    u[p[j]] += delta; v[j] -= delta
                else:
                    minv[j] -= delta
            j0 = j1
            if p[j0] == 0:
                break
        while True:
            j1 = way[j0]; p[j0] = p[j1]; j0 = j1
            if j0 == 0:
                break
    row = [-1] * n
    for j in range(1, m + 1):
        if p[j]:
            row[p[j] - 1] = j - 1
    return row


class Counter:
    def __init__(self, line=128, vertical_axis=False, direction=1, max_step=48, max_missed=2, predict=True,
                 count_unseen=True, settled=False, confirm=False):
        self.line, self.vertical, self.direction = line, vertical_axis, direction
        self.max_step, self.max_missed, self.predict, self.count_unseen = max_step, max_missed, predict, count_unseen
        self.settled = settled
        self.confirm = confirm
        self.tracks = [None] * TRACKS
        self.fa = self.fl = 0          # flow along / across the axis, units per second x16
        self.has_flow = False
        self.last_ms, self.has_last = 0, False
        self.total = 0
        self.dropped = 0

    def ax(self, x, y):
        return y if self.vertical else x

    def lat(self, x, y):
        return x if self.vertical else y

    def side(self, x, y):
        return 1 if self.ax(x, y) >= self.line else -1

    def cost(self, t, p, now):
        """Residual of detection p from where track t is predicted at `now`, or -1 outside the gate."""
        dt = now - t["ms"]
        ta, tl = self.ax(t["x"], t["y"]), self.lat(t["x"], t["y"])
        pa, pl = self.ax(*p), self.lat(*p)
        tol = self.max_step // 4
        if not self.predict or not self.direction:
            d = max(abs(pa - ta), abs(pl - tl))
            return d if d <= self.max_step else -1
        forward = (pa - ta) * self.direction
        steps = min(t["missed"], self.max_missed) + 1
        if forward < -tol or forward > self.max_step * steps:
            return -1
        if t["moving"]:
            va, vl = t["va"], t["vl"]
        elif self.has_flow:
            va, vl = self.fa, self.fl
        else:   # nothing known about the motion: the gate of one step
            dl = abs(pl - tl)
            return dl if dl <= self.max_step // 2 else -1
        ea = abs(pa - (ta + cdiv(va * dt, 16000) * self.direction))
        el = abs(pl - (tl + cdiv(vl * dt, 16000)))
        gate = self.max_step // 2 + tol * (steps - 1)
        if ea > gate or el > gate:
            return -1
        return ea + el

    def count(self, t, p, events):
        self.total += 1
        t["counted"] = True
        events.append((self.total, p))
        # how long ago it crossed: travel past the line over its speed along the axis (ms)
        past = abs(self.ax(*p) - self.line)
        v = abs(t["va"] if t["moving"] else self.fa)
        self._keys.append(cdiv(past * 16000, v) if v > 0 else 0)

    def ordered(self, events):
        """The events of one update in the order of crossing, earliest first (the largest time since
        the crossing first; stable), with their IDs in that order. As the C code, only the first
        EVENTS events (its buffer) are reordered."""
        n = min(len(events), EVENTS)
        if n < 2:
            return events
        order = sorted(range(n), key=lambda i: -self._keys[i])   # stable
        base = events[0][0]
        return [(base + k, events[i][1]) for k, i in enumerate(order)] + events[n:]

    def update(self, points, now_ms):
        """points: [(x, y)] 0..255, now_ms: capture time of the frame (ms, increasing).
        Returns the passage events [(id, (x, y))]."""
        points = [(int(x), int(y)) for x, y in points][:POINTS]
        now = int(now_ms)
        events = []
        self._keys = []
        rows = [i for i, t in enumerate(self.tracks) if t is not None]
        n, m = len(rows), len(points)
        match = {}   # track index -> point index
        if n and m:
            c = [[0] * m for _ in range(n)]
            for r, ti in enumerate(rows):
                for pi, p in enumerate(points):
                    d = self.cost(self.tracks[ti], p, now)
                    c[r][pi] = d if d >= 0 else BIG
            if n <= m:
                a = hungarian(c, n, m)
                for r in range(n):
                    if a[r] >= 0 and c[r][a[r]] < BIG:
                        match[rows[r]] = a[r]
            else:
                ct = [[c[r][pi] for r in range(n)] for pi in range(m)]
                a = hungarian(ct, m, n)
                for pi in range(m):
                    if a[pi] >= 0 and ct[pi][a[pi]] < BIG:
                        match[rows[a[pi]]] = pi
        taken = set(match.values())
        for ti in sorted(match):
            t, p = self.tracks[ti], points[match[ti]]
            before, after = self.side(t["x"], t["y"]), self.side(*p)
            if before != after:
                if not self.direction or self.direction == after:
                    if not t["counted"]:
                        self.count(t, p, events)
                else:
                    t["counted"] = False
            if t.get("pending") and not t["counted"]:
                self.count(t, p, events)   # confirm: born past the line, seen again
            t["pending"] = False
            dt = now - t["ms"]
            if self.predict and self.direction and dt > 0:
                va = cdiv((self.ax(*p) - self.ax(t["x"], t["y"])) * self.direction * 16000, dt)
                vl = cdiv((self.lat(*p) - self.lat(t["x"], t["y"])) * 16000, dt)
                if t["moving"]:
                    t["va"], t["vl"] = cdiv(t["va"] + va, 2), cdiv(t["vl"] + vl, 2)
                else:
                    t["va"], t["vl"] = va, vl
                t["moving"] = True
                if self.has_flow:
                    self.fa, self.fl = cdiv(3 * self.fa + va, 4), cdiv(3 * self.fl + vl, 4)
                else:
                    self.fa, self.fl = va, vl
                self.has_flow = True
            t["x"], t["y"], t["missed"], t["ms"] = p[0], p[1], 0, now
        for ti, t in enumerate(self.tracks):
            if t is None or ti in match:
                continue
            t["missed"] += 1
            if t["missed"] > self.max_missed:
                va = t["va"] if t["moving"] else self.fa
                travel = abs(cdiv(va * (now - t["ms"]), 16000))
                if not (t["moving"] or self.has_flow) or travel > self.max_step:
                    self.tracks[ti] = None
        for pi, p in enumerate(points):
            if pi in taken:
                continue
            free = next((i for i, t in enumerate(self.tracks) if t is None), None)
            if free is None:
                self.dropped += 1
                continue
            t = self.tracks[free] = dict(x=p[0], y=p[1], missed=0, va=0, vl=0, moving=False, counted=False, ms=now)
            if self.count_unseen and self.direction and (self.has_flow or not self.settled):
                past = (self.ax(*p) - self.line) * self.direction
                if self.has_flow and self.has_last:
                    step = cdiv(self.fa * (now - self.last_ms), 16000)
                else:
                    step = self.max_step
                if 0 <= past < step:
                    if self.confirm:
                        t["pending"] = True
                    else:
                        self.count(t, p, events)
        self.last_ms, self.has_last = now, True
        return self.ordered(events)

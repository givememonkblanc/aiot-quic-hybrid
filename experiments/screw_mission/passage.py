"""The passage counter of the device (src/main/moq_passage.c), in Python.

Coordinates are in the units of the device result, 0..255. With `settled`, an object seen
for the first time just past the line is counted only once the flow of the objects is
known, as the device does with AIOT_MOQT_PASSAGE_SETTLED: at the start every object is new,
and those just past the line had not crossed it."""

TRACKS = 24


class Counter:
    def __init__(self, line=128, vertical_axis=False, direction=1, max_step=48, max_missed=2, predict=True,
                 count_unseen=True, settled=False):
        self.line, self.vertical, self.direction = line, vertical_axis, direction
        self.max_step, self.max_missed, self.predict, self.count_unseen = max_step, max_missed, predict, count_unseen
        self.settled = settled
        self.tracks = [None] * TRACKS
        self.flow, self.has_flow, self.total = 0, False, 0

    def axial(self, x, y):
        return y if self.vertical else x

    def lateral(self, x, y):
        return x if self.vertical else y

    def side(self, x, y):
        return 1 if self.axial(x, y) >= self.line else -1

    def cost(self, t, p):
        ta, tl = self.axial(t["x"], t["y"]), self.lateral(t["x"], t["y"])
        pa, pl = self.axial(*p), self.lateral(*p)
        steps, da, dl = t["missed"] + 1, pa - ta, abs(pl - tl)
        if not self.predict or not self.direction:
            d = max(abs(da), dl)
            return d if d <= self.max_step else -1
        forward, tolerance = da * self.direction, self.max_step // 4
        if forward < -tolerance or forward > self.max_step * steps or dl > self.max_step // 2:
            return -1
        known = t["moving"] or self.has_flow
        v = t["v"] if t["moving"] else self.flow
        off = abs(forward - v * steps) if known else 0
        if known and off > self.max_step // 2 + tolerance * (steps - 1):
            return -1
        return off + dl

    def count(self, t, p, events):
        self.total += 1
        t["counted"] = True
        events.append((self.total, p))

    def update(self, points):
        """points: [(x, y)] in 0..255. Returns the passage events [(id, (x, y))]."""
        points = [(int(x), int(y)) for x, y in points][:64]
        events, track_taken, point_taken = [], set(), set()
        while True:
            best = None
            for ti, t in enumerate(self.tracks):
                if t is None or ti in track_taken:
                    continue
                for pi, p in enumerate(points):
                    if pi in point_taken:
                        continue
                    d = self.cost(t, p)
                    if d >= 0 and (best is None or d < best[0]):
                        best = (d, ti, pi)
            if best is None:
                break
            _, ti, pi = best
            t, p = self.tracks[ti], points[pi]
            track_taken.add(ti)
            point_taken.add(pi)
            before, after = self.side(t["x"], t["y"]), self.side(*p)
            if before != after:
                if not self.direction or self.direction == after:
                    if not t["counted"]:
                        self.count(t, p, events)
                else:
                    t["counted"] = False
            if self.predict and self.direction:
                steps = t["missed"] + 1
                v = int((self.axial(*p) - self.axial(t["x"], t["y"])) * self.direction / steps)
                t["v"] = int((t["v"] + v) / 2) if t["moving"] else v
                t["moving"] = True
                self.flow = int((3 * self.flow + v) / 4) if self.has_flow else v
                self.has_flow = True
            t["x"], t["y"], t["missed"] = p[0], p[1], 0
        for ti, t in enumerate(self.tracks):
            if t is None or ti in track_taken:
                continue
            t["missed"] += 1
            if t["missed"] > self.max_missed:
                self.tracks[ti] = None
        for pi, p in enumerate(points):
            if pi in point_taken:
                continue
            free = next((i for i, t in enumerate(self.tracks) if t is None), None)
            if free is None:
                continue
            t = self.tracks[free] = dict(x=p[0], y=p[1], missed=0, v=0, moving=False, counted=False)
            if self.count_unseen and self.direction and (self.has_flow or not self.settled):
                past = (self.axial(*p) - self.line) * self.direction
                step = self.flow if self.has_flow else self.max_step
                if 0 <= past < step:
                    self.count(t, p, events)
        return events

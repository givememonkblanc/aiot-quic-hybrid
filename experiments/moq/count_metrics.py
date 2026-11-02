#!/usr/bin/env python3
"""Count of the device against the count known to the receiver, over time."""


def incorrect_time(truth, known, begin, end):
    """Seconds in [begin, end] during which the two counts differ.

    `truth`: (time, count of the device) at every produced result. `known`: (time, count
    known to the receiver) at every arrival. Both counts are step functions."""
    points = sorted({begin, end, *[t for t, _ in truth if begin < t < end], *[t for t, _ in known if begin < t < end]})

    def at(series, t):
        value = 0
        for u, v in series:
            if u <= t:
                value = v
            else:
                break
        return value

    truth, known = sorted(truth), sorted(known)
    wrong = 0.0
    for a, b in zip(points, points[1:]):
        if at(truth, a) != at(known, a):
            wrong += b - a
    return wrong


def recovery_times(truth, known, outage_ends):
    """For every outage end, the time until an arrival makes the two counts equal."""
    out = []
    for end in outage_ends:
        for t, c in sorted(known):
            if t >= end and c == max([v for u, v in truth if u <= t], default=0):
                out.append(t - end)
                break
        else:
            out.append(None)
    return out

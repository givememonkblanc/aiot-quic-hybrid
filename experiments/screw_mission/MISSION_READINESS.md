# Mission validation readiness — 2026-09-26

Transport cohort remote-v33 is running separately from physical mission testing.
It replays a fixed image; no current camera observations or pass GT are collected.

## Existing evidence

The static 19-screw cap24 experiment yielded 3–5 blobs/result; even treating all
five as correct, its best-frame count implies at least 14 missed screws.
This is an optimistic static count bound, not a spatially matched recall score
or physical passage miss count. See ESP32_19_RESULTS.md.

## Required before mission success can be reported

- A new camera session after relocation, verifying the turntable is visible.
- Independent held-out GT pass events with object/pass identity, visibility
  interval, timebase mapping and a fixed deadline. The number 19 is an object
  count and does not alone define the pass count across rotations.
- Correctness matching of predictions to GT, followed by evidence-image linkage
  and receive/deadline evaluation. Preserve undetected/late events in denominator.
- Installation-domain train/validation data and frozen threshold, separated from
  test sessions; no training on the final GT evaluation frames.

The existing evaluate_events.py only uses temporal windows and a detected flag;
it does not establish spatial correctness or disambiguate overlapping objects.
It also chooses the earliest detection even if its evidence is invalid and a
later valid detection meets the deadline. Do not present that helper's output
as the final mission result until matching and evidence semantics are corrected
and tested using event identity and ground truth.

No new physical miss-count result is claimed from remote-v33.

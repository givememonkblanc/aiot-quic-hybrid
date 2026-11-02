# Upstream source and modifications

SWMAth/IFA arithmetic is derived from the Apache-2.0 licensed
[public-moq-demo](https://github.com/streaming-university/public-moq-demo), commit
`2071137a5967e645ab97b6e914dc4f30c8c47d95`, `player/src/player.ts`.
The upstream LICENSE is retained at `vendor/public-moq-demo/LICENSE`.
The upstream license appendix retains its copyright placeholder; the estimator
source does not carry an individual copyright notice.
See upstream repository for attribution; no authorship of original algorithms
is claimed here. Our modifications implement bounded C state, microsecond
durations and bytes/s units; the golden harness executes extracted upstream
method bodies with TypeScript annotations removed, without changing the math.

`player/src/estimator.ts` is a different offline estimator: it adds a factor-five
filter around the previous estimate. We deliberately reproduce the live player
path, not a mixture of these two implementations. Configured live defaults are
window=25, interval=5, trim=5%; fallback getter defaults differ.

An ACK-held-time measurement on ESP32 is an **application adaptation**, not the
original browser moof/mdat download interval. Neither JPEG completion samples
nor JPEGs called 'I-frames' reproduce the original IFA video workload.

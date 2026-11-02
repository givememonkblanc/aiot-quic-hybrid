# MoQ bounded packet work and JPEG slot ordering

This follows the [dual-track baseline](CONCURRENT.md). All device runs use real
1024×1024 JPEG, ESPDet cat224 on CPU1 at priority 0, capture on CPU0 at priority 2,
60 s acquisition and a 10 s drain/observation interval. No Wi-Fi shaping is
applied. Images are live camera input, **not an identical replay trace**; this
is an exploratory comparison, not a controlled protocol-effect estimate.

## Changes

`AIOT_MOQT_BOUNDED_PUMP=ON` is an opt-in experiment:

- Each receive service visits at most 16 packets; each send service visits at
  most 8 write iterations. Both check elapsed wall time against 20 ms between
  operations. QUIC timestamps refresh for each write iteration. A single crypto,
  socket or preempted operation can exceed this budget: it is not a hard 20 ms
  bound on `quic_client_process()`.
- A preview is copied into staging, hashed and submitted only when a JPEG TX
  slot is free. While both slots are busy the capture cache retains the latest
  image. Overwrites are counted as omitted images, never as delivered service.
- Control and AI retain application selection priority. Among pending JPEG
  objects, the oldest admission is served first instead of always slot 4 before
  slot 5. Stream buffers remain immutable until stream-close acknowledgement.
- Per-call pump timing and per-object slot residency distinguish final-byte
  submission from subsequent FIN acknowledgement. These are MCU-local elapsed
  times, not synchronized camera-to-server latency.

No object cancellation/expiry, new adaptive algorithm, or priority changes are
introduced. Cancelling all images at an arbitrary short deadline could simply
produce zero complete images; first fix the demonstrated selection bug.

## Defect isolated by the intermediate run

`device-pump-v3` added bounded packet work and slot-aware staging but retained
slot-index ordering. Completed images increased from 15 to 42. However, its
worst JPEG occupied a slot for **25.131 s**: **25.023 s before final-byte
submission**, followed by only **107 ms waiting for close/ACK**. A newly
admitted object in recycled slot 4 repeatedly overtook the older object in
slot 5. Thus ACK waiting does not explain this particular long residency.

`test_tx_policy.c` reproduces the slot-reuse sequence and verifies that the
older waiting JPEG is chosen, while control/AI remain ahead of video and
FIN-submitted streams are excluded by the eligibility mask. The fixed policy
is in the shared header used by the real adapter, not a duplicate test model.

The intermediate run also increased AI inference median from 2.229 to 2.500 s.
More completed video alone is not sufficient to declare overall QoS better.

## Reproduction

```
AIOT_MOQT_CONCURRENT=ON AIOT_MOQT_BOUNDED_PUMP=ON bash experiments/moq/build.sh
experiments/moq/vendor/venv/bin/python experiments/moq/device_trial.py --dual --name UNIQUE_RUN
python experiments/moq/analyze_pump.py experiments/moq/results/UNIQUE_RUN
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/components/transport experiments/moq/test_tx_policy.c -o /tmp/moq-policy-test
/tmp/moq-policy-test
```

The harness retains firmware, hashes, received images, raw events and restore
logs in ignored local results. It restores the full original flash in `finally`.
After subscriber completion it observes another second for normal connection
closure; this does not shorten the 60 s workload or 10 s drain interval.

## Completed physical comparison

| Measurement | Previous v2 | Bounded pump, index order v3 | Bounded pump, oldest JPEG v4 |
|---|---:|---:|---:|
| Captured frames | 598 | 601 | 600 |
| Mean collection Hz | 9.950 | 9.999 | 9.974 |
| Collection gap p95 / max, ms | 154.6 / 315.4 | 141.9 / 219.2 | 152.4 / 259.7 |
| Complete received JPEGs | 15 | 42 | 39 |
| Received / collected | 2.51% | 6.99% | 6.50% |
| Received JPEG mean size, B | 135,121 | 129,583 | 133,020 |
| Actual AI executions / received results | 27 / 27 | 24 / 24 | 24 / 24 |
| AI inference p50 / p95, s | 2.229 / 2.325 | 2.500 / 2.565 | 2.498 / 2.550 |
| JPEG slot residency p50 / max, s | not instrumented | 1.478 / 25.131 | 3.102 / 3.243 |
| JPEG post-submission ACK wait p50 / max, ms | not instrumented | 36.0 / 107.2 | 31.3 / 140.2 |

All admitted images were fully decoded and SHA256 matched. Both trials passed
actual-model-execution checks and dual-track normal closure. No decoder allocation
failure, watchdog or panic was observed. The corrected v4 sampled minimum free
internal memory was 32,303 B and PSRAM was 1,940,648 B (not guaranteed worst-case
minima). Tests restore the original full flash after completion.

**Trade-off:** the final run delivered 2.6 times as many complete images as the
previous v2 run, but AI median inference took about 12.1% longer and produced
24 instead of 27 results in the finite test. Oldest-JPEG selection removed the
long starvation tail, while increasing typical slot residency relative to the
unfair index-order trial. This is a tail-fairness correction, not improvement
of every latency percentile. Live image sizes also differed, and neither one
run nor this bundled change isolates the contribution of each optimization.

V4 capture accounting: 600 acquisitions = 11 preview-cache lock misses + 549
cache replacements + 39 submissions + 1 final cached image. No submission was
refused, but **561 acquired images still were not delivered**. Moving omission
to the latest cache does not turn those images into mission successes. All AI
results in these runs are valid zero-detection results from the cat224 model;
GT event accuracy/deadline success remains unmeasured.

V4's instrumented QUIC process-call mean was 22.64 ms and maximum 342.16 ms,
including startup. This confirms that per-loop 20 ms checks are not a hard
end-to-end execution bound; an individual operation and task preemption remain.
The maximum acquisition gap is still 259.7 ms. The strict 100 ms collection-gap
requirement is **not met**, even with an average near 10 Hz.

## Decision and next comparison

Keep the packet-work and slot-order fixes as an opt-in research configuration.
Do not replace the operational FOMO firmware or call this a universal QoS win.
Before adding an adaptive scheduler, compare one JPEG in flight against the
current two, preserving the AI slot. This tests whether simple latest-only
admission can reduce typical image age without sacrificing complete goodput.
Then choose an explicit application deadline and implement cancellation with
correct reset/stream-count accounting. Weak-link tests must include unreceived
and uninferred GT events in mission-success denominators.

Local artifacts: `results/device-pump-v3/` (intermediate selection bug),
`results/device-pump-v4/` (fixed order), firmware images/hashes, `pump-summary.json`,
raw events, received JPEGs, flash/restore logs. Regression test: `test_tx_policy.c`.

Final restoration verification: five baseline setting acknowledgements and
seven FOMO AI samples on CPU0; no NVS save. Temporary receiver14568 is stopped.
Firmware build, codec ASan/UBSan test, slot-order ASan/UBSan regression, and seven
Python receiver tests passed. Machine-readable outcomes explicitly distinguish
functional validation from the failed 100 ms capture-gap SLO and unmeasured
mission QoS.

The follow-up [one-versus-two-slot device comparison](SLOT_COMPARISON.md) is
complete. It reduces slot residence with one slot but also receives fewer
images; AI inference remains near2.5s. One slot is the next freshness baseline,
not a universal optimum.

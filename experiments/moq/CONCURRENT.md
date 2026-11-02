# Independent capture and AI-result track (draft-21 profile)

This opt-in stage uses the ESPDet Pico cat224 model on CPU1, not the restored
FOMO baseline. It measures camera collection, real local inference, and server
receipt separately. It is not a screw detector or a mission-accuracy experiment.

## Fixed configuration

- Camera: 1024×1024 sensor-produced JPEG. A CPU0 priority-2 task requests frames
  on a 100 ms release grid. Whole missed releases are skipped; completion gaps
  are measured. No guarantee of sensor exposure spacing or hard 10 Hz deadlines.
- AI: existing one-active/one-latest-pending worker, CPU1 priority 0, offers at
  most 5 Hz. Cat224 runs on decoded RGB; the 3 MiB output allocation is retained
  throughout this experiment. No model quantization/accuracy change.
- Preview: one latest cache and one network staging buffer, each capped at
  256 KiB. Producer uses a nonblocking mutex. Cache overwrite and lock misses
  are recorded. Network admission has two immutable JPEG object slots.
- AI results: `aiot/esp32/ai/events`, alias 2, JSON carrying source sequence,
  application capture timestamp, observation timestamp, backend/count, and the
  complete existing v2 binary detection record as hex. Alias 1 is `preview/jpeg`.
  One reserved AI object slot; application write selection visits AI before
  JPEG. Shared QUIC congestion control and Wi-Fi still couple the tracks.
- One group per object, object 0; independent SUBSCRIBE, SUBSCRIBE_OK and
  PUBLISH_DONE/FIN for both tracks. Exact leaf-certificate pin and moqt-21 ALPN.
- Main task owns QUIC calls. AI observations and video submissions can still
  be delayed by a long `quic_client_process()` call. Track separation alone
  does not remove this source of service latency.
- 60 s capture, then 10 s observation/drain cutoff. Pending/active inference
  exceeding that cutoff would be an unobserved result, not a success.

## Loss/accounting rules

Every real camera acquisition logs completion time, acquire duration, bytes,
cache contention, and cumulative overwrite count. `capture_us` is an application
acquisition/copy timestamp, **not sensor exposure onset**. Capture gaps describe
application collection. Both >100 ms and >110 ms counts are reported so normal
small scheduling jitter is not hidden or confused with large stalls.

A preview is attempted once; full transport slots cause a counted rejection.
AI results are also attempted once per observed source sequence; full AI slot
causes a counted rejection. Latest-input replacement and latest-result polling
can omit work/results. This implementation deliberately does not label these
omissions as successful mission events.

`source_manager_ai_completed()` currently counts attempts, including failed
JPEG decode. Normal execution must be corroborated by `ESPDET_PHASE`; the trial
rejects runs whose `AI_PROFILE` and `ESPDET_PHASE` counts differ. Zero detections
with a successful model execution is valid telemetry, not a positive GT event.

## Reproduction

```
python experiments/moq/prepare.py
AIOT_MOQT_CONCURRENT=ON bash experiments/moq/build.sh
experiments/moq/vendor/venv/bin/python experiments/moq/device_trial.py --dual --name UNIQUE_RUN_NAME
```

The device harness temporarily flashes the isolated build and restores the
known full-flash backup in `finally`. It requires the configured physical board
and local receiver address from this workspace. Private credentials are not
printed or changed. Test images and raw logs remain under ignored directories.

Host dual-profile validation:

```
cc -shared -fPIC -Isrc/components/transport src/components/transport/moqt21.c -o experiments/moq/results/libmoqt21.so
# Start receiver.py with --dual, local certificate/key and output directory.
# Run host_publisher.py with --dual and --lib, --jpeg, --cert.
```

The C and Python peers are in-project implementations. Passing this profile is
not third-party MoQ interoperability or full draft conformance. No RF weakening,
handover, synchronized host/device latency, GT, or event deadline success is
measured in this stage.

## First attempt: keep as invalid AI-load evidence

`results/device-dual-v1` completed the dual-track transport lifecycle but has
62 inference attempts, only 22 actual model runs, and 40 RGB output allocation
failures. It must not be used as a successful concurrent-AI workload result.
The corrective change reuses the already available aligned RGB decoder buffer
implementation under the MoQ concurrent opt-in flag. The first attempt also
added a whole 100 ms delay after some overruns; the corrected release grid no
longer accumulates that extra delay.

## Corrected physical run: `results/device-dual-v2`

One 60 s collection interval plus the explicit drain window; no network shaping.
This is a functional characterization run, not a repeated comparative benchmark.

| Measurement | Result |
|---|---:|
| Real camera acquisitions | 598 |
| Mean collection rate (first-to-last completion) | 9.950 Hz |
| Completion gap p95 / p99 / maximum | 154.6 / 175.5 / 315.4 ms |
| Gaps >100 ms / >110 ms | 242 / 58 out of 597 |
| Camera acquisition failures | 0 |
| Preview-cache lock misses / overwrites | 19 / 0 |
| Preview submission attempts / refused | 578 / 563 |
| Preview admitted / completely received / SHA256 matched | 15 / 15 / 15 |
| Actual ESPDet executions / observed results / received results | 27 / 27 / 27 |
| AI result admission refusals | 0 |
| Inference wall time p50 / p95 | 2.229 / 2.325 s |
| Sampled minimum internal free / PSRAM free | 32,347 / 1,949,016 B |
| RGB allocation failures / watchdog / panic | 0 / 0 / 0 |
| Publication lifecycle | Both tracks completed; subscriber FIN; close code 0 |

Preview accounting is explicit: 598 captures = 19 cache lock misses + 578
submission attempts + 1 final cached image at the collection cutoff. Of 578
attempts, 563 were refused by the fixed two-image in-flight limit. Thus 15/15
admitted delivery is **not** 100% camera-frame delivery: only 15/598 collected
images reached the server. The reserved AI slot admitted every observed result
in this run, but without a no-reservation control this is not a measured QoS gain.

All 27 inference results reported zero detections. Each has a matching actual
ESPDet model-execution record; they are valid negative results. They provide no
screw-detection accuracy or GT-event deadline success measurement. In particular,
27/27 result delivery does not account for objects missed between selected AI
inputs. Model execution is far below the camera's acquisition frequency.

The mean rate is near the target, but this run **fails a strict requirement of
one completed acquisition every 100 ms**. Retained RGB memory removes observed
allocation failures; it does not solve capture jitter, sparse image delivery,
or event deadlines. First-to-last rate and sampled heap minima are not hard
real-time/worst-case memory bounds.

Next work is to measure bounded time spent in the QUIC main-loop service and
camera acquisition, introduce object-age deadlines/cancellation and reconnect,
and compare fixed policies under matched input/weak-link conditions. Mission
success requires annotated events, including events never inferred or received.


Validation: the firmware build, C ASan/UBSan codec checks, seven Python unit
tests, and host dual-track lifecycle passed. The corrected device run matched
all admitted JPEG hashes and all 27 actual AI results, then closed with code 0.
The original full 8 MiB flash was restored; five baseline CLI settings were
acknowledged and seven FOMO AI runtime samples on CPU0 were observed. No NVS
save was issued. Temporary receiver port 14568 is stopped. The passing label
means transport/AI functional validation, **not** satisfaction of hard 10 Hz
or mission QoS. Firmware/source hashes and restore verification are stored
with `results/device-dual-v2/manifest.json` and the other local run artifacts.

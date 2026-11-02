# MoQ source map

## Firmware

| Responsibility | Source |
|---|---|
| App entry and experiment selection | `src/main/app_main.c`, `app_moq.inc` |
| Capture, AI, evidence and codec coordination | `src/main/app_moq_concurrent.inc` |
| JPEG conversion / single-flight worker | `src/main/moq_representation.{c,h}`, `moq_codec_worker.{c,h}` |
| Main component experiment options | `src/main/moq_options.cmake` |
| MOQT draft-21 wire codec: messages, parameters, filters, properties, data streams | `src/components/transport/moqt21.{c,h}` |
| Publisher protocol state: subscriptions, FETCH and fill, namespaces, GOAWAY, FETCH cache index | `src/components/transport/moqt_core.{c,h}` |
| QUIC connection integration / object lifecycle | `src/components/transport/ngtcp2_sample.c`, `moqt_ngtcp2.inc` |
| S0–S5 selection and draft-21 wire-priority order (section 5.1.2) | `src/components/transport/moqt_scheduler.{c,h}` |
| Pacing and in-flight caps | `src/components/transport/moqt_tx_policy.h` |
| Project service estimator / feasibility | `src/components/transport/moqt_feasibility.h` |
| Literature estimator arithmetic | `src/components/transport/moqt_swma.h` |
| Transport experiment options | `src/components/transport/moq_options.cmake` |
| Exact source snapshots and acquisition | `src/components/sources/source_async.inc` |

Paths above are relative to the project root. `.inc` files intentionally share
their parent translation unit's private state; compiling them as independent C
files without changing those interfaces would change behavior. The unbuilt
`moqt_http3_client.cpp` placeholder is not an active implementation.

## Host experiment code

| Entry | Purpose |
|---|---|
| `receiver.py` | MOQT-21 subscriber and raw event recording (push-only profile) |
| `receiver_draft21.py` | Experiment receiver: pushed results, evidence pushed or fetched by Group ID |
| `subscriber.py`, `moqt_wire.py` | Full draft-21 subscriber and independent Python codec |
| `host_core.py`, `host_core_shim.c` | Drives the firmware's C core over real QUIC on the host |
| `remote_ota_trial.py` | One finite OTA trial, scoped shaping, finally restore |
| `ladder_suite.py` | Frozen firmware manifest and randomized repeated trials |
| `flow_shaper.py` | Only the dedicated device flow's network impairment |
| `analyze_remote.py` | Sender/receiver accounting and transport metrics |
| `analyze_ladder.py` | Frozen JPEG ladder validation, aggregation and plots |
| `analyze_evidence.py` | Same-source result/evidence matching |
| `analyze_codec_suite.py` | Codec cohorts and provenance validation |
| `freeze_codec_build.py` | Archive selected source/configuration identities |
| `run_checks.sh` / `run_checks.py` | Single host-test entry point |
| `tests/` | Unit tests and hardware API stubs |
| `literature/` | Pinned reference mechanisms, adapters and comparison contract |

`results/` contains immutable measurements, firmware archives and reports;
`fixtures/` contains frozen input images. `vendor/` and Rust `target/` contain
downloaded dependencies/build products. Generated directories are ignored by Git,
not deleted. Existing experiment CLI paths are retained so old run manifests and
reproduction commands remain valid.

## Editing and reproducing

1. Change the smallest relevant module and run `bash experiments/moq/run_checks.sh`.
2. Build with explicit experiment options; do not rely on the previous CMake cache.
3. Archive a new firmware version with `freeze_codec_build.py`. Never overwrite a
   binary already used in a measured cohort.
4. Run a new named suite and retain failures as well as successes.
5. Keep paper reproductions, adaptations and engineering ablations distinct.

Build artifacts are deliberately not cleaned by this workflow. Model weights,
camera captures, experiment outputs and the deployed recovery image stay in place.

## Draft-21 coverage

The feature checklist, its verification level and the limits of this profile are in
`papers/mission_aiot_moq/MOQT_DRAFT21_CHECKLIST_KO.md`. Firmware options added for it:
`AIOT_MOQT_WIRE_PROFILE`, `AIOT_MOQT_EVIDENCE_DELIVERY`, `AIOT_MOQT_POLICY=6`, `AIOT_MOQT_CACHE_KB`,
`AIOT_MOQT_CACHE_OBJECTS`, `AIOT_MOQT_BEHIND_LIMIT`, `AIOT_MOQT_SUBSCRIPTION_LIFETIME_MS`.
The sources before this change are kept in `revisions/pre-draft21-checklist-20260928/`.

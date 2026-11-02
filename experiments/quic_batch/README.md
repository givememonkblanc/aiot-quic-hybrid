# 1024×1024 JPEG QUIC submission batching

[측정 결과와 QoS 해석](RESULTS.md).

Opt-in experiment (`AIOT_QUIC_BATCH_EXPERIMENT=ON`). Default firmware paths are unchanged when disabled. `batch 1|4|8|16|0` selects KiB of JPEG data per QUIC submission; `0` groups a frame (up to 256 fragments). Change this setting with preview stopped and its queue drained.

## Controlled input and wire format

`private/trace.jpg` is a real, previously received 1024×1024 OV2640 JPEG: 123,179 bytes. `trace.json` records its SHA-256. The experiment repeats this **one-frame trace**, runs replay at nominal 10 Hz and attempts preview admission at 5 Hz. The existing AI offer function caps offers at 5 Hz and retains the latest pending input. It is not a changing-scene video or an accuracy benchmark. Physical capture continues in the background.

Every condition uses 1,024 JPEG bytes per application fragment (the legacy source used 1,000). There are 121 fragments/frame: 120 × 1,024 plus 299 bytes. Each has the existing 12-byte application object header. Maximum submitted DATAGRAM data is therefore 1,036 bytes, below the local 1,180-byte cap. The peer's advertised DATAGRAM-frame limit is also checked, including framing overhead.

The queue continues allocating/copying these same fragments in all conditions. The TX worker groups references immediately before calling `quic_client_write_batch_safe`; this isolates QUIC API submission/locking from changing JPEG allocations. A frame submission **does not become one large QUIC DATAGRAM**. `ngtcp2_conn_write_datagram` is still called separately for each fragment, without `WRITE_MORE`. Its packet outputs are sent individually. Header format and receiver remain unchanged.

Grouped/dequeued fragments remain counted in route payload memory. The admission gate also checks an in-flight image flag, preventing a large group from appearing to have emptied the pipeline while still transmitting. Partial groups are possible at the end of a frame or a queued control-message boundary; actual calls and fragments are counted.

The formal comparison retains a single aligned 3 MiB RGB decode buffer in PSRAM for every condition. The initial smoke test showed recurring allocation failures, eventually eliminating the AI workload in larger-group conditions. That test is preserved as invalid for a concurrent-AI performance ranking. The retained buffer uses the same JPEG decoder, RGB888 format, full 1024 resolution and model; only output-buffer allocation lifetime changes. This opt-in correction prevents a failed AI task from falsely making transport appear faster.

## Locking and scheduling

All conditions use the same application settings, 2 s image deadline, 100 ms pacing-refusal retry allowance per fragment, and 20 ms connection-lock hold target. A packet operation can finish after that target; it is a cooperative bound, not hard preemption. Release and yield on pacing refusal or budget exhaustion; never sleep holding the lock. The existing 3 ms outer TX-worker yield check remains the same across group sizes. Batching therefore changes scheduling opportunities as well as API/lock frequency; effects are not attributed exclusively to mutex instructions.

Frame grouping can require multiple lock acquisitions. QUIC STREAM/MQTT work remains serviced. Failed or expired frame tails are dropped consistently. Queue, internal heap, PSRAM and watchdog guards remain enabled. AI stays on core 1 at priority 1; TX stays on core 1 at priority 5. Software ChaCha20-Poly1305 remains enabled; hardware AES is not used.

## Counters

`BATCH_PERF` contains cumulative counters; the analyzer differences aligned snapshots:

- `calls`: batch API submissions. `fragments`, `bytes`: accepted DATAGRAM data, including application headers.
- `write_calls`: ngtcp2 DATAGRAM packetizer calls, including refusals; `packets`, `packet_bytes`: successful UDP send outputs of that packetizer, including QUIC overhead. These exclude other control/STREAM send paths; they are not a universal count of every packet on the connection. During established 1-RTT image traffic these outputs are individual short-header packets. No claim that a large application group is one packet.
- `locks`, `wait_us`, `hold_us`: image-batch connection-lock acquisitions and wall-time cost. `all_lock_*` additionally counts read/process/MQTT and other connection-lock callers. The statistics spinlock is not a connection lock.
- `cpu_us`: image-batch API task execution time. `other_quic_cpu_us`: process/read/STREAM/single-DATAGRAM public API execution time. Their sum is instrumented QUIC I/O task CPU, not the entire Wi-Fi/ISR stack. The read-only probe in the isolated SDK includes the current task's unfinished runtime slice; blocked/preempted time is excluded. FreeRTOS attribution can still charge interrupt time to the interrupted task.
- `refused`: local non-acceptance/pacing, not network loss. `dropped`, `expired`, `failed`: batch-local unsent fragments/failing batches. Existing transport counters additionally track tails dropped outside the batching API.

Receiver goodput counts only complete JPEGs matching the fixture SHA-256. Fragment throughput, admission rejection, and admitted-but-incomplete frame cohorts are reported separately. A missing JPEG is not automatically classified as network packet loss. AI attempts and successful inference latencies are separate; JPEG decode allocation failures are not hidden in a latency average.

## Reproduce

Requires the existing isolated ESP-IDF 5.3.6 / ESP-DL build, private fixture and verified device flash backup.

```sh
cc -std=c11 -Wall -Wextra -Werror experiments/quic_batch/test_batch.c -o /tmp/test_quic_batch
/tmp/test_quic_batch
experiments/quic_batch/build.sh
# Flash the temporary 8 MiB experiment layout via USB and wait for success.
python experiments/quic_batch/trial.py
python experiments/quic_batch/analyze.py <run-directory>
# Restore the original full flash and verify firmware/configuration ACKs.
experiments/vlm_split/restore_integrated.sh
```

Formal order: 1/8/frame/4/16 then 16/4/frame/8/1 KiB, 60 s each with first 10 s excluded, 8 s drains. This mirrored sequence controls some time drift but does not hold the radio channel deterministic. Do not present two repeats as a statistically established ranking or a p99 guarantee. No network shaping or external service changes are required.

The initial formal run hit the CPU1 idle watchdog in the frame condition. This is a stability outcome, not a fast valid result. The subsequent harness stops preview immediately for a watchdog-warning condition, records it as failed, drains the workload, then proceeds to the next condition. Panic, reboot, heap guard or repeated decode failure still abort the entire run. Watchdog-stopped windows are excluded from pooled normal-performance metrics.

## QoS 방향

[서비스 QoS 고도화 로드맵](../aiot_qos/ROADMAP.md)에 따라 AI 이벤트 deadline, 영상 신선도·연속성, 안정성을 함께 평가한다. `experiments/aiot_qos/profile_results.py <run-directory>`는 기존 로그에서 같은 시계로 계산 가능한 QoS를 추가 추출한다.

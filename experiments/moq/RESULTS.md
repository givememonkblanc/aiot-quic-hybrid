# MOQT-21 first device milestone — 2026-09-22

**The ESP32 transmitted actual camera JPEGs through a native QUIC MOQT-21 single-track profile, and the subscriber verified complete objects.** This is a transport-functionality result, not QoS/mission success or full draft conformance.

## Device result

| Check | Result |
|---|---|
| TLS | Receiver certificate SHA256 pin verified; TLS1.3 encryption retained |
| Negotiation | moqt-21 ALPN; QUIC DATAGRAM capability negotiated |
| Control | Peer SETUP accepted; server SUBSCRIBE request1; SUBSCRIBE_OK alias1 |
| Track | namespace(aiot,esp32), name preview/jpeg |
| Camera | Actual OV2640 1024x1024 JPEG |
| Attempts / admitted / refused | **39 / 30 / 9** |
| Complete received / hash matched | **30 / 30** |
| JPEG bytes | 132574–134759 per admitted frame |
| Final publication | PUBLISH_DONE(TRACK_ENDED), stream count30, request FIN and subscriber FIN |
| Session end | QUIC application close code0 |
| Sampled minimum internal free | 115715B |
| Sampled minimum PSRAM free | 6420232B |
| Watchdog/panic/protocol failure | None recorded in the passing run |
| Restoration | Original full8MiB image restored with esptool hash verification |

The denominator is39 submission attempts, not30. Nine frames were refused when the two data slots were occupied. The30/30 figure only describes admitted objects; it is NOT100% mission completion or proof that dropping frames improves QoS. Group IDs can skip refused attempts and complete out of order. Every received JPEG was fully decoded with Pillow and matched against the SHA256 printed by the ESP32 for that group.

This test attempts at most one camera submission per second and stops after30 admissions. It does not establish10Hz acquisition or delivered video, AI-event deadlines, detection accuracy, weak-link recovery or scheduler gains. The isolated SDK build is ESPDet-capable but does not submit inference work in this milestone. Its RAM/throughput must not be compared to the earlier concurrent FOMO workload as a protocol-only comparison.

## Host and parser validation

The portable C wire codec passed ASan/UBSan checks for draft vi64 boundary/spec vectors, truncated reads, bounded message lengths, setup roles and the fixed subscription/object profile. Four Python tests cover spec vectors, malformed options, length/truncation and PUBLISH_DONE waiting for FIN/late objects. A separate localhost native QUIC run passed three synthetic1024x1024 JPEG objects using the C encoder and Python decoder, including publication completion and close code0.

Both peers are project implementations. This is not interoperability validation against an independent third-party MoQ library or a complete compliance suite. [Implementation scope and reproduction](IMPLEMENTATION.md) lists the unsupported messages, parameters and lifecycle features.

## Failure found and corrected

The first device run also delivered30 matching JPEGs but was marked failed: after finite publication it left the session idle and QUIC timed out. This was corrected by adding PUBLISH_DONE with exact stream count, waiting for the subscriber's request-stream FIN, and sending a normal session close. The corrected run passed. The earlier failure is preserved rather than relabeled successful.

## Current operational state and next boundary

The normal board remains on the restored FOMO application; the new MoQ path is selected only by AIOT_MOQT_EXPERIMENT. Wi-Fi/NVS credentials and the existing receiver service were not changed. Test receiver port14568 is stopped after the run. Runtime restoration verification is recorded separately.

Follow-up [independent capture and AI-result track testing](CONCURRENT.md) achieved mean9.95Hz and27/27 result delivery, with a315ms maximum capture gap and only15/598 images delivered. Remaining work includes the capture-gap requirement, evidence tracks, bounded deadlines/cancellation and reconnect. Only after that can the same-input weak-link experiment compare mission completion against a fixed latest-only baseline. A finite JPEG transfer alone does not answer that research question.

## Local artifacts

- Passing device run: `results/device-roundtrip-v2/`: outcome.json, events.jsonl, device-events.json, JPEG objects, flash/restore logs.
- Earlier failed lifecycle run: `results/device-roundtrip/`.
- Host lifecycle run: `results/host-lifecycle/`.
- Firmware/source hashes: `results/build-manifest-v2.json`; host dependency versions: `results/host-requirements.txt`.
- Restore CLI/core verification: `results/restore-verification.json`.
- Backup SHA256: `6ddb628302bb58777aded5267f3d4efd6057840e8d24551f0727e215a3d5a93e`.

Private backups, credentials, captured images and raw logs remain ignored locally.

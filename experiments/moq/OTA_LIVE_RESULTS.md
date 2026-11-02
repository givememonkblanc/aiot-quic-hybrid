# Native MoQ remote-OTA live check (2026-09-22)

## Scope

This run checks that the ESP32-S3 can be moved from the deployed UXGA
camera/FOMO image to the native draft-21 MoQ dual-track image without USB,
complete a finite session, and return to the deployed image through signed
pull OTA. It is a functional baseline, not a transport-policy comparison.

## Image

- Application: UXGA camera + asynchronous TFLM/FOMO + native MoQ preview and
  AI tracks + signed pull OTA
- Binary: `moqt-ota-fomo-v8.bin`
- Size: 1,953,584 bytes; OTA slot: 2,031,616 bytes; margin: 78,032 bytes
- Image SHA-256: `adb2b57fcbfb5dd5d485c8b450098a8e53ff4684b789b1b09a4aeb4adcad4d17`
- ELF identity: `143520599f39df6486cd9c231787bfa69dd997330139f9cd619eb896a9e67c1b`
- Compiler setting: size optimization (`-Os`). The normal mobility OTA
  configuration was returned to performance optimization after archiving the
  binary.

The full native MoQ/TLS/FOMO image exceeded the existing OTA slot under `-O2`.
The OTA image also omits the fixed replay JPEG, which the live camera path does
not use. No model, live capture, transport, or OTA recovery feature was removed.

## Successful run

Receiver evidence is in `results/moqt-ota-fomo-v8-live2/events.jsonl`.

| Observation | Result |
|---|---:|
| QUIC/MoQ handshake | success, ALPN `moqt-21` |
| Subscriptions | `preview/jpeg` and `ai/events` |
| Preview objects | 32 |
| AI objects | 11 |
| Total completed objects | 43 |
| Preview payload | 3,145,654 bytes |
| AI payload | 1,617 bytes |
| Preview dimensions | 1600 x 1200 JPEG |
| Preview receive interval, median / p95 | 1.879 s / 2.159 s |
| Preview payload goodput | 0.413 Mbit/s |
| AI receive interval, median / p95 | 6.417 s / 6.501 s |
| Device capture-to-AI-observed, median | 6.658 s |
| Device capture-to-AI-observed, range | 5.700-8.762 s |
| Track completion | 32/32 preview, 11/11 AI |
| Connection close | clean, code 0 |

The first check (`results/moqt-ota-fomo-v8-live`) received three valid UXGA
JPEGs and then exposed a receiver bug: it accepted only ESP-DL backend 3 even
though this OTA build intentionally uses the deployed TFLM backend 2. The
receiver now accepts model backends 2 and 3 and checks that the JSON and binary
backend identifiers agree. The table above uses only the clean rerun.

## OTA evidence and limits

The OTA service log records this sequence:

1. deployed image `0ccf...b8e9` downloaded the MoQ image;
2. the MoQ image polled with current identity `1435...c1b`;
3. after the run, the MoQ image downloaded the deployed UXGA image again.

The run had no USB serial connection. Consequently receiver-side object count,
dimensions, hashes, clean completion, and OTA identities are verified, while
device-side capture count, heap trace, scheduler runtime, and overwrite count
are not available for this run. Preview receipt is only about 0.54 frame/s, so
this result does not establish 10 Hz media delivery. It establishes the safe
remote measurement path needed for the next pacing/unacknowledged-byte-cap
comparison.

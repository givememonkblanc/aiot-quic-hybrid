# Communication quality experiment status — 2026-09-26

**The 27-run comparison remains incomplete. v29 stopped at 13 validated runs. USB-controlled, no-OTA measurements resumed on September 26; see the recovery section below.**

## Preserved cohorts

- v26: one valid measurement; next trial mixed sessions and stopped.
- v27: five measurements; next target download was not observed. Historical
  single-poll restore flags do not establish durable restoration.
- v28 recovery: measurement complete, stable operational restoration failed;
  no v28 quality matrix was launched.
- v29: 13 measurements passed integrity and stable restoration checks. Trial 14
  (`wifi-quality-v29-medium-medium-r2`) completed measurement but failed
  restoration. It is excluded from the validated comparison. Shaping removed.

Original manifests are retained. The comparison tables/plots under
`results/wifi-quality-v29/` are partial descriptive results, not a finished
three-repeat comparison, physical screw recall, or measured RF-quality sweep.

## Recovery investigation

USB recovery on September 25 validated the original camera image normally.
See `../ota/results/usb-recovery-20260925/RESULTS.md`. Later remote restoration
failed again. Failed boot telemetry showed frames=0 and ai_us=0 at 56 seconds.

The operational image gates local camera/AI boot health on MQTT/preview
admission. `camera-local-health-v4` removed that dependency during pending
validation while retaining real camera/TFLM checks, rollback, and the
previous-attempt guard. It built but rolled back remotely: **not a fix**.

`camera-local-health-v5` adds non-sensitive health counters to the existing
pinned HTTPS OTA poll. Diagnostic verification is in progress. This separates
camera initialization, valid capture, inference, and pending-validation state
without requiring successful MQTT delivery. Neither image is measurement data.

The runner now supports an explicit frozen recovery image, preserving the
original measurement binaries. It retains the MQTT/QUIC recovery service on
14567 and no longer starts an unrelated MoQ receiver for operational recovery.
`restore_verified` means stable image identity, not sustained live streaming;
`operational_streaming_verified=false` makes that limitation explicit.

v5 has confirmed local camera/AI health (pending=0, camera=1, frames=1,
inference about 2.16 s). Its frame counter remains at the boot self-test value,
so operational streaming is still unresolved. Six consecutive polls over more than 150 seconds were verified for the diagnostic
image before v30. v30 then failed its first restore. A subsequent attempt to
restore the original baseline produced only one baseline poll before the medium
test identity returned, so no new cohort is being launched. This recovery change
is recorded separately; v29 is not pooled into the new 27-run matrix.

A replay intermission binary was prepared but not deployed or used. No camera
health check was bypassed. v6 corrects the diagnostic backend counter and has
built successfully, but is not deployed; v5 remains the frozen recovery target.

Run plan: low / medium / EWMA × good / medium / poor × 3 repeats, seed 20260924. Launch metadata and current state: `results/wifi-quality-v30/launch.json` and `manifest.json`. First restore failure stops the suite; report watcher updates completed-run charts automatically.

Baseline restore diagnostic v31 was unstable: one baseline poll, followed by a return to the medium test identity. The original 13 validated v29 runs remain the last valid cohort; v30 first measurement is retained but invalid for the comparison because restoration failed.

The OTA release was then aligned to the currently running medium test image to
stop repeated baseline-download attempts. The device is now polling 204 for
that identity. No new OTA trial is active. The v30 first-run measurements are
summarized separately in `results/wifi-quality-v30-medium-poor-r1/PARTIAL_RESULT.md`.

A controlled v31 low/good run passed with medium firmware as recovery. A
following low/medium retry was blocked by the persistent previous-attempt guard
before download; no measurement occurred. An attempted rebuild with current
sources exceeded the 0x1f0000 OTA slot and was not deployed. The service was
reset to the medium image and shaping was removed. No bypass or NVS erase was
performed.

An exploratory ladder-v17 low image was also rejected by the same previous-
attempt guard before download. It was not pooled with v25 data. The reserved
IFB and ingress filter were explicitly removed after the bounded trial stop.

The size issue is now understood: the current default vehicle model is 3.49 MB
and links into replay builds unless the 307 KB person model is explicitly
selected. A person-model low build (`a82fa8f3...`) is 1,610,592 bytes and fits
the OTA slot. It was offered as a new identity, but the installed device never
requested the image bytes; the trial was stopped and shaping removed. This is
now an OTA service/device polling failure, separate from the previous-attempt
guard. The image was not deployed.

The correct experiment architecture is therefore: install each policy image
once after a verified USB/NVS baseline, run all good/medium/poor network cells
without OTA rotation, and restore only at the end of that policy block. The
per-cell OTA runner is being retired from the main protocol.

## USB recovery, September 26

The local ESP32-S3 is accessible at `/dev/ttyACM0`. Direct flash app-header
inspection at 0x200000 confirms ELF identity
`e5bafc0b28c7f0d617a39e31d88b9003cffea82e622d5a029150f1e128ee960c`
(frozen **low**, 400x300 replay), not the previously assumed medium image.
The release server was offering `ccde03...`; the serial log reports that target
as blocked_previous_attempt. Release now matches the running image. No NVS
key was erased and no firmware was flashed. This aligns the OTA offer but does
not prove future cross-image OTA or operational live-camera restoration.

Starting the receiver before a controlled USB reset restores measurement.
`network_only_trial.py --usb-port /dev/ttyACM0` now starts the receiver, waits
for its UDP socket, resets the installed image, captures serial logs, and
requires exactly one handshake and a finished session before analysis.
The initial diagnostic run was incomplete/mixed and is excluded.
The good and medium clean-run directories have a provisional `medium` label;
their manifests explicitly correct the representation to **low**.

This is a new USB-controlled cohort, not automatically pooled with OTA runs.
Input is replay and AI result counts are not screw recall or mission success.
For the complete results see `results/usb-recovery-20260926/RESULTS.md`.

Backup correction: the earlier file called `otadata.bin` was read from 0xe000
(PHY area). It is renamed `phy-and-gap-0xe000.bin`; the actual OTA data at
0x3f0000 is backed up as `otadata-correct-offset.bin`. See the USB backup
layout note under `../ota/results/usb-inspection-20260926/`.

## Remote continuation after moving device — v32

USB is absent. The device is reachable at its established Wi-Fi address and
continues polling the OTA service. The frozen v25 EWMA image was downloaded,
completed one good-profile trial and retained its identity across three HTTP204
polls. SWMA also completed using the same profile, with post-run identity verified. This uses install-once
OTA without restoration; it does not fix automatic restarting of the same
finite image. New results stay in `results/remote-v32/` and per-run v32 folders.
EWMA/SWMA provenance differs only in estimator setting and project version;
RTOS values and recorded source hashes match.

## v33 repeated remote matrix started

`remote_comparison_suite.py` launched frozen low/medium/EWMA/SWMA × good/medium/poor
× three repeats (36 trials), seed 20260926. Each repeat block contains all 12
cells; shuffle rejects consecutive identical policies because the installed
finite image cannot restart itself. No recovery-image detour. Every cell must
pass terminal accounting, a single handshake and two fresh post-measurement
OTA identity polls; first failure stops the supervisor.
Live state: `results/remote-v33/manifest.json`; supervisor log:
`results/remote-v33-supervisor.log`. This matrix is not complete merely because
it has been launched. Physical mission readiness: ../screw_mission/MISSION_READINESS.md.

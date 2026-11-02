# Controlled communication-quality experiment

Date: 2026-09-24. This is a host-impaired Wi-Fi-connected ESP32 experiment,
not a controlled RSSI, radio interference, or handover measurement.

| Profile | Forward cap (kbit/s) | Added forward delay (ms) | Added random loss (%) |
|---|---:|---:|---:|
| good | 2048 | 5 | 0 |
| medium | 512 | 25 | 1 |
| poor | 128 | 75 | 3 |

Only the dedicated device-to-receiver UDP flow is impaired. Reverse ACKs,
OTA, unrelated flows and the physical Wi-Fi configuration are unchanged.
Existing path latency/loss adds to netem; these are not measured total RTT/loss.
The 64-packet netem limit is identical and queue overflow can add further loss.
Profiles jointly vary three parameters, so outcomes compare composite service
conditions and cannot isolate a causal loss-versus-bandwidth effect.

Frozen v25 binaries: fixed low, fixed medium, EWMA representation selection.
Same image/model, offline JPEG ladder, 60-second offer window, AI-first policy,
one image slot, 32-KiB unacked cap, 2-s preview and 10-s AI deadlines.
Three randomized blocks: 3 profiles x 3 policies x 3 repeats = 27 runs.
Each finite OTA run must restore the baseline; stop on trial/restore failure.

Report received AI and ACK-confirmed deadline counts, complete image counts and
resolution, AI ACK p95, preview gaps/goodput/resets, and heap minima. Preserve
failed runs. This does not measure physical screw recall or live capture cadence.
Compare run means/ranges, not frame-level pseudo-replicates.

Command:
```sh
experiments/moq/vendor/venv/bin/python -u experiments/moq/ladder_suite.py \
  --name wifi-quality-v26 --policies low medium ewma \
  --networks good medium poor --repeats 3 --seed 20260924 \
  --firmware-root experiments/moq/results/literature-v25-firmware --execute
```

## v27 acquisition amendment

v26 stopped after one complete run and one mixed-connection failure; both
restored baseline. The failed raw log contains objects from an earlier session,
so it is not eligible for setup-only filtering or a silent success relabel.
The receiver previously listened before publishing OTA, allowing the operational
firmware to send data into the next trial directory. v27 publishes first and
waits for the target image HTTP download request before starting the receiver.
This gate is not proof of successful boot; target identity and subsequent baseline
restoration are still checked separately. Multi-session measurement logs remain
invalid. Firmware, network profiles, seed and policy settings are unchanged.
The complete 27-cell matrix is restarted as a separate cohort, not pooled with v26.

## v28 recovery amendment

v27 completed five measurement runs, then stopped at the sixth with no target
image download and failed restoration. Historical service records show baseline
polls followed about two minutes later by the prior test identity. Thus the old
single-poll restoration check was insufficient to establish durable recovery.
These original records are preserved, not silently relabelled.

A separate unshaped recovery diagnostic uses the previously validated low image.
Restoration now starts a dedicated receiver in a separate recovery directory,
so baseline end-to-end boot confirmation can run; require at least three baseline
polls and 150 seconds stable observation. The rollback mechanism is not disabled.
Only after successful diagnostic restoration may the v28 matrix start. Repeat
the same frozen firmware/settings in a fresh 27-run cohort. Recovery traffic is
excluded from measured logs and shaping is removed before recovery.

## v29 after USB recovery

USB readback found the unchanged baseline in ABORTED state. A diagnostic boot
retaining normal NEW/PENDING_VERIFY validation completed real camera/AI work;
readback then confirmed VALID. No NVS/firmware edits or forced-valid state were
used. The precise cause of prior unattended rollback remains unresolved.
Restart the same 27 conditions as wifi-quality-v29 with the stronger restoration
check. Keep v26/v27 failures and the failed v28 recovery as separate artifacts.

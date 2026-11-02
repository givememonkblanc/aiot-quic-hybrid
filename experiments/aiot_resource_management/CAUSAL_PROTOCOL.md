# Image-path isolation protocol

This extends A–E profiling without adding a resource manager.

- E1 = profile 3: camera JPEG capture/copy + local FOMO + MQTT result metadata. OV2640 outputs JPEG; there is no additional MCU encoder to switch off. Non-AI preview output is discarded.
- E2 = profile 6: same work plus image chunk allocation/copy/enqueue; the existing net_tx worker immediately dequeues/frees image chunks before QUIC/socket calls. Network admission gate is bypassed in this case; submitted bytes must be checked.
- E3 = profile 5, probe 1: same work plus actual QUIC image transmission, with at most one image attempt per second. Current Wi-Fi path has no imposed impairment. It is not a proven fastest link or loopback test.
- E4 = E3 plus an isolated server-ingress IFB matching ONLY ESP32 source IP and destination UDP14567. 512kbit/s +50ms one-way delay, queue limit64 packets. Root qdisc unchanged; filter and IFB removed in finally.
- E5 = separate build with AI pinned to core1 instead of core0, keeping priority0 and model unchanged. Camera/main remain core0, net_tx remains core1. Compare E1/E3 again; moving AI to core1 also places it beside net_tx and is not a free dedicated compute core.

Main experiment sequence: E1,E2,E3,E4,E4,E3,E2,E1. 45s each, first15s excluded. At 1 attempted image/s the new E3 is not directly interchangeable with the earlier unrestricted-preview A–E experiment. Live frame content and actual admissions vary; record frame bytes and accepted/rejected attempts.

Measurements: both core idle-derived utilization; every task's cumulative runtime, ID and affinity at5s intervals; per-inference scheduled task CPU time vs wall time; JPEG decode wall time vs full model path (input processing + Invoke + postprocess); per-attempt image enqueue duration/size; heap/queue, real receiver counts. No standalone JPEG or MQTT task is invented: shared main and net_tx attribution is explicit.

Run:

```bash
python3 experiments/aiot_resource_management/profile_trial.py --causal
python3 experiments/aiot_resource_management/analyze_causal.py RUN_DIR
# after deploying the core1 build:
python3 experiments/aiot_resource_management/profile_trial.py --pin-test
```

No application ground truth, accurate per-event deadline success or energy is claimed. These are short mechanism-isolation trials. CPU runtime can include interrupt attribution and excludes descheduled time; wall-minus-runtime alone does not identify a particular mutex or scheduling cause.

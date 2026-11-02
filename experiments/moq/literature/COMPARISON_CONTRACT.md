# Literature comparison contract — 2026-09-24

The completion target is an auditable source→implementation→validation→result
chain for single-link applicable mechanisms, plus explicit exclusions. General
FIFO/EDF/DRR baselines do not count as paper reproductions. Protocol mechanisms,
rate estimation, and media coding solve different problems and are not assigned
one misleading overall rank.

## Cohorts fixed before new measurements

1. **MMSys 2023 live-player estimation math**: original public-moq-demo commit
   `2071137a5967e645ab97b6e914dc4f30c8c47d95`; differential C/JS validation of
   SWMAth and IFA. Preserve window25/interval5/trim5%, first-segment-chunk IFA,
   and outlier rules. Original browser timestamp semantics remain documented.
2. **ESP32 application transfer of SWMAth**: same-source offline JPEG ladder,
   fixed FOMO input, 60 s preview generation, S1, one image slot, 32 KiB ACK-held
   cap, 2 s image and 10 s AI deadlines. Compare fixed-low, fixed-medium,
   project-EWMA and SWMAth with the SAME feasibility selector, safe-low start and
   timeout rule. No early feasibility rejection (to isolate rate selection).
   Both estimators observe the same ACK-held service interval, including resets.
   This is explicitly **not the original receiver-side bandwidth experiment**.
   Randomized 3 blocks × 4 policies × (128 kbit/s,512→128→512 kbit/s), +50 ms.
   Preserve failed runs and restore/verify baseline after every OTA run.
3. **TrackSwitch**: inspect original author draft, ICME workshop slides, and
   MOQtail implementation pinned separately. Test actual alternative tracks and
   switching boundaries; varying JPEG dimensions on one track does not count.
   Compare atomic switching to separate subscribe/unsubscribe with controlled
   control-message delay. Report overlap, gaps and switch delay. A newer reference
   implementation is not automatically the 2025 evaluated artifact.
4. **Freeman implicit adaptation**: original mechanism requires independent
   descriptions, priority per track, timeout since first arrival at relay,
   group-wide cancellation and receiver reconstruction. A JPEG timeout alone is
   not MDC, and event+dependent enhancement is not disjoint point partitions.
   Record the exact adapted workload and unavailable original artifacts. Do not
   invent PCQM or claim full point-cloud reproduction on ESP32.

Multipath/multicast algorithms require path/viewer setups absent from the current
device experiment and stay explicitly excluded. LL-DASH is a system reference,
not an interchangeable MCU scheduling algorithm. Original paper code availability
and mechanism coverage must remain separate columns.

## Analysis rules

Run-level summaries and ranges (n=3), no frame-level pseudoreplication. Count all
offered/accepted/reset/delivered objects and observed deadline ambiguity; dropping
everything cannot win. Include all fixed baselines, quality-level occupancy,
goodput, event age, evidence/preview gaps, heap and acquisition rate. Frozen JPEG
hashes establish image identity only; they do not establish screw accuracy.
Original inference GT and per-screw mission success remain separate pending work.

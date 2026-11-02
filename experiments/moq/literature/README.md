# Literature reference experiments

Start with [COMPARISON_CONTRACT.md](COMPARISON_CONTRACT.md) and
[NOTICE.md](NOTICE.md). This directory distinguishes original code, arithmetic
ports, workload adaptations and measurements. A passing unit test is not an
end-to-end reproduction result.

| Source | Pinned commit | Local path | Current verification |
|---|---|---|---|
| [MMSys 2023 demo](https://github.com/streaming-university/public-moq-demo) | `2071137a5967e645ab97b6e914dc4f30c8c47d95` | `vendor/public-moq-demo` | SWMAth/IFA arithmetic differential check |
| [MOQtail](https://github.com/moqtail/moqtail) | `5711a63936d31af9f644a712c0fef40fcd30c120` | `vendor/moqtail` | 381 upstream tests pass; switching performance incomplete |
| [Freeman fork](https://github.com/ac-freeman/moq-rs) | `f18fae1f520f074bd61344bbe7ee07530a879e08` | `vendor/freeman-moq` | 116 library + 3 doc tests pass; timeout performance incomplete |

## Entry points

- `verify_estimators.py`: extracts the pinned live-player methods and compares
  their results with the C port, including warmup, outliers and tied rates.
- `build_estimators.sh`: builds explicitly configured new ESP32 variants and
  archives provenance. Existing firmware outputs are never overwritten.
- `track_client/`: actual-QUIC evaluation driver using the original MOQtail
  library. The relay's socket is patched to IPv6 loopback only; algorithm code
  is unchanged. This tests a later author implementation, not the exact 2025
  paper artifact. Performance validation is still in progress.
- `run_track_switch.py`: finite loopback trials with owned child processes and
  per-run logs. The relay's application write cap is not an OS network shaper.
- `results/`: generated tests, diagnostics and trial records, ignored by Git.
- `vendor/` and `**/target/`: downloaded sources and build products, ignored by Git.

For a fresh checkout, clone each URL into its local path and check out the exact
commit above before running the verification. Upstream licenses stay in their
repositories. No source snapshots or measured firmware are silently updated to
new upstream revisions.

The first v24 low run completed and restored. The following EWMA run was
interrupted before its test identity was observed; its restoration-order check
therefore failed. Later OTA polling independently confirms the baseline image.
Those records are retained and are not part of a completed comparison matrix.
v25 firmware includes the tied-sample ordering correction; its repeated matrix
has not been executed. Source organization changes do not make these experiments
complete.

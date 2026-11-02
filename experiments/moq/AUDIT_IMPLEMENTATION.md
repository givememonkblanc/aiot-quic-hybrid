# P0: bounded deferred measurement journal (v7)

This implements the first P0 instrumentation item from
[RESEARCH_ROADMAP.md](RESEARCH_ROADMAP.md). It does not complete clock
synchronization, mission GT, physical10Hz validation or the entire P0 phase.

## Capture path

`AIOT_MOQT_AUDIT=ON` requires the concurrent policy experiment. Default is OFF.
`common/moq_audit.c` allocates exactly384KiB of PSRAM once before initialization.
Selected MOQ/AI/REPLAY/ESPDET measurement records are formatted into a bounded
512-byte stack buffer and appended as a little-endian uint16 length followed by
UTF-8 text. A mutex serializes the two cores. No overwrite/wrap is permitted.
Overflow, truncation and post-freeze append attempts are counted as invalid
measurement conditions. Unexpected errors are still surfaced immediately.

This deliberately preserves the original field schemas for analysis. It is a
bounded variable-length text journal, not a compact binary event encoding.
Formatting still happens during measurement; USB export moves after the window.
The384KiB capacity is suitable only if verified for the chosen run duration.
Longer180s experiments must size/compact/drain the ledger first, and account for
that cost. Aggregate `append_wall_us` includes formatting, lock wait and copy;
it is not isolated CPU execution time and overlapping calls can overlap in wall
time. The mutex and bounded stack buffer also have a resource cost.

Camera stops before cleanup. The application waits for pending/active inference
to quiesce, records remaining preview/AI events, and freezes the journal. Failed
quiescence is an error. The journal remains allocated until board reset, so a
lost USB page can be requested through a repeated stop-and-wait transfer.

## Transfer integrity

`AUDIT_BEGIN` declares schema, capacity, byte count, record count, page count,
standard CRC32 and loss counters. Pages carry index/offset/length/CRC and hex
payload, up to256 raw bytes each. The host acknowledges each page; a missing ACK
causes retransmission (five attempts,1.5s per attempt). Begin and end also require
ACK. Bad page CRC, unknown/missing records or a changed duplicate cannot complete
successfully. Overall CRC and record framing/count are checked before accepting
`AUDIT_END`. A truncated export is a failed trial, not a shorter valid dataset.

Host parser: `audit_reader.py`. `device_trial.py --audit` retains `audit.bin`,
`audit-manifest.json`, original serial capture and reconstructed event records.
Reconstructed records have `host=null`: their USB receipt time is not the event
occurrencetime. MCU source timestamps remain in fields; the receiver retains
its independent timestamps. No source→server clock mapping is invented.

`--audit-retry-test` intentionally withholds the first page ACK and requires a
matching duplicate page, testing retry without dropping actual experimental data.
`validate_audit.py` reconciles capture/replay IDs and final device count, AI
start/profile/success/phase records, and terminal states for every preview and
completed AI result. States include received, replaced, cache miss, reject-new,
publish rejection, expiry, event queue rejection and pending at cutoff.

## Validation scope

Host tests cover exact reconstruction after an ACK omission, corrupt/missing
pages, overflow metadata, wrong whole-journal CRC and changed duplicates.
Firmware build and temporary device trials are reported below once complete.
The trial harness always restores a freshly read full-flash baseline.

Compare v7 journal ON/OFF as an instrumentation characterization, not a new
scheduler benchmark. The384KiB allocation and elimination of hot-path USB output
change contention. Do not silently combine these samples with v6 policy results.

## Device integration failures retained

`audit-v7-on` and `audit-v7-on-r2` stored roughly259KB with zero header loss
counters, but timed out before transferring their first page. Both were restored
and remain failed attempts. An initial suspicion of competing console readers
was incorrect: the dedicated MoQ entry point bypasses the product console.
The second attempt's ACK mailbox consequently had no active console reader.
The corrected implementation installs the USB Serial/JTAG driver only after
measurement, reads ACKs directly through that driver, then uninstalls it after
export. A host-write check against restored baseline CLI succeeded, separating
host fd transmission from the experimental firmware's receive path. Header
counts alone never establish a successful verified export.

## Successful hardware journal test

`audit-v7-on-r3` passed with2650 records,259514 raw bytes in1014 pages and a
matching whole-journal CRC32. The injected missing ACK caused exactly one
matching page retry; invalid frame, overflow, truncation and late-write counts
were all zero. Every600 replay offers reconciled:561 replaced,5 cache misses,
33 complete received JPEGs and1 pending preview at cutoff. All24 actual
inference attempts succeeded, produced24 unique results and were received.
Both `validate_audit.py` and existing object/ACK analysis passed.

Aggregate append wall time was562681us over the run. This is not a CPU% estimate.
Allocated journal storage is393216bytes (384KiB); actual occupancy was259514bytes.
The live capture requirement and exact source→server latency remain unmeasured:
input was replay and receive clocks are not synchronized. The volatile journal
cannot survive power loss or a crash/reset; those remain failed runs rather than
reconstructed successful cohorts. ACK-based export is for lab measurement,
not a deployed MCU persistence mechanism.

## Journal ON/OFF characterization

Both runs used AI-first, latest replay, one JPEG slot and the same v7 sources;
each is one unshaped trial. Actual inference success and payload hashes were
verified, and both source ledgers reconciled in analysis.

| Metric | Journal ON (r3) | Journal OFF |
|---|---:|---:|
| Replay offers | 600 | 600 |
| AI completed / received | 24 /24 | 24 /24 |
| JPEG received | 33 | 31 |
| Inference wall p50 | 2.524s | 2.548s |
| Inference wall p95 | 2.567s | 2.579s |
| Sampled min free PSRAM | 1521532B | 1914784B |
| Sampled min free internal heap | 32379B | 32283B |

The sampled PSRAM difference393252B is close to the fixed393216B journal
allocation. It is not a continuous allocator low-water measurement. The small
latency/count differences are not statistically established performance gains;
wireless variation, instrumentation and memory layout may contribute. The
journal is an explicit measurement cost, not production resource optimization.

The host has7 passing integrity/accounting tests. Both ON/OFF firmware builds
passed. The updated harness automatically runs terminal-state accounting for
future audited trials before marking them passed; r3 was independently checked
with the same validator and its accounting.json is retained. The two integration
failures are excluded and preserved. The original full flash was restored after
every attempted device trial.

Remaining P0 work: source/server clock offset and drift uncertainty, repeated
instrumentation-cost characterization, and a compact/bounded-drain design if
extending beyond the present60s observation window. Do not enable the planned
180s experiments assuming this384KiB journal will suffice.

Final restore check passed: five runtime setting ACKs and8 actual AI samples
on CPU0, with no NVS save. The board is back on the original FOMO firmware;
the experimental UDP14568 receiver was closed.

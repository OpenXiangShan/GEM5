# MainPipe Prefetch MSHR Eligibility: Test Report

## Summary

The RTL-ID candidate builds successfully and passes NEMU difftest on all 38 matched checkpoints at a 5-million-instruction window. Targeted MSHR-count traces confirm that it does not allocate ordinary prefetches to IDs 0 or 1. A repeated `mcf_6388` run reproduces all simulated statistics.

The average IPC change is small and mixed across workloads. The candidate is an entry-eligibility experiment, not a claim of full RTL behavior or a universal performance improvement.

## Tested versions

| Item | Revision or value |
|---|---|
| Parent PR | [#1351](https://github.com/OpenXiangShan/GEM5/pull/1351), base commit `7d538a7c4630fc62fe9dac72edbba28db9dcce5e` |
| Candidate source | Parent commit plus the four MSHR/BaseCache source changes in this PR |
| Candidate binary SHA-256 | `9f585c900fb6ef599a95a52b3ac48f3af09b807be23bcebeb59a5205ec4286d4` |
| RTL reference | XiangShan commit `4f6da52143b64b7a5a5419d309c79c4bf5e3794e` |
| RTL binary SHA-256 | `ef9238c5152df09a074013b0041ec16a5e0737b600e6ac22582b704d70a7417f` |
| RTL MSHR eligibility | 16 entries, `nMaxPrefetchEntry0=14`, ordinary prefetch IDs 2–15 |

The 38 workload runs use matching checkpoints and a 5M-instruction window with no warmup. GEM5 runs enable NEMU difftest. No Python configuration changes were needed.

## Build and functional checks

- RISC-V optimized GEM5 build completed successfully.
- All 38 candidate runs completed with return code 0 and no NEMU mismatch. Committed instruction counts are 5,000,000–5,000,007.
- Four 50K-instruction `legacy-xalancbmk` traces passed NEMU difftest and were inspected with MSHR/HWPrefetch debug output.
- An independent 5M-instruction `mcf_6388` rerun matched all simulated statistics. Five host-only measurements (`hostSeconds`, `hostTickRate`, `hostMemory`, `hostInstRate`, and `hostOpRate`) differed as expected with host runtime conditions.

### MSHR ID traces

| DCache MSHRs | Eligible prefetch IDs | Allocations observed | S2 prefetch rejections | Result |
|---:|---|---:|---:|---|
| 16 | 2–15 | 1,055; IDs 2–15 observed | Not counted in this MSHR-only trace | Pass |
| 4 | 2–3 | 398; IDs 2 and 3 observed | Not counted in this MSHR-only trace | Pass |
| 3 | 2 | 207; only ID 2 observed | 998 | Pass |
| 2 | None | 0 | 1,208 | Pass |

The 3- and 2-entry traces expose a known limitation: when no eligible ID is available, the candidate rejects a prefetch after dequeuing it from the PFQ. It does not requeue or retry the packet. The rejected request is speculative and did not cause an architectural mismatch in these tests, but request-retention/backpressure behavior remains different from the RTL handshake design.

## 38-checkpoint IPC comparison

| Version | IPC geometric mean | Versus current PR |
|---|---:|---:|
| Original GEM5 | 2.871787 | +0.397% |
| Current PR #1351 | 2.860429 | Baseline |
| RTL-ID candidate | 2.866213 | +0.202% |
| Historical RTL reference | 2.948847 | +3.092% |

The candidate is 0.194% below original GEM5 and about 2.80% below the RTL reference. Against the current PR, 16 checkpoints improve, 17 regress, and 5 are unchanged. Largest gains are `legacy-xalancbmk` (+3.729%), `legacy-milc` (+3.234%), and `xalancbmk_9874` (+2.283%). Largest regressions are `wrf_112496` (−2.164%), `cactusADM_61235` (−2.061%), and `mcf_6388` (−0.850%).

Across the 38 candidate runs, GEM5 reports 1,415,426 prefetch MSHR allocations and 1,241,415 useful prefetches, 4,734,037 ordinary `ReadReq` MSHR misses, and 2,618,837 cycles blocked for lack of MSHRs. The candidate's mean absolute RTL IPC error is 6.661%, compared with 6.806% for the current PR; this is a modest reduction, not full performance alignment.

## Interpretation

The results support the correctness of the tested entry-ID selection and show a small positive geometric-mean IPC change relative to the current PR. Per-workload results are mixed, and pressure traces reveal that a rejected packet is not retained for retry. Further RTL alignment should first establish and implement the request-ready/hold/replay behavior for unavailable MSHRs, then repeat the matched performance suite.

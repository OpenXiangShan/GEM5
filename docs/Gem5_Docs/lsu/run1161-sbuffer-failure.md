# run1161 SBuffer failure analysis

## CI evidence

Run: `spec06-rva23-novec-gcc16-1.0c/20260916_230110_abb479dd7_kmhv3_run1161`,
branch `cactusADM-align-0916`, commit `abb479dd70`.

The run contains 1,094 slices: 177 marked `completed` and 917 marked `abort`.
All 917 failures have exit status 139 and the same complete GEM5 backtrace
(16 executable frames after removing relocated addresses). They form one
observed failure class, across 55 workload inputs, including all 15 cactusADM
slices. No additional assertion, fatal, or difftest failure class was found.

Symbolizing the CI executable with `addr2line -Cfipe` gives:

| Offset | Function |
| --- | --- |
| `0x13fdcd8` | `LSQ::StoreBuffer::update(int)` |
| `0x1413157` | `LSQUnit::insertStoreBuffer(...)` |
| `0x1250977` | `LSQUnit::offloadToStoreBuffer(...)` |
| `0x1246b5c` | `LSQ::processWriteback()` |

CI artifacts are under
`/nfs/home/share/gem5_ci/performance_data/spec06-rva23-novec-gcc16-1.0c/20260916_230110_abb479dd7_kmhv3_run1161`.
The corresponding executable is
`/nfs/home/cirunner/actions-runner-gem5/node028-2/_work/GEM5/GEM5/build/RISCV/gem5.fast`.

## Cause and fix

Removing the global SQ offload gate in `abb479dd70` exposed an incomplete
`StoreBufferEntry::evictionInProgress()` predicate:

1. `getEvict()` removes the selected entry from `lru_index`.
2. The eviction path builds its `SbufferRequest`, including the byte-enable
   mask and packet referencing the entry's data.
3. S0 admission can fail due to MainPipe contention. The entry is retained in
   `blockedSbufferEntry`, but `sending`, `inDcacheMainPipe`, and `replayQueued`
   are all false.
4. SQ offload now proceeds independently. A younger same-line store sees the
   false predicate and incorrectly chooses the unsent-entry merge path.
5. `StoreBuffer::update()` searches for the removed LRU node. The assertion
   fires in `gem5.opt`; `gem5.fast` continues to an invalid erase and crashes.

Treat an existing eviction `request` as in-progress ownership too. Younger
same-line stores then allocate or merge into the existing vice entry, using
the normal capacity checks. The original packet's data and mask remain
stable while it waits for admission. Independent entries can still enqueue;
the fix does not restore the global input gate or change eviction arbitration,
MainPipe stages, MSHR merging, or fence/drain rules.

Checking only for an LRU lookup failure would hide the crash while still
allowing the payload of a prepared request to change. The ownership check
fixes both the invalid LRU operation and that data-lifetime hazard.

## Representative regression

Representative of the single observed class: `bzip2_chicken/5787`, checkpoint
`/nfs/home/share/checkpoints_profiles/spec06_gcc16_rva23_novec_260820/checkpoint/bzip2_chicken/5787/_5787_0.030610_memory_.zstd`.

The unmodified `abb479dd70` opt binary reproduces the LRU-membership assertion
at tick `6647013`, exiting with signal 6. Its log is
`/tmp/run1161-regression/before/bzip2_chicken_5787/sim.log`.

Regression configuration matches CI: `kmhv3.py`, memory dedup enabled,
20M warmup instructions, 40M total instructions, NEMU difftest enabled with
`/nfs/home/share/gem5_ci/ref/releases/d30fff1ece9e-gem5-r3/normal-dedup/riscv64-nemu-interpreter-so`.
The local opt build keeps assertions enabled in addition to difftest.
Commands and output are recorded under
`/tmp/run1161-regression/after/bzip2_chicken_5787/`.

A 100k-instruction run with a narrow `StoreBuffer` trace window also passes
NEMU difftest. At the exact original failure tick `6647013`, line `0x883bb280`
is blocked before MainPipe admission; the younger store to `0x883bb2ac` now
creates a vice entry successfully. In the same tick, an unrelated store to
`0x88bbaba4` also enqueues successfully. The original line enters MainPipe at
tick `6647346`. This confirms both preservation of the prepared request and
continued independent SQ enqueue under output backpressure. Evidence:
`/tmp/run1161-regression/trace/bzip2_chicken_5787/store.trace`, lines 1424–1429.

The three existing directed cases all pass data self-check and NEMU difftest
after the fix (`/tmp/run1161-regression/directed/results.json`):

| Case | Same-line MSHR merges | Miss forwarding queries | Same-line replays |
| --- | ---: | ---: | ---: |
| Normal merge | 45 | 31 | 0 |
| Two-target pressure | 16 | 30 | 15 |
| Legacy release | 0 | 0 | 0 |

The earlier sparse directed workload did not catch the S0-blocked same-line
enqueue window. The SPEC reproducer and the trace above explicitly cover it.

The full representative slice completes successfully with exit code 0 and
`because a thread reached the max instruction count`, at tick `7669562760`.
NEMU difftest and opt assertions remain enabled throughout:

| Statistic | Warmup | Measured |
| --- | ---: | ---: |
| Instructions | 20,000,003 | 20,000,001 |
| CPU cycles | 4,819,436 | 18,212,285 |
| Same-line MSHR merges | 20,145 | 0 |
| Vice entries created | 36,218 | 487 |

The measured segment took longer because it executed substantially more
cycles than warmup. A redundant progress-reporting diagnostic run was stopped
after the full regression completed; it is not counted as another test result.

Build (`scons build/RISCV/gem5.opt --gold-linker -j32`), repository style check
for `src/cpu/o3/lsq.hh`, and `git diff --check` pass. Full CI failure inventory:
`/tmp/run1161-regression/inventory.json`. The 917 failed slices have not all
been rerun; the result above validates one representative of their common
failure class, not a completed full-suite rerun or a new performance score.

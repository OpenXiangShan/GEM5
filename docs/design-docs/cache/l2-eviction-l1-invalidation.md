# L2 victim invalidation experiment

Enable with `configs/example/kmhv3.py --l2-evict-invalidate-l1`. The option is
also passed through to each classic cache inside the aligned L2 slices.
It defaults to off so the same binary can run baseline and experiment.
The experiment uses timing mode.

## Behavior

For every L2 eviction packet, first query upper-level presence using the
existing `isCachedAbove()` path. If no upper copy is reported, put the victim
in the write buffer with a ready time of the next L2 clock edge plus one
cycle. The presence query's annotated delay is intentionally not charged
in this case.

If a copy is reported, send an express `ReadExReq` upward. This invalidates
clean copies immediately. For dirty copies, the existing coherence path
creates a data response, including its support for write-buffer hits and
in-flight MSHRs. An L2 write-buffer slot is reserved until that response
arrives; its initial ready time is `MaxTick`. The response replaces the
victim's old data and upgrades the outgoing command to `WritebackDirty`.
L2 never forwards the victim's stale payload while awaiting dirty data.

The snoop carries a dedicated marker. Only responses to these victim
invalidations add the responding cache's `data_latency`. Other snoops keep
their existing timing. The simplified latency is:

- No upper copy: one L2 cycle, plus clock alignment.
- Clean upper copy: XBar forward/filter/alignment plus upper tag lookup.
- Dirty upper copy: the clean probe costs plus upper data access. The
  writeback additionally waits until the complete data response is available,
  including return-link header/payload delay and arbitration or MSHR waits.

The buffer ready time is the maximum of the annotated probe deadline and
full-data availability. No additional one-cycle delay is appended after a
snoop hit. Same-address misses cannot use the existing writeback ordering
shortcut to send a snoop-dependent victim before its ready time. When such
a miss is blocked, the response event wakes the sender instead of polling
at the same tick.

This is a latency experiment. Tag lookup and clean invalidation still happen
synchronously. It does not add L1 bank/port/mainpipe arbitration or replace
the classic protocol with a TileLink Probe/ProbeAck implementation. The
refill pipeline preceding victim generation remains in place.

## Observability

Each cache exposes `evictionSnoop` counters (active on enabled L2s):

- `absent`, `clean`, `dirty`, `dirtyResponses`: path counts.
- `absentDelayTicks`, `cleanDelayTicks`, `dirtyProbeDelayTicks`: summed
  readiness/probe delays measured from victim generation.
- `dirtyDataReturnTicks`: summed time until full dirty data returns.

A stats reset can separate a dirty request from its response, so their
counts need not match inside a warmup/measurement boundary. `CacheRepl`
traces include `EvictL1Absent`, `EvictL1Probe`, and `EvictL1Data` records with
addresses and ready times.

## Validation

`tests/gem5/cache_eviction/l2_invalidate.py` uses MemTest's independent data
checker, a small L2, and larger L1s to exercise clean and dirty invalidations.
Reverse the cache sizes to exercise absent upper copies. Vary the L1 data
latency and write-buffer depth to check timing sensitivity and progress.

Checkpoint regression uses the same executable, configuration, reference
model, checkpoint and instruction counts with the option disabled/enabled.
Normal single-core checkpoints use their embedded restorer. Results must
show instruction-limit completion with difftest enabled, and nonzero
invalidation/data-response counters on the experiment runs.

## Results: 2026-11-09

Implementation and validation are isolated on branch
`codex/l2-eviction-l1-invalidate`, based on `9208445e5d`. The original checkout's
uncommitted work was not copied into this worktree.

The optimized build and the repository's modified-region style checks passed.
Ten MemTest regressions passed: baseline, predominantly clean reads, mixed
reads/writes, functional accesses, longer data latency, clean-evict packets,
fast/slow timing traces, mostly absent upper copies, and four upper caches
with a one-entry write buffer. MemTest's command RNG samples an inclusive
range, so `percent_reads=100` still produces rare writes; the clean case
checks predominance rather than assuming there are no dirty lines.

The one-GHz timing traces verified these ready times from victim generation:

| Path | Upper data latency | Probe deadline | Full-data/writeback ready |
|---|---:|---:|---:|
| No upper copy | any | n/a | 1 cycle |
| Clean copy | any | 4 cycles | 4 cycles |
| Dirty copy | 2 cycles | 6 cycles | at least 8 cycles |
| Dirty copy | 6 cycles | 10 cycles | at least 12 cycles |

The two dirty trace cases checked 3,876 and 3,864 completed stable responses,
respectively. The mixed test checked 54,919 dirty responses; the constrained
write-buffer test checked 364,846. All used MemTest's independent reference
data checks. In the absent-copy test, all 109,202 absent victims had exactly
one cycle of ready delay.

Three SPEC06 checkpoint pairs completed with difftest enabled, 2M warmup
instructions and 3M measurement instructions each. Both variants used the
same binary, reference, checkpoint and generated configuration; the only
configuration differences were the four inner L2 cache experiment flags.
The instruction-limit/reset boundary differs by at most seven instructions
from the nominal measurement count, within the core's commit granularity.

| Checkpoint | Baseline / experiment difftest | Absent victims | Clean invalidations | Dirty responses | Measured cycle change |
|---|---|---:|---:|---:|---:|
| mcf / 10438 | PASS / PASS | 684,907 | 196 | 15 | +1.244% |
| libquantum / 85933 | PASS / PASS | 76,577 | 20 | 0 | -0.002% |
| bzip2_html / 13707 | PASS / PASS | 3,080 | 40 | 9 | -0.086% |

The default KMHv3 upper caches produced five-cycle dirty probe deadlines and
seven-cycle full-data readiness for the 24 dirty measurement responses.
Every absent-copy measurement had exactly one L2 cycle of ready delay.
Libquantum did not exercise dirty recovery in this measurement window.

Checkpoint files came from
`/nfs/home/share/gem5_ci/spec06_cpts/h_spec06/checkpoint-0-0-0`, using the
normal reference at
`/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so` and embedded
restorers. The independent build did not include DRAMsim3, so these matched
correctness runs explicitly used `SimpleMemory`. The cycle changes are
small-sample diagnostics, not a SPEC score or a DRAMsim3 performance result.
Multi-core private-L2 coherence and detailed L1 port/bank arbitration were
not validated by these single-core checkpoints.

Artifacts in this worktree:

- `m5out/l2-eviction-regression-v2/results.json`: ten regression outcomes,
  counters and dirty timing sample counts.
- `m5out/l2-eviction-checkpoints-v1/verification.json`: configuration,
  instruction-count, delay and executable/reference hash checks.
- Each checkpoint directory contains `command.json`, `config.json`,
  `stats.txt`, `run.log`, `status.json` and its process ID.
- `manifest.json` records binary, reference and checkpoint SHA256 hashes;
  `implementation.patch` records the implementation changes.

Reproduce the local memory tests:

```sh
scons build/RISCV/gem5.opt --gold-linker -j64
python3 tests/gem5/cache_eviction/run_regression.py \
  --binary=build/RISCV/gem5.opt \
  --output=m5out/l2-eviction-regression-new --loads=50000
```

For checkpoint regression, use
`tests/gem5/cache_eviction/run_checkpoints.py --binary=build/RISCV/gem5.opt`
with a new `--output` directory, `--ref-so`, and repeated `--checkpoint`
arguments. Defaults reproduce the 2M/3M instruction windows; saved
`command.json` files contain every exact invocation used above.

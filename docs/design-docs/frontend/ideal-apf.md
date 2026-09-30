# Ideal-resource alternate-path fetching

## Scope and configuration

This is a single-thread, instruction-driven, scalar APF model. It generates real
decoded instructions before a redirect and replays them at the Rename input.
It is not a PC-only recovery shortcut or a cycle-accurate implementation of the
paper's pipeline. Existing H2P classification, confidence and allocation filters
are unchanged. `enableAPF` defaults to false; the existing metadata-only H2P
buffer remains an independent classification/admission experiment.

Enable with `--param='system.cpu[0].enableAPF=True'` on `kmhv3.py`.

| CPU parameter | Default | Meaning |
| --- | ---: | --- |
| `enableAPF` | false | Generate and replay alternate paths |
| `apfBufferEntries` | 4 | Saved paths, excluding the one active slot |
| `apfBufferUops` | 104 | Maximum decoded uops per path |
| `apfWidth` | 8 | Maximum uops generated per generation cycle |
| `apfGenerationCycles` | 13 | Maximum generation steps per path |
| `apfFetchLatency` | 1 | Cycles before each ideal read/generation step |
| `apfBranchEntries` | 20 | Maximum control-flow records per path |

At the defaults generation starts one cycle after selection and has at most 13
generation steps. A step can stop at a taken branch or the predictor's aligned
block boundary, so 104 uops is a capacity, not a promised fill amount. Increasing
read latency spaces steps further apart. Full saved buffers retain a completed
active path and prevent starting a new path, without stalling the main frontend.

## Predictor and instruction state

`DecoupledBPUWithBTB` saves/restores synchronous query contexts around shadow work.
The context contains thread prediction state, folded histories, RAS storage,
metadata handles and component query scratch. Trained tables are shared and are
not restored from these snapshots. Normal training between generation steps is
therefore retained. The source FTQ checkpoint is recovered with the opposite
direction; current younger main-path history is not used as the starting history.

The ideal query uses MBTB, TAGE, SC and RAS final-stage results. It does not run
the early-stage override pipeline, teacher updates or a second PairTAGE block.
No-side-effect lookup/metadata APIs avoid replacement and bank-conflict changes.
Shadow query counts live under `cpu.apf`; shadow RAS operations do not increment
the main RAS access counters. Query metadata is independently allocated. RAS
ring-position metadata is rebound to the live RAS when a prefix is promoted,
preserving intervening committed-stack updates.

An independent decoder reads only the bytes of the current instruction through
functional translation and functional I-cache reads. Translation checks and
normal-memory checks still apply; there are no timing accesses, misses, port
conflicts, backend allocations or architectural faults from generation. Two-byte
reads handle compressed instructions and page-split 32-bit instructions. Unsafe
translation, unknown/vector/system instructions and non-return indirect control
stop generation. Data accesses are not executed. Calls and returns use the
shadow RAS. Macro-ops are expanded, but a macro-op that cannot fit wholly in the
current bounded packet is removed and terminates the prefix.

Generation bandwidth does not split predictor blocks. A block keeps its original
query and metadata while its uops are decoded over multiple generation cycles;
only a taken branch or the predictor block boundary starts a new query. Each
extension rebuilds the endpoint history/RAS from the block's starting context
and the cached prefix, so early recovery neither duplicates history updates nor
includes branches beyond the saved instructions. A capacity/time/unsupported
instruction stop can seal a partial block into one FTQ record.

Each record retains only the branches and history updates in its cached prefix.
H2P branches on it are recorded, not recursively followed. Once promoted, they
can become ordinary main-path APF sources. Main Decode's optional load fusion and
constant folding are not supported with APF. Replayed instructions also bypass
the ordinary ALU fusion pass; uop counts represent unfused decoded instructions.

## Ownership, recovery and ordering

The oldest eligible source in the FTQ starts first; an active path is never
preempted. Correct execution resolution releases its path immediately, independent
of predictor-training queue availability. A retired source FTQ also releases any
predicted-but-never-executed candidate. Ordinary backend direction squashes and
frontend static-target redirects both use the common recovery boundary.

Recovery requires matching FTQ identity, branch PC, epoch, and **alternate start
PC**, not the cached end PC. Empty/mismatched paths fall back to normal fetch.
An active partial prefix can be selected. The ordinary predictor squash runs
once before promotion. Promotion inserts saved prediction records into available
FTQ space, marks them fetched and advances live histories using the saved
outcomes. It never re-queries for a new direction. When FTQ space runs out, already
promoted packets can replay while later packets wait, allowing retirement to
make progress even when the FTQ is smaller than a complete path.

New `DynInst`s are created in prefix order only on promotion, before normal fetch
can resume. Rename admits at most `renameWidth` uops per cycle into its existing
bounded input FIFO and retains all RAT/ROB recovery and backend backpressure.
Decode cannot pass the pending prefix. Replay checks Rename's squash version,
including version wraparound; Rename immediately truncates queued replay on a
squash because its redirect arrives earlier than Fetch's. Subsequent branch
misses in replayed instructions use ordinary squash and training paths.

Replay owns promoted uops independently of the APF storage. Selection transfers
the path out of its active/saved slot; promotion releases it when its records have
been installed. Exceptions, thread reset, address-space flushes and serializing
fence/CSR commits invalidate speculative paths. Promoted uops are removed through
normal squash/RAT recovery, not silently dropped while leaving an advanced PC.
Invalidation requests an additional redirect only when a recovery is pending or
uncommitted promoted uops exist (including uops already delivered to Rename/ROB).
Discarding only shadow paths does not add a main-path squash.

## Statistics

The old `h2pTable*`, `h2p*` and metadata `h2pBuffer*` precision statistics retain
their original meaning. `commitH2PBufferCandidate()` already has a caller in
`commitBranch()`; no duplicate commit call was added.

New `system.cpu.apf.*` counters include:

- `candidates`, `started`, `buffered`: selected, started and saved paths.
- `generatedUops`, `replayedUops`, `committedUops`: distinct production, Rename
  admission and final-commit counts. Squashed replay is not committed work.
- `recovered`, `recoveredCommitted`, `partialRecoveries`: actual prefix handoff,
  handoffs whose source commits, and handoffs of unfinished active paths.
- `recoveryCoverage`: `recoveredCommitted / committedConditionalMisses`.
- `replayCommitRatio`: `committedUops / replayedUops`.
- `fullCycles`, `ftqWaitCycles`, `replayBlockedCycles`: saved-buffer pressure,
  FTQ pressure and cycles with undelivered replay at Rename.
- `replayBlockedWithRecoveryCycles`: the subset of Rename replay-blocked cycles
  with incomplete promotion. FTQ waiting and replay blocking are sampled
  independently and may overlap; newly promoted uops are not counted as blocked
  before Rename has had an opportunity to consume them.
- `activeOccupancyRatio`, `savedOccupancy`, `meanSavedOccupancy`,
  `bufferIdleRatio`: active and saved-path occupancy, separately.
- `stops::*`, `fallbacks::*`, `discarded`, `retiredPaths`: termination, fallback,
  correct-resolution and source-retirement outcomes.
- `generatedMicroops`, `compressedUops`, `crossPageReads`: validation coverage.

Counters follow normal gem5 reset/dump boundaries; production before a reset and
commit after it can cross measurement intervals. Ratios are most directly
interpretable over complete runs. No counter controls progress. Work per tick is
bounded by FTQ size for candidate selection, configured uop/branch limits for
generation and replay, and four plus one path slots for storage management.

## Validation

`util/apf/run_smoke.py` builds a bare-metal program and requires a difftest
reference. It checks APF off/on, eight-entry FTQ backpressure, slower generation,
atomic macro-ops and serializing fences. The program has a difficult branch,
compressed instructions, calls/returns and a 32-bit instruction crossing a 4 KiB
boundary. It must reach its exit, recover nonzero paths and commit replayed uops.
The `blocks` case checks an APF trace for one query per multi-cycle block. The
`fence-off`/`fence-empty` pair disables H2P and requires identical cycle, IPC and
squash counts with APF off/on. `ftq` also requires replay blocking while promotion
is incomplete. `--debug-flags=APF` traces block queries and prefix extensions.

```sh
python3 util/apf/run_smoke.py \
  --ref=/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so \
  --out=/tmp/apf-smoke-validation
```

Targeted TAGE, MGSC, RAS, H2P table and metadata-buffer unit tests cover context
restoration, retained table training and non-mutating H2P lookup. Checkpoint
validation must use the matching reference library and memory size. In particular,
the gcc16 RVA23 checkpoints require the CI pinned deduplicating reference with
`--enable-mem-dedup`, not the older generic reference used by the smoke test.

Short checkpoint passes establish local functional evidence, not full SPEC int
performance results. Full 0.3c CI, more address-space/instruction-consistency
stress, and exact pipeline/port contention remain separate validation/modeling
work. APF is deliberately opt-in until that work is complete.

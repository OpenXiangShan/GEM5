# RAT bandwidth and checkpoint partitioning

The O3 model retains a complete speculative rename map (SRAT), committed
rename map (ARAT), and rename history for every configured hardware thread.
Each thread needs its full architectural register namespace for correctness.
The timing resource is a fixed, tid-owned bank of read/write access lanes.

`sratWidth` and `aratWidth` describe the **whole-core** access budget, in
instruction lanes per cycle. A lane carries the source lookup and
old/new destination-map access bundle of one instruction. Each configured
thread receives `width / numThreads` lanes; an idle, stalled or sleeping
sibling does not donate its bank. Positive widths must divide evenly by the
configured hardware-thread count.

This is a pipeline-lane abstraction of bank read/write bandwidth. It does not
simulate an individual port per operand or address-dependent bank conflicts.
Functional/debug/difftest map reads and initialization are not timing requests.
Existing history-buffer/ROB squash recovery continues to model restore cost;
the bank quota constrains normal rename and retirement, not a new recovery
port arbitration model. No map payload is truncated or aliased across tids.

The single-thread ideal KMHV3 configuration retains both whole-core budgets
at 8. The dual frontend overrides only its SMT configuration to whole-core
`sratWidth = 10` and `aratWidth = 10`: each thread owns five instruction access
lanes, matching its five-wide decode/rename path. Each SMT ARAT bank also
admits at most five instructions per cycle, regardless of how many logical
instructions fit in a compressed ROB group. Single-thread/Hybrid retirement
keeps its existing group/entry behavior. The SRAT physical-register availability
check uses only the prefix and worst-case reservation that its bank can admit.

`numMaxRatSnapshot` is now a **whole-core** NaiveCpt budget. The default 4 gives
one thread 4 slots or two threads 2 slots each. Both quotas use configured
`numThreads`, not the runnable-thread count. Enabled checkpoint budgets must
be divisible by `numThreads`; zero disables checkpoint creation. When a bank
has no checkpoint slot, rename proceeds without creating a new checkpoint.
Commit and squash release only the owning thread's records. Checkpoints remain
sequence-number/instruction-mark records used to estimate recovery cost;
this change does not store copies of the RAT contents.

Validation statistics:

- `rename.sratBankInstsPerCycle_<tid>`: bank admissions at each rename visit.
- `rename.sratBankLimitCycles`: visits reaching the SRAT bank limit with more
  instructions buffered.
- `commit.aratBankInstsPerCycle_<tid>` and `aratBankLimitCycles`: SMT retirement
  admissions and cycles reaching the ARAT bank limit with ROB work remaining.
- `rename.assignedRatSnapshotByThread`: checkpoint allocations by owner.
- `rename.ratSnapshotsInUseByThread_<tid>` and `ratSnapshotsInUse`: per-thread
  and whole-core occupancy, sampled after checkpoint allocation.

Assertions check per-thread admissions, checkpoint ownership counts and the
whole-core checkpoint limit. The A+A regression uses the repository catalog's
`gcc12-spec06-smt-0.3c` list and dual-context checkpoints, with multi-hart NEMU
difftest enabled. Logs, exact commands and a machine-readable result summary
are saved under `work/smt_ratx1_snapshotx4_20261104/`.

The later SMT-only five-lane configuration checks are saved separately under
`work/smt_rat_5ports_20261104/`; the earlier regression directory records the
initial four-lane-per-thread setup.

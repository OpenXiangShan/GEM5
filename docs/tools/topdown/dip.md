# Dispatch-time instruction profiling (DIP)

DIP complements TEA's commit-side cycle stacks with dispatch-side stacks. This
implementation follows the per-slot attribution policy in Figure 3 of
*Chips Need DIP: Time-Proportional Per-Instruction Cycle Stacks at Dispatch*
(ASPLOS 2026, DOI 10.1145/3779212.3790139).

DIP and TEA are enabled by default with the single-thread XiangShan
configuration:

```sh
build/RISCV/gem5.opt configs/example/kmhv3.py \
    --generic-rv-cpt=<checkpoint.zstd>
```

Difftest is enabled by default. Configure its reference model with
`--difftest-ref-so=<reference.so>` or `GCBV_REF_SO`; use `--disable-difftest`
when running without a reference model.

DIP requires the classic memory system. With Ruby, pass both `--disable-dip`
and `--disable-tea`, because Ruby does not supply the cache-depth metadata used
for frontend events. Pass `--disable-dip` for SMT, which DIP does not support.

Use `--disable-dip` to turn DIP off; `--enable-dip` explicitly turns it on.
`--dip-sample-period=N` samples every N active CPU ticks;
zero and one enumerate every tick. TEA and DIP can run together. DIP also
preserves fetch cache-line miss metadata when TEA is disabled.

The ingress boundary is in-order IEW admission: directly to the scheduler/LSQ
when `enableDispatchStage=False`, or into the dispatch queues when it is true.
Out-of-order transfers from dispatch queues to issue queues are not counted as
new ingress. The number of slots is `renameWidth`. The current implementation
supports one thread; enabling it for SMT is rejected because shared dispatch
slots need a separate attribution policy.

Each accepted uop tentatively receives one Base slot. Unused slots blocked by
an instruction belong to the oldest blocked instruction, with the actual
admission gate recorded as the cause. Upstream ROB and queue reservations and
physical-register exhaustion retain the blocked rename instruction's identity.
Empty frontend slots wait for the next admitted uop, whose supplying instruction
cache/TLB events distinguish a miss from a bubble.

These targets remain tentative until retirement establishes the correct path.
An accepted squash transfers younger targets and waiting frontend slots to its
cause as Misspeculation. The last redirect remains active during pipeline
refill. Instruction squash versions distinguish late old-path admission from
new-path instructions. New-path input blocked by an observed upstream rename
gate ends refill as well: its empty slots are Stall. Recovery state advances
every tick, including ticks between sampling boundaries.
Self-including replays and nested redirects keep the cause tentative until
in-order retirement passes its sequence number.

A fused uop consumes one slot and is represented by its first architectural
member's PC; splitting a slot between fused members would change the paper's
uop attribution policy. A simulator-side redirect without an identifiable
instruction has an empty PC and remains explicitly unresolved.

`system.cpu.dip.csv` contains:

| Field | Meaning |
| --- | --- |
| `thread` | Thread ID, currently zero |
| `pc` | Static target PC, empty when no target is known |
| `state` | Base, Stall, FrontEnd, or Misspeculation |
| `cause` | Multi-hot cause mask below |
| `slot_weight` | Integer number of sampled slots |
| `cycle_weight` | `slot_weight / renameWidth` |
| `resolution` | resolved, unresolved, or pending |
| `flush_reason` | Numeric `SquashCause` of the redirect owning Misspeculation slots |

Pending rows are provisional: their target may still be squashed, or the next
frontend target has not arrived. They must not be treated as final per-PC
attribution. A stats reset drops the old window's weights and targets while
retaining live redirect state. Dumping stats does not resolve or consume them.
With `--warmup-insts-no-switch=N`, DIP does not accumulate sampled slots or
write a CSV during warmup. Collection starts after the warmup-ending stats
reset, while live redirect/path tracking continues across the boundary. Without
a configured warmup, collection starts immediately. The sampling period starts
at the first measured CPU tick.

Each new-tick stats dump overwrites the CSV with the current stats-window
snapshot. After warmup and measurement, pair the final CSV with the latest
block in `stats.txt`; that file also retains the other components' warmup
statistics, with zero DIP samples. A later dump replaces the previous CSV,
so save a copy first if an earlier measurement snapshot is needed.

`system.cpu.dip.sampledSlots` equals samples times dispatch width. At every dump,
`attributedSlots + unresolvedSlots + pendingSlots` equals `sampledSlots`.
`slotsByState` and `slotsByCause` describe finalized slots; cause bits can overlap.
Use pending totals explicitly when constructing a complete cycle stack.

At period one, the sum of CSV cycle weights equals active CPU ticks in the
measurement window. At larger periods, weights are in sampled-cycle units;
multiply by the period for an estimate, retaining sampling phase/error.
DIP measures uop ingress and TEA measures architectural egress. Their Base and
Computing categories are different, particularly with fusion and ROB compression.
The two cycle stacks must be compared separately and must not be added together.

This is simulator telemetry with dynamic target maps. It does not model sampling
interrupts or claim the paper's BOOM hardware area, storage, or runtime overhead.

| Bit | Cause | Meaning |
| --- | --- | --- |
| 0 | S_ROB | ROB allocation is blocked |
| 1 | S_ISB | Issue/dispatch queue capacity or admission bandwidth is blocked |
| 2 | S_LDQ | Load queue capacity/reservation is blocked |
| 3 | S_STQ | Store queue capacity/reservation is blocked |
| 4 | S_REN | Physical register rename is blocked |
| 5 | S_SER | A serializing instruction cannot enter |
| 6 | F_BUB | Frontend bubble without an instruction cache/TLB miss event |
| 7 | F_CTM | The next uop carries an instruction cache/TLB miss event |
| 8 | M_SWE | Software-handled instruction exception |
| 9 | M_FL | Branch, memory-order, value-prediction or other hardware flush |
| 10 | M_UNKNOWN | Simulator/interrupt redirect without an instruction cause |

Stall masks describe the gates that the simulator actually evaluates. Upstream
ROB/LSQ/register causes may overlap; at direct admission, earlier failing gates
can mask later predicates. They are not an exhaustive test of all hypothetical
resource constraints, and cause counts must not be summed as disjoint cycles.

## Automatic CSV and Markdown report

After a successful XiangShan simulation, normal exit cleanup and the final stats
dump complete before `python3` generates CSV tables and a Markdown report.
This automatic step uses only the Python standard library; it does not plot,
extract ELF files or require Matplotlib/objdump.

Read `<outdir>/profile/report.md` for cycle/instruction counts, CPI, DIP state
shares and TEA state/event shares. Each CPU has its own directory:

```text
m5out/profile/
  report.md
  system.cpu/dip/
    report.md
    summary.csv
    pcs.csv
    contexts.csv
```

The report uses the final stats window, after the configured warmup reset.
If the run ends during warmup, it says no measured DIP profile was collected;
any TEA samples shown still describe that final warmup window. Disabled
profilers are marked disabled. A nonzero simulated exit skips reporting.
`--disable-profile-report` skips automatic postprocessing without changing
collection; disabling both profilers also skips it. Reporting errors warn
without changing the simulation result.

Regenerate the numerical report without running gem5 again:

```sh
python3 util/profile_report.py m5out
```

Use `-o <directory>` to select a different report directory. The ordinary
`stats.txt` and `config.ini` outputs must be available alongside the raw CSVs.

## Manual image report and PC instruction labels

Generate images only when requested:

```sh
python3 -m pip install matplotlib
python3 util/profile_report.py m5out --plots
```

This adds DIP state/cause and PC charts under `profile/<cpu>/dip/`, TEA
state/event and PC charts at `profile/<cpu>/tea.png` and `tea-pcs.png`, and
image links to `profile/report.md`. Pending or unattributed PCs do not enter
hotspot charts. TEA PC weights include all states; they are not stall costs.
A snapshot with no finalized PC attribution has no PC chart.

PC charts show the address, instruction and function when matching ELF files
and a RISC-V objdump are available. For current `manual-perf` SPEC profiles,
the tool derives the profile from `config.ini`'s `system.gcpt_file`, reads
`manifest/<workload>.json` and `cmd/<workload>.run.sh`, and finds the commanded
application in its rootfs. A profile `elf/*.elf` is reused only when its bytes
match that executable. Otherwise the actual ELF is extracted to `profile/elf/`.
The manifest's kernel ELF is also included. Shared CI assets are only read.
This avoids mislabeling PCs when a similarly named ELF belongs to a launcher.

Outside those profile layouts, supply the workload ELF explicitly:

```sh
python3 util/profile_report.py m5out --plots --elf /path/to/workload.elf
python3 util/plot_dip.py dip-report --view pcs --elf /path/to/workload.elf \
    -o dip-report/instructions.png
```

Repeat `--elf` for a kernel or another matching image. `--objdump <executable>`
selects a disassembler; by default the tools search for
`riscv64-unknown-linux-gnu-objdump`, then `riscv64-linux-gnu-objdump` on PATH.
Use a version that supports the workload's ISA. Debug information is not
required for instructions; function names require symbols. Supply the guest
program's ELF, not the gem5 executable. ELF virtual addresses must match the
profiled PCs; relocated images need their runtime address mapping first.
Unknown PCs remain explicitly unmapped rather than using a nearby instruction.

Without ELF assets or objdump, the manual report keeps PC-only charts and
records why instruction labels are unavailable. Plot failures retain the
numerical report. Re-running with `--plots` regenerates the images; an automatic
numerical run does not update existing image files.

## Offline processing and visualization

From the repository root, process a completed output directory:

```sh
python3 util/analyze_dip.py m5out -o dip-report
python3 util/plot_dip.py dip-report -o dip-report/overview.png
python3 util/plot_dip.py dip-report --view pcs -o dip-report/pcs.png
```

Processing uses only the Python standard library. Plotting requires Matplotlib
(`python3 -m pip install matplotlib`) and works on servers without a display.
Use `.png`, `.svg` or `.pdf` for the figure filename. All paths and labels are
caller supplied; these tools do not depend on a benchmark suite or a cluster.

The processor reads `system.cpu.dip.csv`, `config.ini` and the **latest** block
of `stats.txt` in the supplied directory. Alternatively, pass the CSV filename
with its config and stats alongside it. `--cpu=<object-name>` selects another
CPU object and its CSV. Keep all three files from the same dump/window. The
processor checks the slot/cycle conversion, resolution totals, every finalized
state/cause counter, and `sampledSlots = samples * renameWidth` before writing
results. A missing CSV after a run that ended during warmup means there was no
measurement snapshot to process. Wait for a running simulation to finish before
processing its output; its CSV and stats may still be changing.

The output directory contains:

| File | Content |
| --- | --- |
| `report.md` | Readable accounting summary, state shares, actual/estimated CPI and top PCs for each run |
| `summary.csv` | One row per input: source, width, sample period, samples, cycles, committed instructions, resolution totals, finalized state/cause slots, Pending slots, CPI and auxiliary counters |
| `pcs.csv` | Finalized **resolved** slots aggregated by PC and state, sorted by non-Base slots; includes total/overhead slots, share of all sampled slots and estimated CPI |
| `contexts.csv` | Slots aggregated by state, full cause mask, resolution, IQ ID/flags, frontend reason and flush reason; includes decoded cause/IQ/flush names and all three resolutions |

Re-running the command replaces these four derived files. Raw input files are
only read. PC addresses in `pcs.csv` refer to a single executable/address space;
resolve them against the matching workload ELF if function or source names are
needed. `contexts.csv` retains numeric `frontend_reason` values from
`StallReason` in `src/cpu/o3/comm.hh`.

Compare runs by supplying their directories and unique display labels:

```sh
python3 util/analyze_dip.py baseline/m5out candidate/m5out \
    --labels baseline candidate -o dip-compare
python3 util/plot_dip.py dip-compare -o dip-compare/shares.svg
python3 util/plot_dip.py dip-compare --unit cpi -o dip-compare/cpi.pdf
python3 util/plot_dip.py dip-compare --view pcs --run candidate --top 20 \
    -o dip-compare/candidate-pcs.png
```

The overview shows a complete stack (four finalized states plus Pending) and
an independent heatmap of finalized, possibly overlapping causes. `--view pcs`
shows the largest non-Base PC hotspots with their state stacks. Percentages use
**all sampled slots of the selected run** as their denominator, including on the
PC chart; the top PCs are not renormalized to 100%. For multiple runs, select one
with `--run` when plotting PCs. Labels default to the directory name, or its
parent name when the directory is named `m5out`; use `--labels` when they collide.

The CSV columns ending in `_slots` and `slot_weight` count **sampled slots**;
`contexts.csv`'s `sampled_cycles` divides this by `renameWidth`. The `cause` and
`iq_flags` columns remain full multi-hot masks. `Pending_slots` is a fifth stack
bucket: its provisional state and PC must not enter finalized state or hotspot
totals. `unresolved_slots` is already included in the four finalized states;
adding it again would double count. Auxiliary `decode_stranded_slots` and
`ready_branch_waits` stay outside the stack.

For any state, cause or PC with slot weight `s`:

```text
share_pct      = 100 * s / sampledSlots
sampled_cycles = s / renameWidth
estimated_cpi  = sampled_cycles * max(1, dipSamplePeriod) / committedInsts
actual_cpi     = numCycles / committedInsts
```

`--unit cpi` uses the estimated formula for every segment; at periods above one,
the sum retains sampling error and need not equal actual CPI. Default percentage
plots do not multiply by the period. Do not rescale samples to force agreement
with actual cycles. Compare equivalent measured windows/configurations when
interpreting performance changes; these tools validate snapshot accounting,
not experimental equivalence or workload correctness.

Each input remains a separate run. To combine SimPoints, first calculate each
point's state/cause CPI, then apply normalized point weights within an input and
full input instruction counts across inputs. Do not average state percentages
or pool raw slots from different runs; the commands above compare snapshots and
do not compute suite scores. TEA and DIP remain independent observation points.

## Admission context

CSV rows additionally retain `iq`, `iq_flags`, and `frontend_reason`.
Summing over these columns recovers the original PC/state/cause/resolution
budget. Pending weights retain their context; squashed weights follow the
original Misspeculation transfer and discard the failed-path gate identity.

`flush_reason` further partitions Misspeculation using the existing
`SquashCause` enum: 0 = None/unspecified, 1 = BranchMispredict,
2 = MemOrderViolation, 3 = ValuePrediction, 4 = Trap, 5 = ThreadContext,
6 = SquashAfter, and 7 = LongLatencyFlush. Other states use zero.
It describes the redirect source, including a Trap whose instruction fault
may request re-execution. It does not change the original cause mask.
Pending slots retain the reason; an older redirect transfers younger slots
to its own reason. Summing over this column recovers the previous CSV budget.

For a surviving S_ISB stall at direct scheduler admission, `iq` is the
selected scheduler IQ ID. Flags are a bit set: 1 = enqueue ports exhausted,
2 = IQ entries exhausted,
4 = replay queue above its admission limit, 8 = thread quota, and
16 = another compatible IQ is ready for this same checked opclass.
The flags are observations, not additive cycle costs. For split stores,
the store-data group is checked before the address group, matching admission.
The legacy search policy has no selected IQ (`iq=-1`) and reports the union
of gates across the searched group; it cannot have a ready peer on failure.
With `enableDispatchStage=True`, a full dispatch queue reports `iq=-1` and
`iq_flags=0`, regardless of the IQ search policy. This describes dispatch-queue
backpressure without an IQ selection.
These queries do not allocate, reroute, or alter scheduling state.

`frontend_reason` is the numeric `StallReason` propagated through Rename
for the sampled empty lane, captured before the pipeline time buffers advance.
It is preserved until the next admitted instruction supplies the existing
F_BUB/F_CTM classification. Zero means no propagated explanation. This is
pipeline context, not a causal claim: bundle buffering can separate the lane
from the originating Fetch event, and F_CTM remains a next-uop event tag.


`decodeStrandedSlots` is auxiliary stage context: on sampled cycles with
Decode unblocked, its fixed input buffer drained, and fewer than decodeWidth
instructions decoded, it counts up to the unused lanes still backed by raw
instructions in the existing stall FIFO. It includes tentative-path entries
and does not prove those instructions would decode or retire. It is separate
from the dispatch budget and must not be added to any DIP state or cause.
Compacting Decode (`enableDecodeFusionCompaction=True`) does not collect this
counter: its `tickCompaction()` path bypasses `decodeStranded()`. A zero value
in that mode does not show that the compaction buffer has no stranded work.

`readyBranchWaits` counts selectable, non-cancelled control entries remaining
after all ports in an IQ have selected. Shared ready lists are visited once;
new entries behind an enabled enqueue boundary are excluded. Counts are per
sampled cycle and include tentative paths. This is auxiliary stage context,
not part of the dispatch budget. A selected instruction may still lose later
register-port arbitration, which this counter does not count.

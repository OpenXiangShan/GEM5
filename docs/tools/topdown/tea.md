# TEA Usage Guide

TEA (Timing Event Attribution) samples pipeline states in the XiangShan O3 CPU
and attributes sample weights to instruction PCs and event signatures, called
Performance Signature Vectors (PSVs). Use it alongside the existing Topdown
statistics. TEA is disabled by default.

## Relationship to PerfCCT

TEA provides PC and event-signature weights through online aggregation.
The PerfCCT work in [#1216](https://github.com/OpenXiangShan/GEM5/pull/1216)
and [#1217](https://github.com/OpenXiangShan/GEM5/pull/1217) also ranks slow
PCs and commit blockers, and retains dynamic instruction and resource
relationships for detailed queries. Either can identify hotspots; PerfCCT
does not require a preceding TEA run. TEA can run without an ArchDB database.

The ROB readiness predicate is the source of commit-blocker selection.
`isHeadGroupReady(tid, &blocker)` returns the instruction whose readiness
check actually stopped commit. TEA consumes that result at the commit site,
using the same interface as #1216, rather than scanning the head group again.
An unready younger member behind a serialization boundary does not block
an otherwise eligible head prefix. LSU attempts, replay decisions and cache
responses are other existing observation points that both consumers can use;
their different output formats do not require duplicate timing decisions.

Compare the same ROI, thread and PC when relating the two outputs. Account
for partially used commit windows, fusion, RVV and recovery: TEA classifies
a cycle with any architectural commit as Computing, while PerfCCT can retain
the blocker that stopped further commits. Neither observed time nor event
weight directly measures recoverable IPC. PerfCCT's current causal coverage
is a subset of the mechanisms discussed by
[Microflow](https://arxiv.org/html/2607.13184v2).

## Enabling and Disabling TEA

Build `build/RISCV/gem5.opt` as described in the repository
[README](../../../README.md) and prepare a runnable image or checkpoint.
Run a raw binary image from the repository root:

```sh
./build/RISCV/gem5.opt --outdir=m5out/tea \
    ./configs/example/kmhv3.py \
    --raw-cpt --generic-rv-cpt=/path/to/image.bin \
    --enable-tea --tea-sample-period=1
```

For a GCPT slice, specify the checkpoint path and omit `--raw-cpt`:

```sh
./build/RISCV/gem5.opt --outdir=m5out/tea \
    ./configs/example/kmhv3.py \
    --generic-rv-cpt=/path/to/checkpoint.zstd \
    --enable-tea --tea-sample-period=16
```

These commands use the normal difftest configuration, such as an existing
`GCBV_REF_SO` setting. TEA requires no additional reference model or restorer.
Normal single-core GCPT slices use their embedded restorer.

| Option | Default | Meaning |
| --- | --- | --- |
| `--enable-tea` | Disabled | Create the TEA tracker and enable attribution |
| `--tea-sample-period=N` | `1` | Sample every N CPU cycles; both `0` and `1` sample every cycle |

Omit `--enable-tea` to disable TEA. Setting the sample period alone does not
enable it, and setting the period to `0` does not disable it. When disabled,
TEA creates no tracker, runs no attribution callbacks or event marking, and
produces no TEA statistics group or CSV. The normal execution path retains
enable checks and a small amount of field initialization.

TEA and PerfCCT have independent controls and are both disabled by default.
In this branch, `--arch-db-dump-lifetime` enables PerfCCT lifetime recording;
`--enable-arch-db` and `--arch-db-file` configure its database output.
Append the following arguments to the same simulation command:

| Mode | Additional arguments |
| --- | --- |
| Both disabled | None |
| TEA only | `--enable-tea` |
| PerfCCT only | `--enable-arch-db --arch-db-file=m5out/perfcct.db --arch-db-dump-lifetime` |
| Both enabled | `--enable-tea --enable-arch-db --arch-db-file=m5out/perfcct.db --arch-db-dump-lifetime` |

Enabling ArchDB alone does not enable PerfCCT lifetime recording. Neither
profiler enables or requires the other; shared observation points use each
profiler's own enable check.

Use `--enable-tea --tea-sample-period=16` to reduce sampling overhead. A larger
period reduces sampling and target queries, while event tracking remains
active. Fixed-period sampling can miss short events. Keep the sample period
and measurement window consistent when comparing configurations.

Custom Python configurations can set the O3 CPU parameters `enableTea`,
`teaSamplePeriod`, and `teaLastLevelCache` directly. The last parameter defines
the cache depth used to identify ST_LLC; `0` disables that event. The normal
cache configuration in `kmhv3.py` sets it to `3` with L3 enabled or `2` otherwise.

## Output Files

TEA produces two kinds of output when gem5 dumps statistics. For the default
single-core object name `system.cpu`:

- `system.cpu.tea.*` in `m5out/tea/stats.txt` contains state, event, and
  attribution quality statistics.
- `m5out/tea/system.cpu.tea.csv` contains sample weights grouped by thread,
  PC, and PSV.

The file prefix follows the CPU object name. Each CPU produces its own output
in a multi-CPU configuration. The CSV header is:

```text
thread,pc,psv,sample_weight
```

| Column | Meaning |
| --- | --- |
| `thread` | Hardware thread ID within the CPU |
| `pc` | Instruction address in hexadecimal; empty when no target address is known |
| `psv` | Event bitmask in decimal; negative values indicate special attribution states |
| `sample_weight` | Sample weight, possibly fractional; a cycle with multiple architectural commits splits its weight equally among them |

Each dump overwrites the CSV with a snapshot of the current statistics window.
A statistics reset clears accumulated weights and pending samples. For runs
with warmup and reset, pair the final CSV with the last statistics block in
`stats.txt`; do not add warmup statistics to it. The CSV has no separate
pipeline state column for each PC.

## States and Events

The four states in `samplesByState::*` are mutually exclusive. They are selected
in the priority order Computing, Flushed, Drained, then Stalled. Each active
thread contributes one sample at each sampling boundary:

| State | Meaning and attribution |
| --- | --- |
| Computing | At least one architectural commit is counted by TEA this cycle; split the weight equally among those commits. This does not imply full commit bandwidth utilization |
| Flushed | No such commit, and recovery or a ROB walk is active, or the ROB is empty with a valid recovery cause; attribute to the recovery cause |
| Drained | No such commit or recovery, and the ROB is empty; retain the sample until the next architectural commit |
| Stalled | Otherwise, the ROB is nonempty with no commit; select a commit blocker and wait for its retirement or squash |

PSV event bits are defined below. An instruction can carry multiple events:

| Bit | Mask | Event | Meaning |
| --- | ---: | --- | --- |
| 0 | 1 | DR_L1 | An instruction fetch response records cache access beyond L1 |
| 1 | 2 | DR_TLB | Instruction address translation is delayed |
| 2 | 4 | DR_SQ | A store or atomic cannot proceed due to SQ pressure with no older ROB work |
| 3 | 8 | FL_MB | A branch misprediction triggers recovery |
| 4 | 16 | FL_EX | An instruction exception triggers recovery |
| 5 | 32 | FL_MO | A memory ordering conflict or a commit-time MDP violation triggers recovery |
| 6 | 64 | ST_L1 | The data access path records an L1 miss |
| 7 | 128 | ST_TLB | Data address translation is delayed |
| 8 | 256 | ST_LLC | A cacheable load response reaches the configured last-level cache miss depth |

For example, `psv=320` means `ST_L1|ST_LLC`, and `psv=448` means
`ST_L1|ST_TLB|ST_LLC`. A value of `0` means none of these event bits is set and
still counts as successful attribution. It does not imply the absence of a
bottleneck: ordinary scalar execution waits, for example, have no dedicated
event bit.

| Special PSV | Meaning | Corresponding statistic |
| --- | --- | --- |
| `-1` | A Stalled target was squashed; its samples were cancelled | `cancelledTargets` |
| `-2` | A target has not retired, or a Drained sample is waiting for a successor commit | `pendingSamples` |
| `-3` | No selectable target or known recovery cause | `unresolvedTargets` |

## Reading and Analyzing Results

Inspect the statistics group:

```sh
rg 'system.cpu.tea\.' m5out/tea/stats.txt
```

Within the same statistics window, check these conservation relationships,
allowing for floating-point rounding in attribution weights:

```text
samples = sum(samplesByState::*)
samples = attributedSamples + cancelledTargets + unresolvedTargets + pendingSamples
samples = sum(CSV sample_weight)
```

Compute state shares as `samplesByState::<state> / samples` and event shares as
`samplesByEvent::<event> / samples`. Event statistics count successfully
attributed sample weights carrying the corresponding PSV bit. Events overlap,
so their shares must not be added. States and events do not map one to one:
a Computing sample can be attributed to an instruction carrying FL_MB.
`committedByThread::*` counts observed architectural commits regardless of the
sampling period; do not use it as the denominator for these shares.

The following command lists the top 10 successfully attributed PCs, grouped by
(thread, PC). The denominator includes cancelled, pending, and unknown samples;
the numerator includes only successful attribution:

```sh
python3 - m5out/tea/system.cpu.tea.csv <<'PY'
import csv
import sys
from collections import defaultdict

with open(sys.argv[1]) as f:
    rows = list(csv.DictReader(f))
total = sum(float(row['sample_weight']) for row in rows)
pcs = defaultdict(float)
for row in rows:
    if int(row['psv']) >= 0 and row['pc']:
        pcs[(row['thread'], row['pc'])] += float(row['sample_weight'])
for (thread, pc), weight in sorted(pcs.items(), key=lambda x: -x[1])[:10]:
    print(f'thread={thread} pc={pc} weight={weight:.3f} share={weight / total:.2%}')
PY
```

PC weights include successfully attributed samples from every state. They do
not directly represent stall cycles or removable performance losses. With a
sampling period N greater than 1, weights remain in sample units. Multiply by
N to estimate cycle counts for a single thread; shares need no scaling. Under
SMT, each active thread is sampled separately, so the total sample count is
not the physical CPU cycle count.

When aggregating SimPoints, first normalize by the instruction count in each
measurement window, then multiply by the SimPoint weight and the workload's
profile instruction count before computing shares. Do not directly average
slice percentages. Retain `stats.txt`, the CSV, `config.json`, and slice weights
for the same measurement window so the results can be checked.

## Visualization

Use [util/plot_tea.py](../../../util/plot_tea.py) to draw a stacked state chart
and an event heatmap. The script requires Python 3 and Matplotlib and runs
without a graphical display. Install Matplotlib in your Python environment
if needed:

```sh
python3 -m pip install matplotlib
```

Plot the last statistics block from a single run:

```sh
python3 util/plot_tea.py m5out/tea/stats.txt \
    --output /path/to/plots/tea-breakdown.png
```

Compare runs with one row per input file, in the supplied order:

```sh
python3 util/plot_tea.py /path/to/run-a/stats.txt /path/to/run-b/stats.txt \
    --labels "Run A" "Run B" --title "TEA comparison" \
    --output /path/to/plots/tea-comparison.svg
```

Use `--cpu system.cpu0` to select a different CPU object; the default is
`system.cpu`. The output file extension selects the format, including PNG,
SVG, and PDF. Parent output directories are created automatically.

The script also accepts an already aggregated summary CSV:

```sh
python3 util/plot_tea.py /path/to/summary.csv \
    --output /path/to/plots/tea-summary.pdf
```

The CSV must contain `benchmark`, one `<State>_pct` column for each of the four
states, and one `<Event>_pct` column for each of the nine events listed above.
Each row is a workload or configuration; `benchmark` supplies its display
label. Values are percentages in the range 0 to 100, with the four state
shares summing to 100. Additional columns are ignored. This is a summary
format, not the raw `system.cpu.tea.csv` format. Apply SimPoint weighting
before producing this summary; the plotting script does not aggregate slices
or average percentages across files.

Both panels use all samples as the denominator, including cancelled, pending,
and unresolved samples. Event weights overlap and do not represent removable
stall cycles. The script rejects missing TEA counters or zero-sample runs.

## Current Limitations

Known attribution limitations remain: cycles that retire intermediate RVV
micro-ops are not counted as Computing; some split memory accesses may miss
ST_L1 marking; and macroPSV from a partially retired RVV instruction may carry
into the exception handler. Cross-check these cases with existing Topdown
counters and instruction traces.

# IdealConstantLVP profiling

`IdealConstantLVP(enableProfiling=True)` writes
`m5out/ideal_constant_lvp_profile.csv` when a simulation exits. The file has
one row per `(tid, PC)` for each scope:

- `lifetime`: every commit-qualified LVP update from process start to exit.
  This is the table state that survives the warmup `resetstats` boundary.
- `roi`: only PCs updated after the most recent `m5.stats.reset()`. In the
  standard checkpoint workflow this is the measured ROI.

`ever_saturated=1` means the PC was observed with its value-confidence counter
at saturation during that scope. It answers whether the PC demonstrated a
run long enough to become eligible for constant prediction. `saturated_at_end`
is a final-state snapshot and answers a different capacity question: whether
the entry still occupies a fully confident slot at the phase boundary.
`updates`, `first_update`, `last_update`, and `first_saturation_update` retain
the frequency and temporal evidence needed to choose promotion thresholds and
replacement candidates; their sequence is per thread and per scope.

The default CI configuration uses a 9-bit counter and `resetConfidence=true`.
An entry starts at zero, so a single value must be committed 512 consecutive
times at one PC before the counter first becomes saturated.

Aggregate a complete performance archive with:

```bash
python3 util/valuepred/ideal_constant_lvp_profile.py \
  /path/to/performance_archive \
  --scope lifetime \
  --out-dir /tmp/ideal-constant-lvp-lifetime

python3 util/valuepred/ideal_constant_lvp_profile.py \
  /path/to/performance_archive \
  --scope roi \
  --out-dir /tmp/ideal-constant-lvp-roi
```

The tool writes:

- `per_slice.csv`: the capacity distribution used for a table-size decision.
- `per_benchmark.csv`: static-PC union within one benchmark image across its
  checkpoint slices.
- `per_pc.csv`: per-PC frequency and saturation evidence for replacement and
  promotion analysis.
- `summary.json`: maxima and percentile summaries for the full suite.

Capacity must be chosen from the per-slice distribution, not by summing all
slices. Slices are independent processes, and PCs from different benchmark
ELFs can reuse the same virtual address without representing the same static
instruction. The aggregate tool keys its static-PC union by `(benchmark, tid,
PC)` for that reason.

To evaluate the static set pressure of a candidate physical organization, run:

```bash
python3 util/valuepred/ideal_constant_lvp_capacity.py \
  /path/to/performance_archive \
  --scope lifetime \
  --table qf-1k-4w:1024:4:all \
  --table pct-1k-4w:1024:4:ever-saturated \
  --out-dir /tmp/ideal-constant-lvp-set-pressure
```

The mapper uses a compressed-instruction-safe xor-folded `pc >> 1` index and
reports final static collisions for each slice. It is deliberately not a
replacement or coverage simulator: temporal replacement needs an online shadow
table or an access trace, while this report only identifies static set hot
spots in a candidate geometry.

## Saturated value sharing

Saturated-value profiling additionally writes
`m5out/ideal_constant_lvp_saturated_values.csv`. Each row is one raw `RegVal`
value segment of a saturated `(tid, PC, saturation_epoch)`. Aggregate it with:

```bash
python3 util/valuepred/ideal_constant_lvp_values.py \
  /path/to/performance_archive \
  --scope roi \
  --out-dir /tmp/ideal-constant-lvp-values-roi
```

The tool writes:

- `per_slice_values.csv`: per-slice distinct-value and sharing summaries.
  `cumulative_distinct_values` counts a separate value slot for every PC;
  `global_distinct_saturated_values` unions raw values across PCs in that
  slice, so their difference is the possible sharing reduction.
- `per_pc_values.csv`: per `(tid, PC)` distinct saturated values, value-segment
  count, epoch count, and sharing fanout.
- `per_value_sharing.csv`: raw values and the PC fanout that shares each one.
- `summary.json`: suite-wide distributions. Like other capacity summaries,
  never sum independent slice capacities to choose one table size.

`concurrent_distinct_value_peak` is the online
`profile*PeakDistinctSaturatedValues` stat maintained by the predictor's live
saturated-entry set, and is the hardware value-register capacity metric. The
separate `interval_concurrent_*` fields are an offline inclusive sweep of CSV
saturated-value-segment boundaries. They are not the first/last prediction-use
intervals in `ideal_constant_lvp_prediction_intervals.csv`; they provide
temporal diagnostics only. At a committed value-change boundary both old and
new segments can be present in the dump, so they must not replace the online
capacity stat. If the relevant stats counter is absent, the primary peak is
reported as unavailable rather than falling back to the interval result.

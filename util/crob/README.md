# kmhv2 CROB interruption statistics

This instrumentation measures how complex instructions split compressible
instruction runs in the existing `kmhv2` ROB grouping policy.

## Measurement scope

- A simple instruction is exactly what `ROB::allocateGroup_kmhv2()` accepts for
  compression: it is not a memory reference, control instruction, or
  non-speculative instruction.
- A complex block is counted as an interruption only for the pattern
  `simple + one or more complex + simple` in one successfully admitted rename
  bundle and one FTQ entry.
- Complex instructions at the beginning or end of a bundle are not counted as
  interruptions.
- The counters are allocation-path statistics. They can include younger
  wrong-path instructions that are squashed later.
- The counterfactual entry count removes only fragmentation by complex
  instructions. It preserves the group-width, rename-cycle, and FTQ limits.

## Manual performance CI

Use `.github/workflows/manual-perf.yml` with these inputs:

- configuration: `kmhv3.py`
- benchmark type: a non-SMT SPEC set
- extra args: `--rob-compress-policy=kmhv2 --crob-inst-per-group=8`

Successful matching runs automatically add four CSV files to the normal
GitHub Actions artifact and to the server archive under
`crob-kmhv2-analysis/`.

## Manual aggregation

```sh
python3 util/crob/aggregate_kmhv2_break_stats.py \
  /path/to/performance-run/spec_all \
  --weights /path/to/cluster-config.json \
  --output-dir crob-kmhv2-analysis
```

Outputs:

- `crob_kmhv2_slices.csv`: raw counters and derived metrics per slice.
- `crob_kmhv2_workloads.csv`: normalized SimPoint-weighted workload metrics.
- `crob_kmhv2_breaking_instruction_distribution.csv`: instruction types in
  interrupting complex blocks.
- `crob_kmhv2_simple_run_distribution.csv`: simple-run length distribution.

The most useful metrics are:

- `simple_fraction`: fraction of allocated instructions eligible for kmhv2
  compression.
- `breaking_complex_fraction`: fraction of complex instructions that occur in
  an interrupting complex block.
- `break_blocks_per_1k_inst`: interruption blocks per 1,000 allocated
  instructions.
- `break_lost_fraction`: extra ROB entries caused by interruptions divided by
  actual ROB entries.
- `actual_rob_density`: allocated instructions per actual ROB entry.
- `no_break_rob_density`: density if complex instructions did not fragment
  simple runs.

# TEA Analysis Tools

TEA exports `(thread, pc, psv, sample_weight, state)`. The state is the state
at sampling time, including samples attributed after retirement or cancellation
and pending/unresolved samples. Summing over state reproduces the original
PC/PSV totals; summing over PC/PSV reproduces `samplesByState`. Sampling and
execution timing are unchanged. See [TEA](tea.md) for event semantics.

The scripts below use the Python standard library. Existing table-based
weighted comparisons and plots remain in `util/compare_tea.py` and
`util/plot_tea.py`.

## Frozen experiment identity

```sh
python3 util/tea/audit_experiment_identity.py \
    --manifest /path/to/experiment/manifest.json \
    --build /path/to/build.json --source-patch /path/to/source.patch
```

The manifest supplies `binary`, `binary_sha256`, and `source_patch_sha256`.
The build supplies `complete`, `returncode`, `source_patch_sha256`, and
`variants.fast.binary_sha256`; use `--variant opt` for an opt build. The tool
hashes the binary and patch, reports each check, and exits nonzero on failure.

Add `--completed-runs` to verify `complete.json` and each
`<point>-<variant>/run.json` and `run.log` in the manifest directory. The
manifest declares `points` and `variants`. When it declares
`new_long_control_points` and `new_long_control_variants`, completion records
are grouped by point and the matrix includes those controls. Every declared
run must appear once, exit successfully, use the frozen binary, and have one
logged command matching its recorded arguments. This checks recorded commands,
not an exited process's environment.

## Weighted partial comparisons

```sh
python3 util/tea/summarize_four_corner_subset.py \
    --profile cluster.json --baseline-audit baseline-audit.json \
    --experiment-audit experiment-audit.json \
    --corners control candidate --output contributions.json
```

For four corners, use `--corners control factor_a factor_b combined` to also
report interaction and combined marginal effects. The first corner is always
the control. Inputs are prior passed audits, not raw simulation directories:

- The profile maps workloads to `insts` and `points` (point-to-weight mappings).
  Workload names use the benchmark prefix before the first underscore.
- The baseline has `passed`, `points`, and `rows` containing `point`, `cycles`,
  and `instructions`, and must cover the exact full profile.
- The experiment has `passed`, `failures`, `rows`, `pending`, `completed_points`,
  `expected_points`, and `complete`. Each row contains `point` and
  `measured[corner]` with `cycles` and `instructions`; the control window must
  match the baseline exactly.

Contributions use normalized SimPoint weights and input instruction counts.
Time deltas are percentage points of each benchmark's full baseline time;
negative is beneficial. Unknown candidate points stay unknown. Partial
contributions are not complete benchmark gains or suite scores.

## Schedule traces

```sh
python3 util/tea/analyze_schedule_cohort.py \
    --trace trace.gz --clock-ticks 333 \
    --fetch-start 100000 --fetch-end 200000 --pcs 0x1000 0x2000 \
    --output cohort.json
python3 util/tea/analyze_schedule_dependencies.py \
    --trace trace.gz --clock-ticks 333 \
    --fetch-start 100000 --fetch-end 200000 --pcs 0x2000 \
    --output dependencies.json
```

Use a gzipped trace containing Schedule and O3PipeView records. Both tools
select retired scalar instructions by PC and fetch interval. The cohort tool
uses observed scheduler insertion rather than O3PipeView's zero dispatch
field, and retains first/last events across retries. Adjacent sequence pairs
show program order, not data dependence.

The dependency tool captures physical-register ownership when each dependency
is registered, before tag reuse. It uses observed add-to-FUs times and reports
pipeline PC and scheduled disassembly separately for fused instructions.
Missing observations remain unknown and timing offsets remain signed; these
are local trace observations, not exclusive or recoverable stall cycles.

## Declared CPU observer fields

`cpu_observer_fields.audit_added_cpu_observers(before_phases, after_phases,
observers)` checks that each declared observer was absent before and present
after. It reports other emitted CPU key additions/removals separately, since
sparse histogram/vector output can change with behavior. It does not establish
registered-schema equality or numeric neutrality; callers must check those.

## Validation

```sh
python3 -m unittest discover -s tests/pyunit/tea -p 'pyunit*.py' -v
python3 -m unittest discover -s util/tea -p 'test_*.py' -v
```

The tracker tests compile the real `src/cpu/o3/tea.cc` against small fixtures
and exercise 13 state/quality scenarios, including delayed attribution,
cancellation, pending/drained samples, reset, sparse sampling and threads.
The utility tests cover Schedule timing, physical ownership, and sparse
observer fields. These checks do not replace simulator regression or verify
official CI success, source provenance, correctness, or resource budgets.

# TEA State Preservation and Audited Analysis

This tool-only branch starts at `xs-dev` commit
`1cf461fbc000427f3faaebdb4c130190d5d9bffb`, confirmed from the remote on
2026-11-04. It collects the sampling and offline-analysis improvements from
the SPEC17 investigation. It changes no CPU, scheduler, predictor, or cache
configuration defaults. Simulated performance improvements from that
investigation belong to its configuration experiments, not to these tools.

## Improvements

| Area | Change | Reason |
| --- | --- | --- |
| TEA export | Aggregate by thread, PC, PSV, **and sampled state** | PC totals and global state totals cannot reconstruct their joint distribution |
| Deferred attribution | Preserve Stalled/Drained through retirement, cancellation, pending and unresolved export | Completion time must not replace sampling time |
| Raw analysis | Stream CPU statistics and CSVs; reuse the control parse in paired screens | Avoid retaining unrelated statistics and repeatedly parsing the same control |
| Accounting | Read actual DIP width; include pending and unassigned-PC budgets | Resolved slots and named PCs alone do not cover the measurement window |
| Comparison | Weight measured CPI by SimPoint and input instruction counts; retain positive and negative contributions | A large slice percentage need not be a large benchmark contribution |
| Symbols | Select application/kernel ELFs explicitly; reject addresses outside executable LOAD segments | addr2line can return a nearby symbol even when the ELF does not cover the PC |
| Audit | Check official CI terminal state, source/profile/REF, exact point coverage, actual INI and independent scores | A score file or process exit alone does not validate the experiment |

The producer change adds `state` to the existing CSV. Marginalizing that
column preserves the old `(thread, pc, psv)` totals. Summing it reproduces
`samplesByState::*`, including cancelled, pending and unresolved samples.
The four pipeline states, event definitions and target timing are unchanged.
See the [usage guide](tea.md) for sampling semantics.

## Analysis entry points

The scripts under `util/tea/` use only the Python standard library except for
external ELF inspection/disassembly programs used by the PC analysis.
These scripts were extracted from the validated SPEC17 workflow. Their
current raw-window contract is single-thread XiangShan O3, 333 ps clock,
TEA and DIP enabled with period 1, difftest and memory deduplication enabled,
and two statistics blocks (warmup followed by ROI). The suite entry further
checks 20M warmup plus 20M ROI, the current SPEC17 profile schema, 38 inputs
and 23 scored benchmarks. It does not silently treat sparse samples as
all-cycle CPI, or substitute SPEC06 weights.

`util/compare_tea.py` remains the existing table-based weighted comparison
entry for other exported datasets. `util/plot_tea.py` remains the plotting
entry. This branch does not duplicate those interfaces.

Audit one recorded SPEC17 CI archive after obtaining its official run JSON,
dispatch receipt, and SPEC17 reference-time JSON:

```sh
python3 util/tea/analyze_spec17_tea_dip.py suite /path/to/ci-archive \
    --run-json /path/to/official-run.json \
    --dispatch-json /path/to/dispatch.json \
    --references /path/to/spec17_reftime.json \
    --output /path/to/control-audit.json
```

The existing `screen` mode consumes the investigation's paired-screen
manifest and checks its named parameter variants. Each control point is
parsed once for its variants. Neither mode launches simulation or CI. DIP CSVs must come from a build
with DIP support; this branch adds no DIP producer to the mainline simulator.
Checks include CSV/statistics agreement, both TEA state and attribution
quality budgets, DIP slot/state/pending budgets, actual checkpoint and REF,
configuration expectations, resource consistency and independently computed
scores. The CSV is paired with ROI statistics; warmup counts are not added.

Compare two passed, identical-profile suite audits:

```sh
python3 util/tea/compare_spec17_tea_dip.py \
    /path/to/control-audit.json /path/to/candidate-audit.json \
    --output /path/to/comparison.json
```

For each benchmark, the effective point weight is its normalized SimPoint
weight multiplied by its input's instruction share. The tool sums weighted
CPI before taking a speedup ratio, and uses benchmark geometric means for
group scores. Positive and negative witnesses use weighted absolute CPI
changes; cross-benchmark suite contribution uses log speedup.

Trace the selected benchmarks' largest weighted gains and regressions:

```sh
python3 util/tea/analyze_spec17_weighted_pcs.py \
    /path/to/comparison.json /path/to/control-audit.json \
    /path/to/candidate-audit.json \
    --before-archive /path/to/control-archive \
    --after-archive /path/to/candidate-archive \
    --benchmarks exchange2 blender parest \
    --application-elf blender=/path/to/verified-blender.elf \
    --objdump riscv64-unknown-linux-gnu-objdump \
    --output /path/to/weighted-PCs.json
```

`llvm-readelf`, `llvm-addr2line`, and the RISC-V objdump are resolved through
PATH; `--addr2line` and `--objdump` accept explicit executables. Application
ELFs normally use the profile's shared benchmark ELF; high kernel addresses
use the input manifest's vmlinux. `--application-elf BENCHMARK=PATH` supplies
an actual application image when the profile's auxiliary ELF differs. Verify
its provenance separately: an executable-range check can reject a wrong
image, but cannot by itself prove the correct image was selected.
The SPEC17 Blender case required extracting the application from the
manifest-SHA-verified rootfs; its auxiliary ELF produced invalid `_end`
lookups for the application hotspots. Out-of-range PCs remain unknown.
Only the exact instruction at the requested address is retained.

## Complete configuration pairing

`audit_spec17_config_pairs.py ROOT BEFORE AFTER` consumes passed
`ROOT/<label>-audit.json` and `<label>-state.json` records. Every configuration
SHA must match its suite audit. Unlike a capacity-only resource list, every
INI section and parameter is compared against explicit expected differences:

```sh
python3 util/tea/audit_spec17_config_pairs.py /path/to/records \
    control candidate --expected-changes /path/to/expected-changes.json \
    --output /path/to/config-pairs.json
```

`expected-changes.json` maps section names to parameter names and exact
`[before, after]` strings. An empty object requires identical model settings.
It is experiment input: the tool does not assume a particular optimization.

Different DRAM config paths require existing content snapshots:
`ROOT/dependencies/<label>-dram.json` contains `original_path`, `snapshot`
(relative to that dependencies directory), and `sha256`. The snapshot files
must match those hashes, both hashes must agree, and every actual INI must
reference the recorded original path. Only that content-verified path
variation is excluded before the expected-change comparison.

## Frozen experiment identity

Check a paired-run manifest against its completed build record and the exact
source patch before using its results:

```sh
python3 util/tea/audit_experiment_identity.py \
    --manifest /path/to/experiment/manifest.json \
    --build /path/to/build.json \
    --source-patch /path/to/source.patch
```

The manifest records `binary`, `binary_sha256`, and `source_patch_sha256`.
The build records `complete`, `returncode`, `source_patch_sha256`, and
`variants.fast.binary_sha256` (`--variant opt` selects the opt record).
The command hashes the existing binary and patch, prints each identity check
as JSON, and exits nonzero on a mismatch or unsuccessful build. It does not
modify records or rerun simulations. This binds recorded inputs; correctness,
resource, coverage and official CI checks remain separate.

## Validation and limits

Run the targeted regression suite from the repository root:

```sh
python3 -m unittest discover -s tests/pyunit/tea -p 'pyunit*.py' -v
```

It compiles the **real** `src/cpu/o3/tea.cc` against small CPU/ROB/statistics
fixtures, then checks 13 tracker scenarios. Two timelines deliberately have
identical old PC/PSV marginals and identical global state totals, but differ
in which PC stalled. Other scenarios cover delayed events, cancellation,
pending/unresolved targets, drained successors, split/micro commits, reset,
sampling period 3 and thread isolation. The raw-analysis tests exercise
width 8, pending slots, no-PC weights, overlapping causes, state mismatch,
sparse-sample rejection and rejection of official `in_progress` as success.
The fixtures validate the tracker; they do not replace a simulator build or
checkpoint regression.

The originating investigation additionally verified eight short observer
pairs (including period 7 and TEA disabled), two 20M+20M observer pairs,
and seven full-profile weighted witnesses (14 raw windows). Existing CPU
statistics matched in both phases, DIP CSVs matched byte for byte, and TEA
marginals matched within floating-point tolerance. Those are evidence for
the tested source/workloads; port validation on this base is reported
separately rather than inferred from those earlier runs.

Port validation on the base above completed an optimized RISC-V simulator
build and five fresh 1M-warmup/1M-ROI windows: GCC with TEA enabled, disabled
and period 7, and Fotonik3d enabled and disabled. Difftest passed; all existing
CPU statistics (excluding the TEA group itself) matched exactly in both
phases. Enabled exports closed state and attribution-quality budgets, while
disabled runs produced no TEA CSV. These are focused observer checks, not a
new 0.3/1.0 performance sweep. The ported consumers also reproduced the
archived 14 windows, the full 1783-point comparison, every one of the 1783
configuration pairs, and the seven selected points' symbol results.

State preservation can increase output size: one GCC short window grew from
61,731 to 79,953 CSV rows. Parsing retained 6,922 CPU fields instead of 11,919
fields in one measured block, with exact CPU values; control reuse reduced
24 paired comparisons from 48 window parses to 36. These are field/parse
counts, not a controlled host wall-clock speedup. Hashing still reads files.

TEA and DIP are independent state partitions and must close separately.
Overlapping cause/event bits cannot be added as recoverable CPI. Matching
PCs do not establish a dynamic dependency edge. SHA values establish input
identity, not correctness by themselves. The scripts audit existing files
and recorded CI state; they never submit CI or restart a timed-out observer.


## Partial paired and four-corner comparisons

Summarize completed audited points with full input-instruction and SimPoint
weights, retaining pending and unknown points:

```sh
python3 util/tea/summarize_four_corner_subset.py \
  --profile cluster.json --baseline-audit baseline-raw-audit.json \
  --experiment-audit four-corner-audit.json \
  --corners control best compute combined --output contributions.json
```

For a two-version experiment, pass its control and candidate names instead:

```sh
python3 util/tea/summarize_four_corner_subset.py \
  --profile cluster.json --baseline-audit baseline-raw-audit.json \
  --experiment-audit paired-audit.json \
  --corners parent candidate --output paired-contributions.json
```

Paired output reports candidate signs and measured time contributions, without
interaction or marginal fields. The baseline may be a fully audited parent
version; its exact point windows must match the experiment's control windows.

The experiment audit supplies `rows[].measured[corner]` cycle and retired
instruction counts plus completed/pending coverage. The baseline audit must
cover the exact full profile; every measured control must match its baseline
window. Output includes signs and measured time contributions normalized to the
complete original baseline benchmark time. Negative time contributions are
beneficial. Unknown candidate points remain unknown; partial contributions are
neither complete benchmark gains nor suite scores. This summarizes prior audit
results; it does not re-audit simulator correctness or resource budgets.

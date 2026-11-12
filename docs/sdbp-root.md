# SDBP experiment options

`SDBP-ROOT` starts from `39a2db6f92`, the youngest-two sampler experiment.
It exposes controls for replacement, bypass, and sampler training eligibility.
Defaults preserve that commit's unguarded, L1-copy-protected behavior.

| CLI option | Default | Effect |
|---|---|---|
| `--l2-replacement-policy=sdbp` | Platform default unless selected | Enable SDBP prediction and real-L2 dead-first replacement together |
| `--sdbp-l1-copy-protection` / `--no-sdbp-l1-copy-protection` | Enabled | Filter L1-resident blocks from the real-L2 dead scan |
| `--sdbp-sampler-dead-first` / `--no-sdbp-sampler-dead-first` | Enabled | Select dead-first or pure LRU replacement in the independent sampler |
| `--sdbp-sampler-protect-youngest=N` | `0` | Exclude the N youngest sampler entries from the dead-first scan |
| `--sdbp-sampler-demand-pc-only` / `--no-sdbp-sampler-demand-pc-only` | Disabled | Only let L2 demand requests with a PC access, allocate, and train the sampler |
| `--sdbp-enable-bypass` | Disabled | Bypass dead fills when no eligible dead or unused old victim is available |
| `--sdbp-bypass-dead-victims` | Disabled | Restore the original bypass-first policy without victim aging |

There is no separate real-L2 dead-first switch: it is part of SDBP. Disabling
sampler dead-first changes sampler residency and consequently training, while
the real L2 continues to select predicted-dead victims.

For both the real cache and sampler, invalid entries have first priority.
L1-copy protection filters the real-cache dead scan and also protects an old
LRU victim when deciding whether to bypass. The sampler recency guard applies
only to the sampler dead-first scan. Neither clears dead bits or restricts
the ordinary global LRU fallback.
The sampler guard counts recency positions across all entries, including live
entries. It accepts any nonnegative integer: zero disables protection, two
reproduces the previous youngest-two mode, and N >= sampler associativity
protects every entry from the dead scan, so selection falls back to LRU.
With sampler dead-first disabled, the guard has no effect. By default,
demand/prefetch sampling remains active in either replacement mode.

With bypass enabled, an eligible dead resident victim takes priority over
bypassing the incoming dead fill. When no such victim exists, the LRU victim
remains protected for one slice-capacity of policy references
(`num_sets * candidate_ways`), or while its L1 copy is still resident.
Bypassed references advance this clock, so streaming fills cannot indefinitely
freeze unused live residents. `--sdbp-bypass-dead-victims` restores the original
bypass-first decision and clock behavior for controlled comparisons. These
choices do not alter independent sampler training or bypass transport.
See [the performance analysis](analysis/fixbypass/report.md) for request/PC
traces, native TopDown evidence, local results, and the full-suite CI run.

Enabling `--sdbp-sampler-demand-pc-only` filters out all prefetch requests
and all requests without PC metadata before the sampler lookup, on both
real-L2 hits and fills. Filtered requests cannot allocate or evict sampler
entries, train old signatures live or dead, or change sampler metadata and
LRU order, even if their tag already exists. PC presence uses `hasPC()`,
so a valid PC of zero is accepted. A prefetch fill marked as a sampler
demand after merging a demand follows the existing demand classification
and still requires PC metadata. Lookup/eligible/no-PC counters retain their
existing meanings; sampler and training counters exclude filtered events.

L1-copy protection uses an ideal, read-only lookup of the owning CPU's L1I/L1D
resident tags, with no modeled latency or area. Disabling it removes the probe
targets and allows the original unfiltered L2 dead-first selection.

Examples appended to an existing KMHV3 checkpoint command:

```text
# Reproduce the run 40859791862 policy configuration.
--l2-replacement-policy=sdbp --sdbp-sampler-num=128 --sdbp-sampler-assoc=6

# Enable two-entry protection in the dead-first sampler.
--l2-replacement-policy=sdbp --sdbp-sampler-protect-youngest=2

# Only sample and train L2 demand requests with PC metadata.
--l2-replacement-policy=sdbp --sdbp-sampler-demand-pc-only

# Pure LRU sampler, unfiltered real-L2 dead-first, optional bypass enabled.
--l2-replacement-policy=sdbp --no-sdbp-sampler-dead-first \
    --no-sdbp-l1-copy-protection --sdbp-enable-bypass
```

The inherited `idealkmhv3.py` uses a 1 MiB, eight-way L2 in four slices and a
32 MiB mostly-inclusive L3. Prefetching follows the command line: it remains
enabled by default and `--no-pf` disables it. This change does not import the
separate enhanced L1-to-L3 bypass branch or real-L2 MRU guard.

## Validation

The C++ core tests cover numeric protection counts, dead-first candidate order,
pure-LRU selection and retained training, invalid-entry priority, unchanged dead
bits, prefetch-hit recency, and small-sampler fallback. Policy tests cover L1
protection enabled/disabled, the independence of L2 and sampler dead-first,
parameter propagation, and basic bypass eligibility. Configuration smoke runs
inspect the generated parameters for every L2 slice.

Validated locally on 2026-10-30:

- `gem5.opt` and both SDBP unit-test targets build successfully.
- 20 predictor-core tests and 15 replacement-policy tests pass.
- Two Python configuration tests cover defaults, invalid guard values, and 80
  combinations of classic/sliced L2, L1 protection, sampler policy, bypass, and
  numeric guard; all cores and slices receive the expected parameters.
- Two million mixed demand/prefetch events match the parent core exactly for
  guard counts 0 and 2 at sampler associativities 1, 2, 6, 7, and 8.
- Core tests also pass with AddressSanitizer and UndefinedBehaviorSanitizer.
- Four libquantum checkpoint smoke runs complete 100,000 instructions with
  difftest: default, guard=3, pure-LRU sampler without L1 protection, and basic
  bypass with guard=1. All four L2 slices have the expected generated options.
  These runs use `DDR4_2400_16x4` because this fresh build has no DRAMsim3
  library, and the available `d30fff1ece9e-gem5-r3-multi16g-zfa-cbo/normal-dedup`
  reference for memory deduplication. These are correctness/configuration
  checks, not performance measurements.
- Modified-region gem5 style checks and `git diff --check` pass. The CLI and
  new Python test pass Black; the existing replacement-policy definitions
  retain their surrounding format.

Validated locally on 2026-11-03 for demand/PC filtering:

- `gem5.opt` and both SDBP unit-test targets build successfully.
- All 24 core tests and 17 policy tests pass. New cases cover rejected hits
  and misses, unchanged training/metadata/LRU, valid PC zero, retained demand
  training, merged-demand responses, and legacy behavior with the switch off.
- Both Python configuration tests pass with the new binary, including 160
  combinations of classic/sliced L2 and experiment options across two cores.
- Modified-region gem5 style checks and `git diff --check` pass.

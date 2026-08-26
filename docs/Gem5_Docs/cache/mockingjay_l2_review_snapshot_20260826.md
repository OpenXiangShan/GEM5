# Mockingjay L2 Review Snapshot

## Snapshot Contract

This branch is a review-only handoff captured before follow-up implementation
work. It is deliberately based on the published implementation checkpoint:

| Item | Value |
| --- | --- |
| Review branch | `codex/mockingjay-l2-review-summary-20260826` |
| Snapshot commit | `c95ff7ac13c9e21dc505a266f6ea460f3f422ae3` plus this document |
| Implementation base | `5361c1248804755d285313f41dd73b7a299f7b48` |
| Last source-bearing implementation commit | `c2dbe9837bec4cbe99f165de98f5d1849de66c8c` |
| Later source state | `34f4705357`, `4176c788d2`, and `c95ff7ac13` update handoff documentation only |
| CI performance run | Not dispatched |

No uncommitted work from the implementation worktree is included here. This
makes the branch reproducible for review: every source file is exactly the
published `c95ff7ac13` version, and the only new file is this summary.

## What The Implementation Adds

The patch introduces a behavioral Mockingjay replacement policy for each
non-classic aligned L2 slice.

| Area | Files | Current behavior |
| --- | --- | --- |
| Configuration | `configs/example/kmhv3.py` | Derives the per-slice geometry and creates one `MockingjayL2RP` object per `inner_cache`. Predictor state is not shared between slices. |
| SimObject and build registration | `src/mem/cache/replacement_policies/ReplacementPolicies.py`, `SConscript` | Exposes `MockingjayL2RP` and generates its parameters. |
| Policy model | `mockingjay_l2_rp.hh`, `mockingjay_l2_rp.cc` | Implements sampled history, RDP, signed ETR, per-set aging, writeback insertion, replacement statistics, and a bypass prediction. |
| Packet-aware victim path | `replacement_policies/base.hh`, `dueling_rp.*`, `tags/{base,base_set_assoc,vipt_set_assoc}.*` | Adds `getVictim(candidates, pkt)` and propagates the packet through tags when the caller enables the optional bypass result. |
| Direct response bypass | `src/mem/cache/base.{hh,cc}` | For a narrowly eligible clean L2 `ReadSharedReq`, a `nullptr` victim bypasses allocation and services the target from the lower-level response. |
| Focused tests | `mockingjay_l2_rp.test.cc`, `cache.test.cc` | Covers policy mechanics and clean/dirty/`ReadCleanReq`/pending-downgrade response paths. |

## Current Control Flow

| Event | Source path | Effect |
| --- | --- | --- |
| L2 hit | `BaseSetAssoc::accessBlock()` -> `MockingjayL2::touch()` | Samples reuse history, ages the set periodically, and promotes the resident ETR from the RDP prediction. |
| Normal admitted fill | `BaseCache::handleFill()` -> tags `findVictim()` -> `MockingjayL2::reset()` | Selects a victim, installs the line through the normal cache/coherence path, samples the miss, ages the set, and assigns the insertion ETR. |
| Eligible clean demand fill | `BaseCache::recvTimingResp()` -> packet-aware `getVictim(candidates, pkt)` | If Mockingjay returns `nullptr`, the lower response completes the one CPU target without allocating a temporary or resident block. |
| Writeback fill | `MockingjayL2::reset()` | Keeps the line and inserts it with negative scan ETR; it is not bypassed. |

The direct-bypass predicate is intentionally conservative. It requires a
single CPU-side `ReadSharedReq`, a clean lower response, no MSHR downgrade or
invalidation state, and excludes writable, atomic, cache-maintenance,
prefetch, LL/SC, locked-RMW, and uncacheable operations. This boundary avoids
skipping normal coherence-state conversion in unsafe cases.

## Reported Validation At The Snapshot

The following results were recorded in the published handoff. They are
evidence from that checkpoint, not tests rerun by this summary branch.

* `build/RISCV/mem/cache/replacement_policies/mockingjay_l2_rp.test.opt`:
  10 of 10 tests passed.
* `build/RISCV/mem/cache/cache.test.opt`: five timing cases passed, including
  direct clean bypass, admitted clean fill, dirty responder, `ReadCleanReq`,
  and pending downgrade.
* `python3 -m py_compile configs/example/kmhv3.py` and `git diff --check`
  passed.
* A one-million-instruction `omnetpp/6881` checkpoint smoke constructed four
  policies and completed with `simInsts=1000007` and
  `system.cpu.committedInsts=1000007`.

The smoke used local DDR4 and a checkpoint-compatible reference. It verifies
construction and execution, not matched CI performance. No GCC15 SPEC06 A/B
result exists yet.

## Findings To Resolve Before Performance Attribution

The following are source-level review findings for this snapshot. The first
two change the algorithmic input or learning stream and should be fixed before
any performance conclusion.

### MJ-01: Packet-Aware Ordering Is Coupled To Direct Bypass Eligibility

**Severity: P1. Status: confirmed.**

`BaseCache::recvTimingResp()` supplies the optional bypass-result pointer only
when the direct-response safety predicate holds. `BaseSetAssoc` then calls the
packet-aware overload only when that pointer is non-null; otherwise it calls
the old packet-less `getVictim(candidates)` path.

Consequently, fills that are unsafe to direct-bypass (for example
`ReadCleanReq`, writable/coherence fills, prefetches, or merged requests) do
not compare the incoming RDP prediction with the resident victim ETR. They
fall back to resident-only `abs(ETR)` selection. That preserves functional
safety, but it is not the full Mockingjay incoming-line ordering rule.

The fix should separate two decisions:

1. Pass the incoming packet to victim selection for every cacheable fill that
   should use Mockingjay ordering.
2. Permit a `nullptr` direct bypass only when `BaseCache` has established the
   existing response-safety predicate.

The implementation must retain normal allocation and coherence processing for
an unsafe direct response even when the incoming line is scan-like. A focused
test should prove that an ineligible fill still receives packet-aware ordering
but cannot take the direct-response bypass.

### MJ-02: Non-Demand Traffic Can Train Demand History

**Severity: P1. Status: confirmed.**

`BaseSetAssoc::accessBlock()` invokes `touch(replacementData, pkt)` for every
tag hit. `MockingjayL2::touch()` rejects only writebacks. Likewise,
`MockingjayL2::reset()` applies ordinary sampled-history and ETR insertion to
every non-writeback fill.

This allows packets such as `CleanEvict`, `WriteClean`, and cache-maintenance
traffic to advance sampled timestamps, train the reserved no-PC RDP bucket,
or alter ETR. Those packets are cache-management traffic rather than demand
reuse observations, so they should not be mixed with the demand predictor's
history.

At minimum, filter `CleanEvict`, `WriteClean`, and requests for which
`req->isCacheMaintenance()` is true from sampled-history, RDP, aging, and
promotion/insertion training. Add tests for a hit and a fill so the filter is
observable in predictor and ETR statistics.

### MJ-03: Generic Parameter Guards Need Completion

**Severity: P2. Status: confirmed boundary.**

The default geometry is safe (`block_bits=6`, `slice_bits=2`), but the policy
does not reject generic overrides that shift an `Addr` by too many bits.
`timestamp_bits` is allowed to be smaller than the configured history window;
for example, a six-bit timestamp with default `INF_RD=63` cannot represent an
elapsed distance greater than the history boundary. Add constructor guards
that relate timestamp range and address-shift widths to the configured
geometry.

### MJ-04: Configuration Diagnostic Lower Bound Is Inconsistent

**Severity: P3. Status: confirmed.**

`kmhv3.py` rejects only `slice_size_bits <= 10`, which permits a 2 KiB slice,
while its error message says the lower bound is 4 KiB. Align the condition or
the diagnostic.

### MJ-05: Degenerate Sampling And Counter Semantics Need Documentation

**Severity: P3. Status: review boundary.**

For `sampled_sets == 1`, the implementation samples only set zero, rather than
the literal diagonal predicate used for larger values. This is a reasonable
parameterization choice, but it should be documented or prohibited if exact
reference behavior is required. Also, an admitted no-PC fill computes a
signature during both packet-aware victim selection and `reset()`, so the
`noPcSignatures` statistic can count one access twice. This affects
observability, not replacement correctness.

## Recommended Follow-Up Order

1. Resolve MJ-01 with an interface that separates packet-aware ordering from
   permission to turn a policy bypass into a direct response.
2. Resolve MJ-02 and add focused traffic-class regression tests.
3. Add the parameter/diagnostic guards from MJ-03 and MJ-04; decide and
   document the MJ-05 behavior.
4. Rebuild the policy and cache timing tests, then rerun the checkpoint smoke.
5. Only after a clean source checkpoint and generated `config.ini` audit,
   dispatch the already documented matched GCC15 SPEC06 A/B contract.

## Review Commands

```bash
git log --reverse --oneline \
  5361c1248804755d285313f41dd73b7a299f7b48..c95ff7ac13
git diff --stat 5361c1248804755d285313f41dd73b7a299f7b48 c95ff7ac13
git diff 5361c1248804755d285313f41dd73b7a299f7b48 c95ff7ac13 -- \
  src/mem/cache/replacement_policies/mockingjay_l2_rp.cc \
  src/mem/cache/base.cc src/mem/cache/tags/base_set_assoc.cc \
  configs/example/kmhv3.py
```

Read this snapshot together with `mockingjay_l2_implementation.md` for the
algorithm contract and `mockingjay_l2_progress.md` for the recorded local
validation and the not-yet-dispatched CI contract.

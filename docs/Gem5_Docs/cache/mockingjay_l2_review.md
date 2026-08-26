# Mockingjay L2 Review Handoff

## Current checkpoint

* Branch: `codex/mockingjay-l2-5361c12`
* Base: `5361c1248804755d285313f41dd73b7a299f7b48`
* HEAD: `c2dbe9837b`
* State: local implementation checkpoint; no CI dispatch

The implementation is split into three reviewable commits: the bypass
bookkeeping fix (`96ceca6e3b`), the policy and integration (`ee4aedf618`),
and the admitted-fill regression coverage (`c2dbe9837b`).

This checkpoint adds a packet-aware `MockingjayL2RP` to each aligned L2 slice
in `configs/example/kmhv3.py`. The policy models sampled history, reuse-
distance prediction, signed ETR aging, scan/reuse training, replacement
statistics, and a conservative direct-response bypass for eligible clean
`ReadSharedReq` fills.

## Files to review

* `src/mem/cache/replacement_policies/mockingjay_l2_rp.{hh,cc}`: policy state,
  prediction/training, victim selection, and statistics.
* `src/mem/cache/replacement_policies/mockingjay_l2_rp.test.cc`: policy unit
  tests covering geometry, sampling, RDP behavior, aging, ties, and bypass.
* `src/mem/cache/base.{hh,cc}`: optional packet-aware victim selection and
  direct-response bypass plumbing. Bypassed fills do not allocate a temporary
  block or emit refill/Fill notifications.
* `src/mem/cache/tags/{base,base_set_assoc,vipt_set_assoc}.{hh,cc}` and
  `src/mem/cache/replacement_policies/{base,dueling_rp}.{hh,cc}`:
  backward-compatible packet propagation through existing interfaces.
* `configs/example/kmhv3.py`: geometry-derived policy parameters and one
  independent policy object per L2 slice.
* `src/mem/cache/cache.test.cc`: timing tests for clean bypass, dirty and
  `ReadCleanReq` exclusions, and a concurrent read snoop that forces normal
  allocation.

## Review focus

1. Confirm that the replacement-policy state is per slice and that no cache
   lookup, coherence, routing, or MSHR arbitration semantics were changed.
2. Check the bypass boundary in `BaseCache::recvTimingResp`: only one CPU
   demand `ReadSharedReq`, clean lower response, no writable/atomic/cache-
   maintenance attributes, and no pending downgrade/invalidation may bypass.
3. Check the train/update ordering: victim or bypass prediction happens before
   the miss is recorded; an admitted fill trains once in `reset`.
4. Check the RDP signature indexing and signed ETR tie-break rules against the
   implementation notes in `mockingjay_l2_implementation.md`.

## Validation recorded before this checkpoint

* `mockingjay_l2_rp.test.opt`: 10/10 tests passed.
* `cache.test.opt`: 5/5 timing tests passed.
* `python3 -m py_compile configs/example/kmhv3.py` passed.
* `git diff --check` passed.
* A one-million-instruction `omnetpp` checkpoint smoke completed with matching
  `simInsts` and `system.cpu.committedInsts` (`1000007`) and four constructed
  policies. The short smoke observed zero bypasses, so it is functional and
  configuration evidence, not a performance result.

The latest source correction reads the local bypass result value (rather than
the address of the optional flag), and the admitted-fill test exercises the
opposite path. Both focused binaries were rebuilt after that correction.

## Known limits

* The model is behavioral and reuses gem5's event-driven cache pipeline; it is
  not an RTL timing implementation.
* The local smoke uses the checkpoint-compatible `notama-tvalref` reference
  and local DDR4 timing. Neither substitutes for a matched CI performance A/B.
* The controlled GCC15 SPEC06 CI contract is documented in
  `mockingjay_l2_progress.md` and has not been dispatched.

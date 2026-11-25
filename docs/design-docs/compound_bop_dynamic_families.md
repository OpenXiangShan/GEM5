# Compound BOP: temporary offset families

The initial menus and Teacher scan order match `55107ce6a2`: 72 signed V
offsets, 37 signed P offsets. Both families nominate at a score of at least
2. Student V and P each have four slots and a separate 64-prediction delay
queue; both queues delay coverage visibility by 300 cycles. This delay
applies to scoring, not to sending a prefetch.

At a phase boundary the Student still compares the best resident of each
family. If the primary is enabled and the extra is rejected, regroup only
when **both** of these conditions hold (using the outgoing phase's counters):

- The extra by itself meets the 2% coverage threshold, but after subtracting
  overlap with the primary it covers less than 2% of all training demands.
  Exactly 2% marginal coverage still qualifies for issue.
- At least 80% of the extra's own coverage overlaps the primary, with at
  least one observed overlap. These are coverage-filter hits, not exact
  stream identities or a measurement of performance.

Apply at most one feedback move per phase, after aging earlier moves. The
extra joins the primary's effective family for the next nomination. Clear
residents whose physical partition no longer matches. They can re-enter only
by a later nomination; an evicted Student slot cannot be refilled with
the same offset at the move boundary. The Teacher applies these groups
to the scores it has already collected,
so a rejected offset cannot immediately win its old group again. The scan,
including the original priority prefix, does not change.

A move lasts twelve nominations including the one at the move boundary;
expiry does not require eviction, new feedback or a free Student slot.
Reverse feedback does not shorten the lease. No offset moves in exchange.
If expiry reunites two selected offsets, keep only the primary. Empty groups cannot
nominate. The state is one membership bit per Teacher offset plus a
twelve-entry ring of returned candidate IDs; the 4×4 cross-family overlap
table does not grow.

For example, if P's four highest-scoring offsets each prove redundant against
V's primary on successive phases, all four can briefly be in V together. The
fifth P offset can then win P if it meets the nomination threshold. It is
not guaranteed to issue: it must still pass Student selection and issue gates.

## Comparing runs

For `kmhv3.py`, enable `system.l2_wrappers[0].prefetcher.use_compound_bop=True`
via `--param`. To isolate feedback, run the **same commit and checkpoints**
with the additional parameter
`--param=system.l2_wrappers[0].prefetcher.compound_bop.regroup_hold_phases=0`.
This control keeps the original menus, common nomination threshold and both
delay queues. The default hold of 12 and overlap threshold of 80 are
experimental settings, not tuned results.

Check `regroupPToV`, `regroupVToP`, `regroupExpired`,
`regroupResidentInvalidations` and `regroupExtraCollisions` along with
the existing coverage and issue statistics. In a tracing-enabled build
the `CompoundBOP` debug flag reports the signed offsets involved.

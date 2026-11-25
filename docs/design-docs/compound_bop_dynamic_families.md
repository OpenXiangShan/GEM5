# Compound BOP: permanent random family exchanges

The default menus contain the same 109 signed offsets, in the same merged
Teacher scan order as 55107ce6a2. Move the 17 smallest-magnitude former V
entries (+32 and both signs of 36, 40, 45, 48, 50, 54, 60, 64) into P
at initialization: V starts with 55 entries, P with 54. Each family nominates
at a Teacher score of at least 2. Each Student partition has four slots and
a separate 64-entry queue delaying coverage visibility by 300 cycles.

At a phase boundary, consider the extra only when it could cover at least
2% of demands alone, its marginal coverage falls below 2%, and at least 80%
of its own coverage overlaps the primary's. Exactly 2% marginal passes for
issue and does not trigger an exchange. These are coverage-filter hits.

Exchange that extra with a **uniform random** offset from the primary's
current Teacher family, excluding only the primary. Use one gem5 RNG draw;
Teacher score, Student residency and prior exchanges do not affect eligibility.
If there is no eligible partner, leave both families unchanged. The exchange
is permanent until subsequent feedback moves either offset again. There is
no timer. Sizes stay constant; no additional overlap counters are required.

Kick the extra out of its Student slot. If the random partner was resident,
kick it out too. Neither carries its confidence or bitmap into another slot,
nor can a just-evicted offset re-enter on the same boundary. Teacher
nomination uses the new partition and the just-completed scores. A partner
that was not resident remains eligible for immediate admission. A later
nomination can re-admit either offset. The primary is protected from being
the swap partner.

With kmhv3.py enable the compound unit via
`--param=system.l2_wrappers[0].prefetcher.use_compound_bop=True`.
Set `system.l2_wrappers[0].prefetcher.compound_bop.regroup_enable=False`
with a second `--param` for the fixed-55/54 control. Compare using the same
checkpoints and random seed. See `regroupPToV` / `regroupVToP` for feedback
direction (one count per exchange), `regroupNoPartner` for skipped exchanges,
and `regroupResidentPartners` for the number that also evicted the partner.

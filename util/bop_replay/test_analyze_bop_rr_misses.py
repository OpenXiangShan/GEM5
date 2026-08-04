#!/usr/bin/env python3
"""Focused unit tests for RR miss attribution shadow state."""

import unittest

from analyze_bop_rr_misses import RRMissAnalyzer, RRShadow
from bop_replay import BOPConfig, LearnerOutput, ReplayDelayAction, ReplayEvent, UINT64_MASK


def event(access_seq: int, addr: int, replay_order: int) -> ReplayEvent:
    return ReplayEvent(
        access_seq=access_seq,
        order=access_seq,
        bop_name="system.l2.bop_large",
        bop_kind="large",
        tick=access_seq * 10,
        trigger_addr=addr,
        trigger_pc=0x1000,
        trigger_has_pc=True,
        validation_hit=-1,
        best_offset_changed=False,
        issue_enabled=False,
        validation_enabled=False,
        pc_confidence_enabled=False,
        pc_sampled=False,
        raw_candidate_valid=False,
        raw_candidate_addr=0,
        policy_candidate_valid=False,
        policy_candidate_addr=0,
        replay_order=replay_order,
    )


class RRShadowTest(unittest.TestCase):
    def test_conflicting_insert_replaces_prior_target(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large",
            block_size=64,
            rr_entries=4,
            tag_bits=4,
            delay_queue_enabled=False,
        )
        shadow = RRShadow(config)
        target = 4 * 64
        evictor = 11 * 64
        self.assertEqual(shadow.hash(target), shadow.hash(evictor))
        self.assertNotEqual(shadow.tag(target), shadow.tag(evictor))

        shadow.apply_trigger(event(1, target, 1), None)
        shadow.apply_trigger(event(2, evictor, 2), None)

        self.assertFalse(shadow.hit(target))
        self.assertEqual(
            shadow.miss_cause(target, {target}), "conflict_replaced"
        )
        record = shadow.conflict_record(target)
        self.assertEqual(record.evictor_line, evictor)
        self.assertEqual(record.eviction_ordinal, 2)

    def test_pending_and_drop_full_are_not_capacity_replacements(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large",
            block_size=64,
            rr_entries=4,
            tag_bits=4,
            delay_queue_enabled=True,
            delay_queue_size=1,
        )
        shadow = RRShadow(config)
        pending = 4 * 64
        dropped = 11 * 64
        shadow.apply_trigger(
            event(1, pending, 1),
            ReplayDelayAction(
                "system.l2.bop_large", 1, "enqueue", 10, pending, 20, 1
            ),
        )
        self.assertEqual(
            shadow.miss_cause(pending, {pending}), "delay_pending"
        )
        shadow.apply_trigger(
            event(2, dropped, 2),
            ReplayDelayAction(
                "system.l2.bop_large", 2, "drop_full", 20, dropped, 30, 1
            ),
        )
        self.assertEqual(
            shadow.miss_cause(dropped, {dropped}), "delay_drop_full"
        )
        self.assertEqual(shadow.slot_replacements, 0)

    def test_no_prior_demand_is_distinct_from_prior_demand(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large",
            block_size=64,
            rr_entries=4,
            tag_bits=4,
            delay_queue_enabled=False,
        )
        shadow = RRShadow(config)
        prior_demand = 4 * 64
        no_demand = 11 * 64
        self.assertEqual(
            shadow.miss_cause(prior_demand, {prior_demand}),
            "prior_demand_no_rr_insert",
        )
        self.assertEqual(
            shadow.miss_cause(no_demand, set()), "no_prior_demand"
        )

    def test_validation_address_uses_bop_u64_wraparound(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64,
            rr_entries=4, tag_bits=4, delay_queue_enabled=False,
        )
        source = event(1, 0, 1)
        output = LearnerOutput(
            source, "large", 1, 1, 0, 0, False, True, True, 64, True, 0
        )
        analyzer = RRMissAnalyzer(config, top=1)
        self.assertEqual(
            analyzer._validation_addr(output), UINT64_MASK - 63
        )


if __name__ == "__main__":
    unittest.main()

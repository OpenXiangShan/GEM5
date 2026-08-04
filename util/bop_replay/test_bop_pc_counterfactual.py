#!/usr/bin/env python3
"""Focused tests for BOP PC-validation RR counterfactual evidence."""

from __future__ import annotations

import unittest

from bop_replay import BOPConfig, PCValidationController, ReplayDelayAction, ReplayEvent
from replay_bop_pc_counterfactual import (
    _NoDropRREvidence,
    _PersistentRREvidence,
    _UniqueAddressLRUEvidence,
    _no_conflict_validation,
    _point_config,
    _unique_lru_validation,
    _unique_lru_offset_match_validation,
)


def event(access_seq: int, tick: int, addr: int) -> ReplayEvent:
    return ReplayEvent(
        access_seq=access_seq,
        order=access_seq,
        bop_name="system.l2.bop_large",
        bop_kind="large",
        tick=tick,
        trigger_addr=addr,
        trigger_pc=0x1000,
        trigger_has_pc=True,
        validation_hit=0,
        best_offset_changed=False,
        issue_enabled=True,
        validation_enabled=True,
        pc_confidence_enabled=True,
        pc_sampled=False,
        raw_candidate_valid=True,
        raw_candidate_addr=addr + 64,
        policy_candidate_valid=False,
        policy_candidate_addr=0,
        replay_order=access_seq,
    )


class CounterfactualEvidenceTest(unittest.TestCase):
    def test_unique_lru_evicts_only_the_least_recent_unique_line(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        first = ReplayEvent(**{
            **event(1, 1, 4 * 64).__dict__, "trigger_pc": 0x400,
        })
        second = ReplayEvent(**{
            **event(2, 2, 8 * 64).__dict__, "trigger_pc": 0x500,
        })
        refresh = ReplayEvent(**{
            **event(3, 3, 4 * 64).__dict__, "trigger_pc": 0x600,
        })
        third = event(4, 4, 12 * 64)
        evidence = _UniqueAddressLRUEvidence(config, capacity=2)

        evidence.apply_trigger(first, None)
        evidence.apply_trigger(second, None)
        evidence.apply_trigger(refresh, None)
        evidence.apply_trigger(third, None)

        hit, source, owner_pc, owner_valid = evidence.hit(first.trigger_addr)
        self.assertTrue(hit)
        self.assertEqual(source, "unique_lru")
        self.assertEqual(owner_pc, refresh.trigger_pc)
        self.assertTrue(owner_valid)
        self.assertFalse(evidence.hit(second.trigger_addr)[0])
        self.assertTrue(evidence.hit(third.trigger_addr)[0])
        self.assertEqual(evidence.stats()["duplicate_refreshes"], 1)
        self.assertEqual(evidence.stats()["capacity_evictions"], 1)

    def test_unique_lru_keeps_last_valid_owner_across_no_pc_refresh(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        producer = ReplayEvent(**{
            **event(1, 1, 4 * 64).__dict__, "trigger_pc": 0x400,
        })
        no_pc = ReplayEvent(**{
            **event(2, 2, 4 * 64).__dict__, "trigger_pc": 0,
            "trigger_has_pc": False,
        })
        evidence = _UniqueAddressLRUEvidence(config, capacity=2)
        evidence.apply_trigger(producer, None)
        evidence.apply_trigger(no_pc, None)

        hit, _, owner_pc, owner_valid = evidence.hit(producer.trigger_addr)
        self.assertTrue(hit)
        self.assertEqual(owner_pc, producer.trigger_pc)
        self.assertTrue(owner_valid)

    def test_unique_lru_admits_lines_only_after_native_delay_maturity(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=True, delay_queue_size=1,
            delay_ticks=10, clock_period_ticks=1,
        )
        target = 4 * 64
        evidence = _UniqueAddressLRUEvidence(config, capacity=2)
        evidence.apply_trigger(
            event(1, 10, target),
            ReplayDelayAction("system.l2.bop_large", 1, "enqueue", 10,
                              target, 20, 1),
        )
        self.assertFalse(evidence.hit(target)[0])
        evidence.apply_dequeue(
            ReplayDelayAction("system.l2.bop_large", 2, "dequeue_to_rr", 20,
                              target, 20, 0)
        )
        self.assertTrue(evidence.hit(target)[0])

    def test_unique_lru_preserves_a_recorded_native_hit_and_owner(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        evidence = _UniqueAddressLRUEvidence(config, capacity=2)
        evidence.apply_trigger(event(1, 1, 4 * 64), None)
        native_hit_event = ReplayEvent(**{
            **event(2, 2, 8 * 64).__dict__, "validation_hit": 1,
        })

        hit, source, owner_pc, owner_valid = _unique_lru_validation(
            native_hit_event, evidence, 12 * 64,
            native_owner_pc=0x777, native_owner_valid=True,
        )
        self.assertTrue(hit)
        self.assertEqual(source, "recorded")
        self.assertEqual(owner_pc, 0x777)
        self.assertTrue(owner_valid)

    def test_unique_lru_recovery_credits_its_recorded_producer(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=1,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        producer = ReplayEvent(**{
            **event(1, 1, 4 * 64).__dict__, "trigger_pc": 0x400,
        })
        consumer = ReplayEvent(**{
            **event(2, 2, 11 * 64).__dict__, "trigger_pc": 0x500,
        })
        evidence = _UniqueAddressLRUEvidence(config, capacity=2)
        evidence.apply_trigger(producer, None)
        hit, source, owner_pc, owner_valid = _unique_lru_validation(
            consumer, evidence, producer.trigger_addr,
        )
        self.assertTrue(hit)
        self.assertEqual(source, "unique_lru")

        controller = PCValidationController(config)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=int(hit),
            trigger_addr=consumer.trigger_addr, trigger_pc=consumer.trigger_pc,
            trigger_has_pc=True, validation_owner_pc=owner_pc,
            validation_owner_valid=owner_valid,
        )
        controller.commit()
        self.assertEqual(controller.stats()["rr_owner_cross_pc_hits"], 1)
        self.assertEqual(controller.lookup(producer.trigger_pc, "large", 1).state, "high")

    def test_unique_lru_offset_match_requires_the_historical_offset(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        producer = ReplayEvent(**{
            **event(1, 1, 4 * 64).__dict__,
            "trigger_pc": 0x400,
            "best_offset_after": 2,
        })
        consumer = ReplayEvent(**{
            **event(2, 2, 11 * 64).__dict__,
            "trigger_pc": 0x500,
            "best_offset_after": 1,
        })
        evidence = _UniqueAddressLRUEvidence(config, capacity=2)
        evidence.apply_trigger(producer, None)

        hit, source, owner_pc, owner_valid = _unique_lru_offset_match_validation(
            consumer, evidence, producer.trigger_addr,
        )
        self.assertFalse(hit)
        self.assertEqual(source, "unique_lru_offset_mismatch")
        self.assertEqual(owner_pc, producer.trigger_pc)
        self.assertTrue(owner_valid)

        matched_consumer = ReplayEvent(**{
            **consumer.__dict__, "best_offset_after": 2,
        })
        hit, source, owner_pc, owner_valid = _unique_lru_offset_match_validation(
            matched_consumer, evidence, producer.trigger_addr,
        )
        self.assertTrue(hit)
        self.assertEqual(source, "unique_lru_offset_match")
        self.assertEqual(owner_pc, producer.trigger_pc)
        self.assertTrue(owner_valid)

    def test_compatible_recovered_cross_pc_credit_is_weak(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=1,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_recovered_hit_increment=1,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        controller = PCValidationController(config)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0x500, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
            validation_source="recovered",
        )
        controller.commit()
        producer = controller.lookup(0x400, "large", 1)
        self.assertEqual(producer.confidence, 2)
        self.assertEqual(producer.state, "medium")
        self.assertEqual(controller.stats()["recovered_producer_credits"], 1)

    def test_direct_only_recovered_same_pc_admits_without_credit(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
            pc_validation_recovered_producer_credit=False,
        )
        controller = PCValidationController(config)
        issued = controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
            validation_source="recovered",
        )
        controller.commit()

        self.assertTrue(issued)
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 0)
        self.assertEqual(controller.stats()["recovered_producer_credits"], 0)
        self.assertEqual(
            controller.stats()["recovered_producer_credit_suppressed"], 1
        )

    def test_credit_only_recovered_same_pc_uses_miss_admission_then_credit(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_recovered_hit_increment=1,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
            pc_validation_recovered_admission=False,
        )
        controller = PCValidationController(config)
        issued = controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
            validation_source="recovered",
        )
        controller.commit()

        self.assertFalse(issued)
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 1)
        self.assertEqual(controller.stats()["consumer_miss_updates"], 1)
        self.assertEqual(controller.stats()["recovered_producer_credits"], 1)

    def test_credit_only_does_not_bypass_invalid_owner_or_no_pc_admission(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
            pc_validation_recovered_admission=False,
        )
        invalid_owner = PCValidationController(config)
        self.assertFalse(invalid_owner.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0x500, trigger_has_pc=True,
            validation_owner_pc=0, validation_owner_valid=False,
            validation_source="recovered",
        ))
        invalid_owner.commit()
        self.assertEqual(invalid_owner.stats()["consumer_miss_updates"], 1)

        no_trigger_pc = PCValidationController(config)
        self.assertFalse(no_trigger_pc.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0, trigger_has_pc=False,
            validation_owner_pc=0x400, validation_owner_valid=True,
            validation_source="recovered",
        ))
        no_trigger_pc.commit()
        self.assertEqual(no_trigger_pc.stats()["recovered_producer_credits"], 1)

    def test_cross_pc_credit_off_preserves_consumer_path_without_owner_reward(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
            pc_validation_recovered_cross_pc_credit=False,
        )
        controller = PCValidationController(config)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0x500, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
            validation_source="recovered",
        )
        controller.commit()

        self.assertEqual(controller.stats()["rr_owner_cross_pc_hits"], 1)
        self.assertEqual(controller.stats()["consumer_miss_updates"], 1)
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 0)
        self.assertEqual(controller.stats()["recovered_producer_credits"], 0)
        self.assertEqual(
            controller.stats()["recovered_producer_credit_suppressed"], 1
        )

    def test_same_and_cross_only_profiles_select_their_credit_relation(self):
        base = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_recovered_hit_increment=1,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        full = _point_config("lru_full", base)
        direct = _point_config("direct_only", base)
        credit = _point_config("credit_only", base)
        same_only = _point_config("same_pc_credit_only", base)
        cross_only = _point_config("cross_pc_credit_only", base)

        self.assertTrue(full.pc_validation_recovered_admission)
        self.assertTrue(full.pc_validation_recovered_producer_credit)
        self.assertTrue(full.pc_validation_recovered_same_pc_credit)
        self.assertTrue(full.pc_validation_recovered_cross_pc_credit)
        self.assertTrue(direct.pc_validation_recovered_admission)
        self.assertFalse(direct.pc_validation_recovered_producer_credit)
        self.assertFalse(credit.pc_validation_recovered_admission)
        self.assertTrue(credit.pc_validation_recovered_producer_credit)
        self.assertTrue(same_only.pc_validation_recovered_same_pc_credit)
        self.assertFalse(same_only.pc_validation_recovered_cross_pc_credit)
        self.assertFalse(cross_only.pc_validation_recovered_same_pc_credit)
        self.assertTrue(cross_only.pc_validation_recovered_cross_pc_credit)

        def replay_one(config: BOPConfig, trigger_pc: int, owner_pc: int) -> int:
            controller = PCValidationController(config)
            controller.policy_candidate_values(
                bop_kind="large", best_offset=1, best_offset_changed=False,
                raw_candidate_valid=True, pc_confidence_enabled=True,
                validation_enabled=True, validation_hit=1,
                trigger_addr=11 * 64, trigger_pc=trigger_pc,
                trigger_has_pc=True, validation_owner_pc=owner_pc,
                validation_owner_valid=True, validation_source="recovered",
            )
            controller.commit()
            return controller.lookup(owner_pc, "large", 1).confidence

        self.assertEqual(replay_one(same_only, 0x400, 0x400), 1)
        self.assertEqual(replay_one(cross_only, 0x400, 0x400), 0)
        self.assertEqual(replay_one(same_only, 0x500, 0x400), 0)
        self.assertEqual(replay_one(cross_only, 0x500, 0x400), 1)

    def test_recovered_offset_mismatch_uses_consumer_miss_path(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=1,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        controller = PCValidationController(config)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=0,
            trigger_addr=11 * 64, trigger_pc=0x500, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
        )
        controller.commit()
        self.assertEqual(controller.stats()["rr_owner_cross_pc_hits"], 0)
        self.assertEqual(controller.stats()["producer_hit_updates"], 0)
        self.assertEqual(controller.stats()["consumer_miss_updates"], 1)
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 1)

    def test_recovered_same_pc_gate_uses_pre_update_confidence(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_recovered_hit_increment=1,
            pc_validation_recovered_same_pc_hit_gate=True,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        controller = PCValidationController(config)
        issued = controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=11 * 64, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
            validation_source="recovered",
        )
        controller.commit()
        self.assertFalse(issued)
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 1)
        self.assertEqual(controller.stats()["same_pc_hit_gate_suppressed"], 1)

    def test_recovered_probation_requires_two_compatible_hits(self):
        config = BOPConfig(
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=0,
            pc_validation_medium_threshold=0,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_recovered_hit_increment=1,
            pc_validation_recovered_probation=True,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        controller = PCValidationController(config)
        values = {
            "bop_kind": "large", "best_offset": 1,
            "best_offset_changed": False, "raw_candidate_valid": True,
            "pc_confidence_enabled": True, "validation_enabled": True,
            "validation_hit": 1, "trigger_addr": 11 * 64,
            "trigger_pc": 0x400, "trigger_has_pc": True,
            "validation_owner_pc": 0x400, "validation_owner_valid": True,
            "validation_source": "recovered",
        }
        controller.policy_candidate_values(**values)
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 0)
        controller.policy_candidate_values(**values)
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 1)
        self.assertEqual(controller.stats()["recovered_probation_armed"], 1)
        self.assertEqual(controller.stats()["recovered_probation_confirmed"], 1)

    def test_no_conflict_retains_matured_line_after_direct_map_conflict(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        target = 4 * 64
        evictor = 11 * 64
        persistent = _PersistentRREvidence(config)
        direct = _NoDropRREvidence(config, persistent=False)

        for sequence, address in ((1, target), (2, evictor)):
            trigger = event(sequence, sequence, address)
            persistent.apply_trigger(trigger, None)
            direct.apply_trigger(trigger, False)

        self.assertTrue(persistent.hit(target)[0])
        self.assertFalse(direct.hit(target)[0])
        self.assertEqual(direct.stats()["direct_replacements"], 1)

    def test_no_delay_drop_preserves_delay_and_same_tick_ordering(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=True, delay_queue_size=1,
            delay_ticks=10, clock_period_ticks=1,
        )
        evidence = _NoDropRREvidence(config, persistent=False)
        target = 4 * 64

        evidence.apply_trigger(event(1, 10, target), True)
        self.assertFalse(evidence.hit(target)[0])
        # A callback due at tick 20 runs after a trigger at tick 20.
        evidence.apply_trigger(event(2, 20, 12 * 64), False)
        self.assertFalse(evidence.hit(target)[0])
        evidence.apply_trigger(event(3, 21, 13 * 64), False)
        self.assertTrue(evidence.hit(target)[0])

    def test_combined_mode_marks_recovery_from_a_historical_delay_drop(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=True, delay_queue_size=1,
            delay_ticks=2, clock_period_ticks=1,
        )
        evidence = _NoDropRREvidence(config, persistent=True)
        target = 4 * 64

        evidence.apply_trigger(event(1, 10, target), True)
        evidence.apply_trigger(event(2, 13, 12 * 64), False)
        hit, source, _, _ = evidence.hit(target)
        self.assertTrue(hit)
        self.assertEqual(source, "delay_drop")

    def test_tag_zero_alias_remains_a_hit_without_an_insert(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        evidence = _PersistentRREvidence(config)
        self.assertTrue(evidence.hit(0)[0])

    def test_exact_line_evidence_does_not_model_nonzero_tag_aliases(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=1, delay_queue_enabled=False,
        )
        evidence = _PersistentRREvidence(config)
        inserted = 4 * 64
        alias = 12 * 64
        evidence.apply_trigger(event(1, 1, inserted), None)
        self.assertFalse(evidence.hit(alias)[0])

    def test_no_conflict_recovery_credits_the_matured_producer(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=1,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        producer = event(1, 1, 4 * 64)
        producer = ReplayEvent(**{**producer.__dict__, "trigger_pc": 0x400})
        consumer = event(2, 2, 11 * 64)
        consumer = ReplayEvent(**{**consumer.__dict__, "trigger_pc": 0x500})
        evidence = _PersistentRREvidence(config)
        evidence.apply_trigger(producer, None)
        evidence.apply_trigger(consumer, None)

        hit, source, owner_pc, owner_valid = evidence.hit(producer.trigger_addr)
        self.assertTrue(hit)
        self.assertEqual(source, "matured_recorded")
        self.assertEqual(owner_pc, producer.trigger_pc)
        self.assertTrue(owner_valid)

        controller = PCValidationController(config)
        self.assertTrue(controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=int(hit),
            trigger_addr=consumer.trigger_addr, trigger_pc=consumer.trigger_pc,
            trigger_has_pc=True, validation_owner_pc=owner_pc,
            validation_owner_valid=owner_valid,
        ))
        controller.commit()
        self.assertEqual(controller.stats()["rr_owner_cross_pc_hits"], 1)
        self.assertEqual(controller.lookup(producer.trigger_pc, "large", 1).state, "high")
        self.assertEqual(controller.lookup(consumer.trigger_pc, "large", 1).state, "low")

    def test_no_conflict_age_limit_uses_latest_native_maturity(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        target = 4 * 64
        evidence = _PersistentRREvidence(config)
        evidence.apply_trigger(event(1, 1, target), None)

        self.assertEqual(evidence.stats()["mature_insertions"], 1)
        self.assertTrue(evidence.hit(target, max_insert_age=0)[0])

        evidence.apply_trigger(event(2, 2, 8 * 64), None)
        hit, source, _, _ = evidence.hit(target, max_insert_age=0)
        self.assertFalse(hit)
        self.assertEqual(source, "stale_matured")
        self.assertTrue(evidence.hit(target, max_insert_age=1)[0])

        # A later native maturity of the same target refreshes its age.
        evidence.apply_trigger(event(3, 3, target), None)
        self.assertTrue(evidence.hit(target, max_insert_age=0)[0])

    def test_age_limited_cross_pc_hit_keeps_historical_producer_credit(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=1,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )
        producer = ReplayEvent(**{
            **event(1, 1, 4 * 64).__dict__, "trigger_pc": 0x400,
        })
        consumer = ReplayEvent(**{
            **event(2, 2, 11 * 64).__dict__, "trigger_pc": 0x500,
        })
        evidence = _PersistentRREvidence(config)
        evidence.apply_trigger(producer, None)

        hit, source, owner_pc, owner_valid = _no_conflict_validation(
            consumer, evidence, producer.trigger_addr, 0,
        )
        self.assertTrue(hit)
        self.assertEqual(source, "matured_recorded")

        controller = PCValidationController(config)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=int(hit),
            trigger_addr=consumer.trigger_addr, trigger_pc=consumer.trigger_pc,
            trigger_has_pc=True, validation_owner_pc=owner_pc,
            validation_owner_valid=owner_valid,
        )
        controller.commit()
        self.assertEqual(controller.stats()["rr_owner_cross_pc_hits"], 1)
        self.assertEqual(controller.lookup(producer.trigger_pc, "large", 1).state, "high")

    def test_recorded_hit_bypasses_age_gate_and_keeps_native_owner(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large", block_size=64, rr_entries=4,
            tag_bits=4, delay_queue_enabled=False,
        )
        target = 4 * 64
        evidence = _PersistentRREvidence(config)
        evidence.apply_trigger(event(1, 1, target), None)
        evidence.apply_trigger(event(2, 2, 8 * 64), None)
        native_hit_event = ReplayEvent(**{
            **event(3, 3, 12 * 64).__dict__, "validation_hit": 1,
        })

        hit, source, owner_pc, owner_valid = _no_conflict_validation(
            native_hit_event, evidence, target, 0,
            native_owner_pc=0x777, native_owner_valid=True,
        )
        self.assertTrue(hit)
        self.assertEqual(source, "recorded")
        self.assertEqual(owner_pc, 0x777)
        self.assertTrue(owner_valid)


if __name__ == "__main__":
    unittest.main()

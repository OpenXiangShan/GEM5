#!/usr/bin/env python3
"""Focused tests for issuer-PC raw/P/C quality attribution."""

from __future__ import annotations

import unittest

from bop_replay import BOPConfig, Demand
from analyze_bop_pc_quality import (
    _IssuerKey,
    _PCQualityTracker,
    _TracingPCValidationController,
    _coverage_transition,
)


class PCQualityTrackerTest(unittest.TestCase):
    def test_combined_labels_are_owned_by_the_issuer_and_duplicates_redundant(self):
        tracker = _PCQualityTracker(horizon=2)
        first = _IssuerKey(True, 0x100, "large")
        duplicate = _IssuerKey(True, 0x200, "large")
        unused = _IssuerKey(True, 0x300, "large")

        tracker.emit(first, 1, 10, 0x1000, 1)
        tracker.emit(duplicate, 2, 20, 0x1000, 1)
        tracker.observe_demand(Demand(3, 30, 0x1000, 1))
        tracker.emit(unused, 4, 40, 0x2000, 1)
        tracker.observe_demand(Demand(5, 50, 0x3000, 1))
        tracker.observe_demand(Demand(6, 60, 0x3000, 1))
        tracker.observe_demand(Demand(7, 70, 0x3000, 1))
        tracker.finish()

        combined = tracker.views["combined"]
        self.assertEqual(combined.by_key[first].useful, 1)
        self.assertEqual(combined.by_key[duplicate].redundant, 1)
        self.assertEqual(combined.by_key[unused].unused, 1)
        self.assertEqual(combined.demand_owners, {3: first})
        self.assertEqual(
            tracker.aggregate_report()["combined"]["candidates"], 3,
        )

    def test_coverage_transition_attributes_only_lost_and_gained_demands(self):
        raw = _PCQualityTracker(horizon=1)
        current = _PCQualityTracker(horizon=1)
        raw_owner = _IssuerKey(True, 0x100, "large")
        current_owner = _IssuerKey(True, 0x200, "small")
        changed_owner = _IssuerKey(True, 0x300, "large")
        raw.views["combined"].demand_owners.update({
            10: raw_owner,
            11: raw_owner,
        })
        current.views["combined"].demand_owners.update({
            11: changed_owner,
            12: current_owner,
        })

        transition = _coverage_transition(raw, current)
        self.assertEqual(transition["raw_only_demands"], 1)
        self.assertEqual(transition["current_only_demands"], 1)
        self.assertEqual(transition["both_covered_demands"], 1)
        self.assertEqual(transition["ownership_changed_demands"], 1)
        self.assertEqual(
            transition["raw_only_by_issuer_pc_kind"][0]["issuer_trigger_pc"],
            "0x100",
        )
        self.assertEqual(
            transition["current_only_by_issuer_pc_kind"][0]["issuer_trigger_pc"],
            "0x200",
        )


class TracingControllerTest(unittest.TestCase):
    def _config(self, initial: int, same_pc_hit_gate: bool = False) -> BOPConfig:
        return BOPConfig(
            bop_name="system.l2.bop_large",
            block_size=64,
            pc_validation_confidence=True,
            pc_validation_producer_consumer=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=3,
            pc_validation_initial=initial,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=3,
            pc_validation_hit_increment=2,
            pc_validation_same_pc_hit_gate=same_pc_hit_gate,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
        )

    def test_cross_owner_hit_reports_current_consumer_admission_reason(self):
        controller = _TracingPCValidationController(self._config(initial=1))
        issued = controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x1000, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x500, validation_owner_valid=True,
        )
        self.assertTrue(issued)
        self.assertEqual(controller.last_decision.validation, "hit")
        self.assertEqual(controller.last_decision.rr_owner_relation, "cross_key")
        self.assertEqual(controller.last_decision.admission_reason, "medium_sampled")

    def test_validation_miss_reports_low_state_suppression(self):
        controller = _TracingPCValidationController(self._config(initial=0))
        issued = controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=0,
            trigger_addr=0x1000, trigger_pc=0x400, trigger_has_pc=True,
        )
        self.assertFalse(issued)
        self.assertEqual(controller.last_decision.validation, "miss")
        self.assertEqual(controller.last_decision.admission_reason, "low_suppressed")

    def test_same_owner_gated_hit_reports_same_key_admission_reason(self):
        controller = _TracingPCValidationController(
            self._config(initial=0, same_pc_hit_gate=True)
        )
        issued = controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x1000, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
        )
        self.assertFalse(issued)
        self.assertEqual(controller.last_decision.rr_owner_relation, "same_key")
        self.assertEqual(
            controller.last_decision.admission_reason, "same_key_low_suppressed",
        )


if __name__ == "__main__":
    unittest.main()

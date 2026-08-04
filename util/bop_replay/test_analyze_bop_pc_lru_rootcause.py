#!/usr/bin/env python3
"""Unit tests for bounded LRU P/C root-cause attribution."""

from __future__ import annotations

import unittest

from analyze_bop_pc_lru_rootcause import _CausalAccumulator, _decision_cause
from bop_replay import (
    BOPConfig,
    Candidate,
    ControllerDecision,
    PCValidationController,
    ReplayEvent,
)


def decision(
    *, issued: bool, source: str = "native", native_hit: bool = False,
    issuer_confidence: int | None = 0, issuer_state: str | None = "low",
    bypass: bool = False, recovered_cross_seen: bool = False,
    recovered_same_seen: bool = False,
) -> ControllerDecision:
    return ControllerDecision(
        issued=issued,
        reason="high_confidence" if issued else "low_confidence",
        validation_hit=native_hit,
        validation_source=source,
        owner_relation="none",
        issuer_confidence=issuer_confidence,
        issuer_state=issuer_state,
        owner_confidence=None,
        owner_state=None,
        global_bypass_before=bypass,
        global_bypass_at_admission=bypass,
        issuer_recovered_same_pc_credit_seen=recovered_same_seen,
        issuer_recovered_cross_pc_credit_seen=recovered_cross_seen,
        owner_recovered_same_pc_credit_seen=False,
        owner_recovered_cross_pc_credit_seen=False,
    )


def event(sequence: int = 10) -> ReplayEvent:
    return ReplayEvent(
        access_seq=sequence,
        order=sequence,
        bop_name="system.l2.bop_large",
        bop_kind="large",
        tick=sequence,
        trigger_addr=0x1000,
        trigger_pc=0x400,
        trigger_has_pc=True,
        validation_hit=0,
        best_offset_changed=False,
        issue_enabled=True,
        validation_enabled=True,
        pc_confidence_enabled=True,
        pc_sampled=False,
        raw_candidate_valid=True,
        raw_candidate_addr=0x1040,
        policy_candidate_valid=False,
        policy_candidate_addr=0,
        replay_order=sequence,
    )


def candidate(candidate_id: int) -> Candidate:
    return Candidate(
        candidate_id=candidate_id,
        kind="large",
        access_seq=10,
        tick=10,
        addr=0x1040,
        demand_index_at_issue=0,
    )


class RootCauseTest(unittest.TestCase):
    def test_decision_credit_snapshot_precedes_current_recovered_update(self):
        config = BOPConfig(
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
        controller = PCValidationController(config)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x1000, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x500, validation_owner_valid=True,
            validation_source="recovered",
        )
        assert controller.last_decision is not None
        self.assertFalse(
            controller.last_decision.issuer_recovered_cross_pc_credit_seen
        )
        controller.commit()
        owner_lookup = controller.lookup(0x500, "large", 1)
        self.assertTrue(owner_lookup.recovered_cross_pc_credit_seen)
        controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=0,
            trigger_addr=0x1040, trigger_pc=0x400, trigger_has_pc=True,
        )
        assert controller.last_decision is not None
        self.assertFalse(
            controller.last_decision.issuer_recovered_cross_pc_credit_seen
        )

    def test_direct_recovered_has_priority_only_on_current_evidence(self):
        native = decision(issued=False)
        lru = decision(issued=True, source="recovered", native_hit=True)
        self.assertEqual(
            _decision_cause(native, lru), "direct_recovered_evidence"
        )

    def test_global_bypass_precedes_confidence_state_difference(self):
        native = decision(issued=False, issuer_confidence=0, bypass=False)
        lru = decision(issued=True, issuer_confidence=10, issuer_state="high", bypass=True)
        self.assertEqual(
            _decision_cause(native, lru), "global_bypass_amplification"
        )

    def test_confidence_state_divergence_requires_no_current_recovery(self):
        native = decision(issued=False, issuer_confidence=0)
        lru = decision(
            issued=True, issuer_confidence=10, issuer_state="high",
            recovered_cross_seen=True,
        )
        self.assertEqual(
            _decision_cause(native, lru), "confidence_state_divergence"
        )

    def test_lru_only_resolution_closes_and_records_prior_cross_pc_credit(self):
        tracker = _CausalAccumulator(top_pcs=1)
        native = decision(issued=False)
        lru = decision(
            issued=True, issuer_confidence=10, issuer_state="high",
            recovered_cross_seen=True,
        )
        tracked = candidate(1)
        tracker.register(
            event=event(), native_candidate=None, lru_candidate=tracked,
            native=native, lru=lru, validation_owner_pc=0,
        )
        tracked.status = "useful"
        tracker.resolve("unique_lru", tracked)

        report = tracker.report()
        self.assertEqual(
            report["unique_lru_only_by_cause"]["confidence_state_divergence"][
                "issued"
            ],
            1,
        )
        row = report["unique_lru_only_breakdown"][0]
        self.assertEqual(row["prior_recovered_relation"], "cross_pc")
        self.assertEqual(row["useful"], 1)


if __name__ == "__main__":
    unittest.main()

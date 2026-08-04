#!/usr/bin/env python3
"""Tests for the directed no-conflict P/C confidence experiment."""

from __future__ import annotations

import unittest

from bop_replay import BOPConfig, PCValidationController
from sweep_bop_pc_confidence import (
    POLICY_FIELDS,
    UPDATE_FIELDS,
    _cohort_report,
    _quality_with_coverage,
    load_experiment_payload,
)


def _pc_row(
    pc: str | None, raw: dict[str, object], current: dict[str, object],
) -> dict[str, object]:
    return {
        "issuer_has_pc": pc is not None,
        "issuer_trigger_pc": pc,
        "raw": raw,
        "current": current,
    }


def _counts(
    candidates: int, useful: int, unused: int,
) -> dict[str, object]:
    denominator = useful + unused
    return {
        "candidates": candidates,
        "useful": useful,
        "unused": unused,
        "redundant": 0,
        "censored": 0,
        "eligible_demands": 100,
        "covered_demands": useful,
        "accuracy": useful / denominator if denominator else None,
        "coverage_contribution": useful / 100,
    }


class ConfidenceExperimentTest(unittest.TestCase):
    def _trace_config(self) -> BOPConfig:
        return BOPConfig(
            bop_name="system.l2.bop_large",
            pc_validation_confidence=True,
            pc_validation_producer_consumer=False,
            pc_validation_entries=128,
            pc_validation_tag_bits=10,
            pc_validation_counter_bits=5,
            pc_validation_initial=16,
            pc_validation_medium_threshold=8,
            pc_validation_high_threshold=20,
            pc_validation_hit_increment=4,
            pc_validation_medium_sample_period=4,
            pc_validation_miss_decay_period=4,
            pc_validation_offset_context_slots=2,
            global_coverage_guard=True,
        )

    def _payload(self) -> dict[str, object]:
        return {
            "name": "unit",
            "horizon": 2048,
            "no_conflict_max_insert_age": 2048,
            "raw_pc_accuracy_threshold": 0.1,
            "base_controller_overrides": {
                "pc_validation_entries": 128,
                "pc_validation_offset_context_slots": 2,
                "pc_validation_producer_consumer": True,
            },
            "profiles": [
                {
                    "name": "baseline",
                    "overrides": {
                        "pc_validation_hit_increment": 4,
                        "pc_validation_miss_decay_period": 4,
                        "pc_validation_low_entry_miss_streak_threshold": 0,
                    },
                },
                {
                    "name": "hysteresis",
                    "overrides": {
                        "pc_validation_hit_increment": 4,
                        "pc_validation_miss_decay_period": 4,
                        "pc_validation_low_entry_miss_streak_threshold": 4,
                    },
                },
            ],
        }

    def test_profile_loader_keeps_all_non_update_parameters_fixed(self):
        experiment = load_experiment_payload(self._payload(), self._trace_config())
        self.assertEqual(experiment.no_conflict_max_insert_age, 2048)
        self.assertEqual(experiment.raw_pc_accuracy_threshold, 0.1)
        self.assertEqual([profile.name for profile in experiment.profiles], [
            "baseline", "hysteresis",
        ])
        baseline, hysteresis = experiment.profiles
        self.assertEqual(baseline.config.pc_validation_entries, 128)
        self.assertEqual(hysteresis.config.pc_validation_entries, 128)
        self.assertTrue(hysteresis.config.pc_validation_producer_consumer)
        self.assertEqual(hysteresis.config.pc_validation_hit_increment, 4)
        self.assertEqual(
            hysteresis.config.pc_validation_low_entry_miss_streak_threshold, 4,
        )

    def test_profile_loader_rejects_non_update_override(self):
        payload = self._payload()
        payload["profiles"][0]["overrides"]["pc_validation_high_threshold"] = 16
        with self.assertRaisesRegex(ValueError, "only confidence-update fields"):
            load_experiment_payload(payload, self._trace_config())

    def test_profile_loader_applies_explicit_same_pc_policy_overrides(self):
        payload = self._payload()
        payload["profiles"][0]["policy_overrides"] = {
            "pc_validation_same_pc_hit_gate": True,
            "pc_validation_same_pc_hit_increment": 1,
        }
        experiment = load_experiment_payload(payload, self._trace_config())
        baseline = experiment.profiles[0]
        self.assertEqual(set(baseline.policy_overrides), POLICY_FIELDS)
        self.assertTrue(baseline.config.pc_validation_same_pc_hit_gate)
        self.assertEqual(baseline.config.pc_validation_same_pc_hit_increment, 1)

    def test_profile_loader_rejects_partial_same_pc_policy_override(self):
        payload = self._payload()
        payload["profiles"][0]["policy_overrides"] = {
            "pc_validation_same_pc_hit_gate": True,
        }
        with self.assertRaisesRegex(ValueError, "missing explicit same-PC policy"):
            load_experiment_payload(payload, self._trace_config())

    def test_cohort_uses_raw_accuracy_and_excludes_no_pc_rows(self):
        rows = [
            _pc_row("0x100", _counts(10, 1, 9), _counts(5, 1, 4)),
            _pc_row("0x200", _counts(10, 0, 10), _counts(1, 0, 1)),
            _pc_row(None, _counts(10, 10, 0), _counts(10, 10, 0)),
        ]
        cohort = _cohort_report(rows, 100, 0.1)
        self.assertEqual(cohort["qualified_pc_count"], 1)
        self.assertEqual(cohort["raw"]["candidates"], 10)
        self.assertEqual(cohort["current"]["candidates"], 5)
        self.assertEqual(cohort["candidate_retention"], 0.5)
        self.assertEqual(cohort["useful_retention"], 1.0)
        self.assertEqual(cohort["qualified_pcs"][0]["issuer_trigger_pc"], "0x100")

    def test_aggregate_quality_exposes_standard_coverage_name(self):
        quality = _quality_with_coverage({
            "combined": {
                "coverage_contribution": 0.25,
                "accuracy": 0.5,
            },
        })
        self.assertEqual(quality["combined"]["coverage"], 0.25)
        self.assertEqual(quality["combined"]["coverage_contribution"], 0.25)

    def test_medium_hysteresis_holds_before_medium_to_low_transition(self):
        config = BOPConfig(
            bop_name="system.l2.bop_large",
            block_size=64,
            pc_validation_confidence=True,
            pc_validation_entries=8,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=4,
            pc_validation_initial=8,
            pc_validation_medium_threshold=8,
            pc_validation_high_threshold=12,
            pc_validation_hit_increment=4,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
            pc_validation_low_entry_miss_streak_threshold=3,
            pc_validation_offset_context_slots=1,
        )
        controller = PCValidationController(config)
        for _ in range(2):
            self.assertTrue(controller.policy_candidate_values(
                bop_kind="large", best_offset=1, best_offset_changed=False,
                raw_candidate_valid=True, pc_confidence_enabled=True,
                validation_enabled=True, validation_hit=0,
                trigger_addr=0x1000, trigger_pc=0x400, trigger_has_pc=True,
            ))
            controller.commit()
            lookup = controller.lookup(0x400, "large", 1)
            self.assertEqual(lookup.confidence, 8)

        self.assertTrue(controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=0,
            trigger_addr=0x1000, trigger_pc=0x400, trigger_has_pc=True,
        ))
        controller.commit()
        lookup = controller.lookup(0x400, "large", 1)
        self.assertEqual(lookup.confidence, 7)

    def _same_pc_gate_config(self, **overrides: object) -> BOPConfig:
        values: dict[str, object] = {
            "bop_name": "system.l2.bop_large",
            "block_size": 64,
            "pc_validation_confidence": True,
            "pc_validation_producer_consumer": True,
            "pc_validation_entries": 8,
            "pc_validation_tag_bits": 8,
            "pc_validation_counter_bits": 4,
            "pc_validation_initial": 0,
            "pc_validation_medium_threshold": 2,
            "pc_validation_high_threshold": 6,
            "pc_validation_hit_increment": 4,
            "pc_validation_medium_sample_period": 4,
            "pc_validation_miss_decay_period": 1,
        }
        values.update(overrides)
        return BOPConfig(**values)

    @staticmethod
    def _same_pc_values() -> dict[str, object]:
        return {
            "bop_kind": "large",
            "best_offset": 1,
            "best_offset_changed": False,
            "raw_candidate_valid": True,
            "pc_confidence_enabled": True,
            "validation_enabled": True,
            "validation_hit": 1,
            "trigger_addr": 0x1000,
            "trigger_pc": 0x400,
            "trigger_has_pc": True,
            "validation_owner_pc": 0x400,
            "validation_owner_valid": True,
        }

    def test_same_pc_hit_default_preserves_unconditional_issue(self):
        controller = PCValidationController(self._same_pc_gate_config())
        self.assertTrue(controller.policy_candidate_values(**self._same_pc_values()))
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 4)
        self.assertEqual(controller.stats()["same_pc_hit_gate_checks"], 0)

    def test_gated_same_pc_hit_suppresses_low_state_but_trains_producer(self):
        controller = PCValidationController(self._same_pc_gate_config(
            pc_validation_same_pc_hit_gate=True,
        ))
        self.assertFalse(controller.policy_candidate_values(**self._same_pc_values()))
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 4)
        stats = controller.stats()
        self.assertEqual(stats["producer_hit_updates"], 1)
        self.assertEqual(stats["consumer_miss_updates"], 0)
        self.assertEqual(stats["same_pc_hit_gate_checks"], 1)
        self.assertEqual(stats["same_pc_hit_gate_suppressed"], 1)

    def test_gated_same_pc_medium_state_uses_existing_sampling(self):
        controller = PCValidationController(self._same_pc_gate_config(
            pc_validation_initial=3,
            pc_validation_same_pc_hit_gate=True,
        ))
        lookup = controller.lookup(0x400, "large", 1)
        expected = controller._sample(0x400, "large", 0x1000 // 64, 4, 0x9E37, 1)
        self.assertEqual(
            controller.policy_candidate_values(**self._same_pc_values()), expected,
        )
        controller.commit()
        self.assertEqual(lookup.state, "medium")

    def test_same_pc_reward_override_only_changes_same_pc_credit(self):
        controller = PCValidationController(self._same_pc_gate_config(
            pc_validation_same_pc_hit_gate=True,
            pc_validation_same_pc_hit_increment=1,
        ))
        self.assertFalse(controller.policy_candidate_values(**self._same_pc_values()))
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large", 1).confidence, 1)


if __name__ == "__main__":
    unittest.main()

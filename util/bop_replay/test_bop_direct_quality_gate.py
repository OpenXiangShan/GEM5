#!/usr/bin/env python3
"""Focused tests for causal sampled direct BOP-quality admission."""

from __future__ import annotations

import unittest

from replay_bop_direct_quality_gate import (
    DirectQualityConfig,
    DirectQualityController,
    _IssuedCandidate,
    _QualityAccumulator,
    SampledFeedbackTable,
)


def config(**overrides: object) -> DirectQualityConfig:
    values: dict[str, object] = {
        "quality_entries": 4,
        "quality_ways": 1,
        "quality_tag_bits": 8,
        "feedback_entries": 2,
        "feedback_ways": 1,
        "feedback_tag_bits": 8,
        "horizon": 4,
        "observe_sample_period": 1,
        "open_sample_period": 1,
        "block_probe_period": 1,
        "reopen_probe_period": 1,
        "min_samples": 4,
        "unused_per_useful": 10,
        "block_guard": 0,
        "reopen_unused_per_useful": 8,
        "reopen_guard": 0,
        "decay_period": 0,
    }
    values.update(overrides)
    return DirectQualityConfig(**values)


class DirectQualityControllerTest(unittest.TestCase):
    def test_ten_to_one_negative_evidence_blocks_context(self):
        controller = DirectQualityController(config())
        lookup = controller.lookup(0x1000, "large")
        for _ in range(4):
            controller.note_sample(lookup.index, lookup.generation, "unused")
        final = controller.lookup(0x1000, "large")
        self.assertEqual(final.state, "block")

    def test_positive_samples_open_context_after_minimum_evidence(self):
        controller = DirectQualityController(config())
        lookup = controller.lookup(0x1000, "small")
        for _ in range(4):
            controller.note_sample(lookup.index, lookup.generation, "useful")
        final = controller.lookup(0x1000, "small")
        self.assertEqual(final.state, "open")
        issued, sampled = controller.should_issue(final, 0x1000, "small", 0x2000)
        self.assertTrue(issued)
        self.assertTrue(sampled)

    def test_fail_open_observe_issues_unsampled_candidate(self):
        legacy = DirectQualityController(config(observe_sample_period=16))
        fail_open = DirectQualityController(config(
            observe_sample_period=16, observe_issue_all=True,
        ))
        legacy_lookup = legacy.lookup(0x1000, "large")
        fail_open_lookup = fail_open.lookup(0x1000, "large")

        for line in range(0x2000, 0x4000, 64):
            legacy_issued, legacy_sampled = legacy.should_issue(
                legacy_lookup, 0x1000, "large", line,
            )
            if legacy_sampled:
                continue
            fail_open_issued, fail_open_sampled = fail_open.should_issue(
                fail_open_lookup, 0x1000, "large", line,
            )
            self.assertFalse(legacy_issued)
            self.assertFalse(fail_open_sampled)
            self.assertTrue(fail_open_issued)
            self.assertEqual(fail_open.stats["observe_fail_open_issued"], 1)
            break
        else:
            self.fail("expected at least one unsampled deterministic OBSERVE line")

    def test_global_counter_sampling_is_invariant_to_line_translation(self):
        def pattern(base: int) -> list[bool]:
            controller = DirectQualityController(config(
                observe_sample_period=4,
                observe_issue_all=True,
                sample_source="global_counter",
                sample_counter_bits=8,
            ))
            result = []
            for offset in range(32):
                lookup = controller.lookup(0x1000, "large")
                _, sampled = controller.should_issue(
                    lookup, 0x1000, "large", base + offset * 64,
                )
                result.append(sampled)
            return result

        self.assertEqual(pattern(0x2000), pattern(0x900000))

    def test_trained_open_context_does_not_revert_to_observe_after_decay(self):
        controller = DirectQualityController(config(decay_period=4))
        lookup = controller.lookup(0x1000, "small")
        for _ in range(4):
            controller.note_sample(lookup.index, lookup.generation, "useful")

        final = controller.lookup(0x1000, "small")
        self.assertEqual(final.state, "open")
        self.assertEqual(final.useful, 2)
        self.assertEqual(final.unused, 0)

    def test_one_offset_context_preserves_pc_kind_behavior(self):
        controller = DirectQualityController(config(offset_context_slots=1))
        initial = controller.lookup(0x1000, "large", best_offset=1)
        for _ in range(4):
            controller.note_sample(initial.index, initial.generation, "useful")

        changed_offset = controller.lookup(0x1000, "large", best_offset=-4)
        self.assertEqual(changed_offset.context_index, 0)
        self.assertEqual(changed_offset.state, "open")
        self.assertEqual(controller.context_snapshot(0x1000, "large"), [{
            "offset": 1,
            "state": "open",
            "block_class": None,
            "sampled_useful": 4,
            "sampled_unused": 0,
            "trained": True,
        }])

    def test_two_offset_contexts_keep_positive_and_negative_evidence_separate(self):
        controller = DirectQualityController(config(offset_context_slots=2))
        positive = controller.lookup(0x1000, "large", best_offset=1)
        negative = controller.lookup(0x1000, "large", best_offset=-4)
        self.assertNotEqual(positive.context_index, negative.context_index)

        for _ in range(4):
            controller.note_sample(
                positive.index, positive.generation, "useful",
                positive.context_index, positive.context_generation,
            )
            controller.note_sample(
                negative.index, negative.generation, "unused",
                negative.context_index, negative.context_generation,
            )

        self.assertEqual(
            controller.lookup(0x1000, "large", best_offset=1).state,
            "open",
        )
        self.assertEqual(
            controller.lookup(0x1000, "large", best_offset=-4).state,
            "block",
        )

    def test_replaced_offset_context_rejects_stale_feedback_label(self):
        controller = DirectQualityController(config(offset_context_slots=2))
        original = controller.lookup(0x1000, "large", best_offset=1)
        controller.lookup(0x1000, "large", best_offset=2)
        controller.lookup(0x1000, "large", best_offset=3)
        controller.note_sample(
            original.index, original.generation, "unused",
            original.context_index, original.context_generation,
        )
        self.assertEqual(controller.stats["offset_context_replacements"], 1)
        self.assertEqual(controller.stats["orphaned_feedback_labels"], 1)

    def test_borderline_block_uses_separate_probe_rate(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            strict_unused_per_useful=4,
            block_guard=0,
            strict_block_guard=0,
            block_probe_period=16,
            borderline_block_probe_period=1,
        ))
        borderline = controller.lookup(0x1000, "large")
        for status in ("useful", "unused", "unused", "unused"):
            controller.note_sample(
                borderline.index, borderline.generation, status,
            )
        self.assertEqual(
            controller.lookup(0x1000, "large").state, "block",
        )
        issued, sampled = controller.should_issue(
            controller.lookup(0x1000, "large"), 0x1000, "large", 0x2000,
        )
        self.assertTrue(issued)
        self.assertTrue(sampled)
        self.assertEqual(controller.stats["block_checks:borderline"], 1)

        strict = controller.lookup(0x2000, "large")
        for _ in range(4):
            controller.note_sample(strict.index, strict.generation, "unused")
        self.assertEqual(controller.lookup(0x2000, "large").state, "block")
        strict_lookup = controller.lookup(0x2000, "large")
        for line in range(0x3000, 0x5000, 64):
            issued, _ = controller.should_issue(
                strict_lookup, 0x2000, "large", line,
            )
            if not issued:
                break
        else:
            self.fail("expected a non-probed strict BLOCK line")
        self.assertEqual(controller.stats["block_checks:strict"], 1)

    def test_default_reopen_transitions_directly_to_open(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            reopen_unused_per_useful=2,
            block_guard=0,
            reopen_guard=0,
        ))
        lookup = controller.lookup(0x1000, "large")
        for status in ("useful", "unused", "unused", "unused"):
            controller.note_sample(lookup.index, lookup.generation, status)
        self.assertEqual(controller.lookup(0x1000, "large").state, "block")

        controller.note_sample(lookup.index, lookup.generation, "useful")
        self.assertEqual(controller.lookup(0x1000, "large").state, "open")

    def test_open_audit_rejects_only_current_episode_low_quality_samples(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            strict_unused_per_useful=4,
            reopen_unused_per_useful=4,
            block_guard=0,
            strict_block_guard=0,
            reopen_guard=0,
            audit_samples=4,
            audit_sample_period=1,
            audit_unused_per_useful=20,
            audit_block_guard=4,
        ))
        lookup = controller.lookup(0x1000, "large")
        for _ in range(4):
            controller.note_sample(lookup.index, lookup.generation, "unused")
        self.assertEqual(controller.lookup(0x1000, "large").state, "block")

        # The probe that clears the reopen threshold starts AUDIT, but it was
        # issued before that audit generation and cannot count toward it.
        controller.note_sample(lookup.index, lookup.generation, "useful")
        auditing = controller.lookup(0x1000, "large")
        self.assertEqual(auditing.state, "audit")
        self.assertEqual(auditing.audit_generation, 1)
        controller.note_sample(
            lookup.index, lookup.generation, "useful", audit_generation=0,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "audit")
        self.assertEqual(
            controller.offset_contexts[lookup.index][0].audit_samples, 0,
        )

        for _ in range(4):
            controller.note_sample(
                lookup.index, lookup.generation, "unused",
                audit_generation=auditing.audit_generation,
            )
        self.assertEqual(controller.lookup(0x1000, "large").state, "block")
        self.assertEqual(controller.stats["audit_failures"], 1)
        self.assertEqual(controller.stats["audit_samples:unused"], 4)

    def test_open_audit_passes_boundary_quality_after_fixed_sample_count(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            strict_unused_per_useful=4,
            reopen_unused_per_useful=4,
            block_guard=0,
            strict_block_guard=0,
            reopen_guard=0,
            audit_samples=32,
            audit_sample_period=4,
            audit_unused_per_useful=20,
            audit_block_guard=4,
        ))
        lookup = controller.lookup(0x1000, "large")
        for _ in range(4):
            controller.note_sample(lookup.index, lookup.generation, "unused")
        controller.note_sample(lookup.index, lookup.generation, "useful")
        auditing = controller.lookup(0x1000, "large")
        self.assertEqual(auditing.state, "audit")

        # A 3/32 useful sample is below the desired 10% boundary rate but
        # above the audit's deliberately strict below-5% rejection line.
        for _ in range(3):
            controller.note_sample(
                lookup.index, lookup.generation, "useful",
                audit_generation=auditing.audit_generation,
            )
        for _ in range(29):
            controller.note_sample(
                lookup.index, lookup.generation, "unused",
                audit_generation=auditing.audit_generation,
            )
        self.assertEqual(controller.lookup(0x1000, "large").state, "open")
        self.assertEqual(controller.stats["audit_passes"], 1)
        self.assertEqual(controller.stats["audit_samples:useful"], 3)
        self.assertEqual(controller.stats["audit_samples:unused"], 29)

    def test_open_audit_issues_unsampled_candidates(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            strict_unused_per_useful=4,
            reopen_unused_per_useful=4,
            block_guard=0,
            strict_block_guard=0,
            reopen_guard=0,
            audit_samples=4,
            audit_sample_period=16,
        ))
        lookup = controller.lookup(0x1000, "large")
        for _ in range(4):
            controller.note_sample(lookup.index, lookup.generation, "unused")
        controller.note_sample(lookup.index, lookup.generation, "useful")
        auditing = controller.lookup(0x1000, "large")
        self.assertEqual(auditing.state, "audit")
        for line in range(0x2000, 0x4000, 64):
            issued, sampled = controller.should_issue(
                auditing, 0x1000, "large", line,
            )
            if not sampled:
                self.assertTrue(issued)
                self.assertEqual(controller.stats["audit_unsampled_issued"], 1)
                break
        else:
            self.fail("expected an unsampled fail-open audit candidate")

    def test_reopen_quarantine_requires_confirmation_before_open(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            reopen_unused_per_useful=2,
            block_guard=0,
            reopen_guard=0,
            reopen_confirm_samples=2,
            reopen_probe_period=1,
        ))
        lookup = controller.lookup(0x1000, "large")
        for status in ("useful", "unused", "unused", "unused"):
            controller.note_sample(lookup.index, lookup.generation, status)
        self.assertEqual(controller.lookup(0x1000, "large").state, "block")

        controller.note_sample(lookup.index, lookup.generation, "useful")
        recovering = controller.lookup(0x1000, "large")
        self.assertEqual(recovering.state, "recover")
        self.assertEqual(
            controller.should_issue(recovering, 0x1000, "large", 0x2000),
            (True, True),
        )

        controller.note_sample(
            lookup.index, lookup.generation, "useful",
            recovery_generation=recovering.recovery_generation,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "recover")
        controller.note_sample(
            lookup.index, lookup.generation, "useful",
            recovery_generation=recovering.recovery_generation,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "open")

    def test_reopen_quarantine_returns_to_block_on_negative_evidence(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            reopen_unused_per_useful=2,
            block_guard=0,
            reopen_guard=0,
            reopen_confirm_samples=4,
        ))
        lookup = controller.lookup(0x1000, "large")
        for status in ("useful", "unused", "unused", "unused", "useful"):
            controller.note_sample(lookup.index, lookup.generation, status)
        self.assertEqual(controller.lookup(0x1000, "large").state, "recover")

        controller.note_sample(lookup.index, lookup.generation, "unused")
        self.assertEqual(controller.lookup(0x1000, "large").state, "block")

    def test_reopen_quarantine_rejects_pre_recovery_sample_as_confirmation(self):
        controller = DirectQualityController(config(
            unused_per_useful=2,
            reopen_unused_per_useful=2,
            block_guard=0,
            reopen_guard=0,
            reopen_confirm_samples=2,
        ))
        lookup = controller.lookup(0x1000, "large")
        for status in ("useful", "unused", "unused", "unused"):
            controller.note_sample(lookup.index, lookup.generation, status)
        self.assertEqual(controller.lookup(0x1000, "large").state, "block")

        # This useful response causes the current BLOCK to enter RECOVER, but
        # it was issued before that recovery generation and cannot confirm it.
        controller.note_sample(lookup.index, lookup.generation, "useful")
        recovering = controller.lookup(0x1000, "large")
        self.assertEqual(recovering.state, "recover")
        self.assertEqual(recovering.recovery_generation, 1)

        controller.note_sample(
            lookup.index, lookup.generation, "useful",
            recovery_generation=0,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "recover")
        controller.note_sample(
            lookup.index, lookup.generation, "useful",
            recovery_generation=1,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "recover")
        controller.note_sample(
            lookup.index, lookup.generation, "useful",
            recovery_generation=1,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "open")

    def test_replaced_quality_entry_rejects_stale_feedback_label(self):
        controller = DirectQualityController(config(quality_entries=1))
        original = controller.lookup(0x1000, "large")
        controller.lookup(0x2000, "large")
        controller.note_sample(original.index, original.generation, "unused")
        self.assertEqual(controller.stats["orphaned_feedback_labels"], 1)

    def test_quality_table_plru_preserves_recent_hot_context(self):
        controller = DirectQualityController(config(
            quality_entries=4, quality_ways=4, quality_tag_bits=8,
        ))
        contexts: list[tuple[int, int]] = []
        tags: set[int] = set()
        pc = 0x1000
        while len(contexts) < 5:
            _, tag = controller._key(pc, "large")
            if tag not in tags:
                tags.add(tag)
                contexts.append((pc, tag))
            pc += 2

        first = controller.lookup(contexts[0][0], "large")
        for candidate_pc, _ in contexts[1:4]:
            controller.lookup(candidate_pc, "large")
        controller.lookup(contexts[0][0], "large")
        controller.lookup(contexts[4][0], "large")

        self.assertEqual(
            controller.entries[first.index].tag, contexts[0][1],
        )
        self.assertEqual(controller.stats["quality_replacements"], 1)


class SampledFeedbackTableTest(unittest.TestCase):
    def test_capacity_eviction_drops_label_without_negative_update(self):
        resolved: list[tuple[int, str]] = []
        dropped: list[int] = []
        table = SampledFeedbackTable(
            config(feedback_entries=1),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry: dropped.append(entry.candidate_id),
        )
        table.insert(0x1000, 0, 1, 0, 1)
        table.insert(0x2000, 0, 1, 0, 2)
        self.assertEqual(dropped, [1])
        self.assertEqual(resolved, [])
        self.assertEqual(table.report()["feedback_evicted_without_label"], 1)

    def test_duplicate_sample_is_coalesced_before_feedback_allocation(self):
        resolved: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(feedback_entries=2, feedback_ways=2),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry: None,
        )
        self.assertTrue(table.insert(0x1000, 0, 1, 0, 1))
        self.assertFalse(table.insert(0x1000, 0, 1, 0, 2))
        table.observe_demand(0x1000, 1)
        self.assertEqual(resolved, [(1, "useful")])
        self.assertEqual(table.report()["feedback_coalesced"], 1)

    def test_expired_sample_becomes_unused(self):
        resolved: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(horizon=2),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry: None,
        )
        table.insert(0x1000, 0, 1, 0, 1)
        table.observe_demand(0x2000, 3)
        self.assertEqual(resolved, [(1, "unused")])


class QualityAccumulatorTest(unittest.TestCase):
    def test_expired_unique_lines_are_not_retained_in_address_buckets(self):
        accumulator = _QualityAccumulator(1)
        for index in range(128):
            accumulator.emit(_IssuedCandidate(
                index, "large", index, index, 0x1000 + index * 64, 0, 0,
            ))
            accumulator.observe_demand(type("Demand", (), {"addr": 0})())
        self.assertLessEqual(len(accumulator.pending_by_line), 1)


if __name__ == "__main__":
    unittest.main()

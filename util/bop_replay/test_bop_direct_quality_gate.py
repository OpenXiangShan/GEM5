#!/usr/bin/env python3
"""Focused tests for causal sampled direct BOP-quality admission."""

from __future__ import annotations

import unittest

from replay_bop_direct_quality_gate import (
    DirectQualityConfig,
    DirectQualityController,
    FEEDBACK_ADDRESS_LAYOUT_SV48,
    FEEDBACK_ADDRESS_LAYOUT_SV48_TRUNCATED,
    FEEDBACK_AGE_ENCODING_EPOCH6,
    FEEDBACK_AGE_ENCODING_EPOCH7,
    FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
    FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
    DirectQualityReplay,
    QUALITY_HASH_LAYOUT_XOR_FOLD,
    _IssuedCandidate,
    _QualityAccumulator,
    SampledFeedbackTable,
)
import bop_replay as replay


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
    def test_xor_fold_tag8_is_tag10_low_subset_with_same_set(self):
        tag10 = DirectQualityController(config(
            quality_entries=256,
            quality_ways=4,
            quality_tag_bits=10,
            quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
        ))
        tag8 = DirectQualityController(config(
            quality_entries=256,
            quality_ways=4,
            quality_tag_bits=8,
            quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
        ))
        for pc in (0x1000, 0x123456, 0x400000, 0x7fff_ffff_ffe):
            for kind in ("large", "small"):
                set10, value10 = tag10._key(pc, kind)
                set8, value8 = tag8._key(pc, kind)
                self.assertEqual(set8, set10)
                self.assertEqual(value8, value10 & 0xff)

    def test_xor_fold_tag8_audits_cross_pc_quality_alias(self):
        tag8 = DirectQualityController(config(
            quality_entries=256,
            quality_ways=4,
            quality_tag_bits=8,
            quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
        ))
        tag10 = DirectQualityController(config(
            quality_entries=256,
            quality_ways=4,
            quality_tag_bits=10,
            quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
        ))
        seen: dict[tuple[int, int], tuple[int, tuple[int, int]]] = {}
        collision: tuple[int, int] | None = None
        for pc in range(0x1000, 0x200000, 2):
            key8 = tag8._key(pc, "large")
            key10 = tag10._key(pc, "large")
            previous = seen.get(key8)
            if previous is not None and previous[1] != key10:
                collision = (previous[0], pc)
                break
            seen[key8] = (pc, key10)
        self.assertIsNotNone(collision)
        first_pc, second_pc = collision

        first = tag8.lookup(first_pc, "large")
        for _ in range(4):
            tag8.note_sample(first.index, first.generation, "unused")
        second = tag8.lookup(second_pc, "large")
        self.assertEqual(second.index, first.index)
        self.assertEqual(second.state, "block")
        self.assertEqual(tag8.stats["quality_key_alias_admissions"], 1)

        first10 = tag10.lookup(first_pc, "large")
        second10 = tag10.lookup(second_pc, "large")
        self.assertNotEqual(second10.index, first10.index)
        self.assertEqual(second10.state, "observe")

    def test_quality_key_owner_rejects_unsupported_context_modes(self):
        with self.assertRaisesRegex(ValueError, "offset_context_slots=1"):
            config(
                feedback_owner_layout=FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
                offset_context_slots=2,
            )

    def test_quality_key_owner_allows_recovery_confirmation(self):
        controller = DirectQualityController(config(
            feedback_owner_layout=FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
            min_samples=1,
            unused_per_useful=1,
            block_guard=0,
            strict_unused_per_useful=2,
            strict_block_guard=0,
            reopen_unused_per_useful=1,
            reopen_guard=0,
            reopen_confirm_samples=2,
        ))
        lookup = controller.lookup(0x1000, "large")
        controller.note_sample(lookup.index, lookup.generation, "unused")
        lookup = controller.lookup(0x1000, "large")
        self.assertEqual(lookup.state, "block")

        controller.note_sample(
            lookup.index, lookup.generation, "useful", 0,
            lookup.context_generation, lookup.recovery_generation,
        )
        recovering = controller.lookup(0x1000, "large")
        self.assertEqual(recovering.state, "recover")

        controller.note_sample(
            recovering.index, recovering.generation, "useful", 0,
            recovering.context_generation, recovering.recovery_generation - 1,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "recover")
        controller.note_sample(
            recovering.index, recovering.generation, "useful", 0,
            recovering.context_generation, recovering.recovery_generation,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "recover")
        controller.note_sample(
            recovering.index, recovering.generation, "useful", 0,
            recovering.context_generation, recovering.recovery_generation,
        )
        self.assertEqual(controller.lookup(0x1000, "large").state, "open")

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

    def test_quality_table_keeps_cross_kind_tag_aliases_separate(self):
        controller = DirectQualityController(config(
            quality_entries=16, quality_ways=4, quality_tag_bits=8,
        ))
        seen: dict[tuple[int, int], tuple[int, str]] = {}
        collision: tuple[int, str, int, str] | None = None
        for pc in range(0x1000, 0x100000, 2):
            for kind in ("large", "small"):
                key = controller._key(pc, kind)
                previous = seen.get(key)
                if previous is not None and previous[1] != kind:
                    collision = (previous[0], previous[1], pc, kind)
                    break
                seen[key] = (pc, kind)
            if collision is not None:
                break
        self.assertIsNotNone(collision)
        first_pc, first_kind, second_pc, second_kind = collision

        first = controller.lookup(first_pc, first_kind)
        for _ in range(4):
            controller.note_sample(first.index, first.generation, "unused")
        self.assertEqual(controller.lookup(first_pc, first_kind).state, "block")

        second = controller.lookup(second_pc, second_kind)
        self.assertNotEqual(second.index, first.index)
        self.assertEqual(second.state, "observe")
        self.assertEqual(controller.lookup(first_pc, first_kind).state, "block")


class SampledFeedbackTableTest(unittest.TestCase):
    def test_epoch_encodings_require_round_robin_and_a_nonzero_timeout(self):
        with self.assertRaisesRegex(ValueError, "round_robin"):
            config(feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7)
        with self.assertRaisesRegex(ValueError, "feedback_epoch_timeout"):
            config(
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7,
            )
        with self.assertRaisesRegex(ValueError, r"\[1, 123\]"):
            config(
                feedback_entries=256,
                feedback_ways=4,
                horizon=2048,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7,
                feedback_epoch_timeout=124,
            )
        with self.assertRaisesRegex(ValueError, r"\[1, 59\]"):
            config(
                feedback_entries=256,
                feedback_ways=4,
                horizon=2048,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH6,
                feedback_epoch_timeout=60,
            )

    def test_explicit_e5_s7_uses_half_range_and_horizon_checks(self):
        cfg = config(
            feedback_entries=64,
            feedback_ways=4,
            horizon=2048,
            feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
            feedback_age_encoding="epoch5",
            feedback_epoch_bits=5,
            feedback_epoch_shift=7,
            feedback_epoch_timeout=15,
        )
        self.assertEqual(cfg.epoch_bits, 5)
        self.assertEqual(cfg.epoch_shift, 7)
        with self.assertRaisesRegex(ValueError, "half range"):
            config(
                feedback_entries=64,
                feedback_ways=4,
                horizon=2048,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding="epoch5",
                feedback_epoch_bits=5,
                feedback_epoch_shift=7,
                feedback_epoch_timeout=16,
            )

    def test_round_robin_sweep_expires_one_slot_at_or_after_timeout(self):
        resolved: list[tuple[int, str]] = []
        dropped: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(
                feedback_entries=4,
                feedback_ways=1,
                horizon=4,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_sweep_timeout=1,
            ),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry, reason: dropped.append((entry.candidate_id, reason)),
        )
        self.assertTrue(table.insert(0x1000, 0, 1, 0, 1))
        inserted_index = next(
            index for index, entry in enumerate(table.entries) if entry.valid
        )
        for demand_index in range(1, 5):
            table.observe_demand(0x8000, demand_index)

        self.assertEqual(resolved, [])
        self.assertEqual(
            dropped, [(1, "feedback_sweep_expired_without_label")],
        )
        stats = table.report()
        self.assertEqual(stats["feedback_sweep_expired_without_label"], 1)
        self.assertGreaterEqual(stats["feedback_sweep_expiry_age_min"], 1)
        self.assertLessEqual(stats["feedback_sweep_expiry_age_max"], 4)
        self.assertEqual(stats["feedback_sweep_pointer_final"], 0)
        self.assertEqual(inserted_index + 1, stats["feedback_sweep_expiry_age_min"])

    def test_round_robin_sweep_gives_useful_priority_at_timeout(self):
        resolved: list[tuple[int, str]] = []
        dropped: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(
                feedback_entries=1,
                feedback_ways=1,
                horizon=2,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_sweep_timeout=2,
            ),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry, reason: dropped.append((entry.candidate_id, reason)),
        )
        self.assertTrue(table.insert(0x1000, 0, 1, 0, 1))
        table.observe_demand(0x8000, 1)
        table.observe_demand(0x1000, 2)

        self.assertEqual(resolved, [(1, "useful")])
        self.assertEqual(dropped, [])
        stats = table.report()
        self.assertNotIn("feedback_sweep_expired_without_label", stats)
        self.assertEqual(stats["feedback_sweep_useful_priority"], 1)

    def test_epoch7_sweep_uses_epoch_timeout_without_shadow_age_policy(self):
        dropped: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(
                feedback_entries=1,
                feedback_ways=1,
                horizon=2048,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7,
                feedback_epoch_timeout=30,
            ),
            lambda entry, status: None,
            lambda entry, reason: dropped.append((entry.candidate_id, reason)),
        )
        self.assertTrue(table.insert(0x1000, 0, 1, 0, 1))
        table.observe_demand(0x8000, 1919)
        self.assertEqual(dropped, [])
        table.observe_demand(0x8000, 1920)

        self.assertEqual(
            dropped, [(1, "feedback_sweep_expired_without_label")],
        )
        stats = table.report()
        self.assertEqual(stats["feedback_sweep_epoch_delta_min"], 30)
        self.assertEqual(stats["feedback_sweep_epoch_delta_max"], 30)
        self.assertEqual(stats["feedback_sweep_expiry_age_min"], 1920)

    def test_epoch7_sweep_epoch_difference_wraps_modulo_128(self):
        dropped: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(
                feedback_entries=1,
                feedback_ways=1,
                horizon=2048,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7,
                feedback_epoch_timeout=30,
            ),
            lambda entry, status: None,
            lambda entry, reason: dropped.append((entry.candidate_id, reason)),
        )
        issue_demand = 126 << 6
        self.assertTrue(table.insert(0x1000, 0, 1, issue_demand, 1))
        table.observe_demand(0x8000, issue_demand + 1919)
        self.assertEqual(dropped, [])
        table.observe_demand(0x8000, issue_demand + 1920)

        self.assertEqual(
            dropped, [(1, "feedback_sweep_expired_without_label")],
        )
        self.assertEqual(table.report()["feedback_sweep_epoch_delta_min"], 30)

    def test_epoch6_t30_matches_epoch7_across_the_six_bit_ring_wrap(self):
        def run(encoding: str) -> tuple[list[tuple[int, str]], dict[str, int | str]]:
            dropped: list[tuple[int, str]] = []
            table = SampledFeedbackTable(
                config(
                    feedback_entries=1,
                    feedback_ways=1,
                    horizon=2048,
                    feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                    feedback_age_encoding=encoding,
                    feedback_epoch_timeout=30,
                ),
                lambda entry, status: None,
                lambda entry, reason: dropped.append((entry.candidate_id, reason)),
            )
            # issueEpoch=62. At +1,920 demands the six-bit current epoch is
            # 28, so elapsedEpoch must resolve to 30 across the modulo wrap.
            issue_demand = 62 << 6
            self.assertTrue(table.insert(0x1000, 0, 1, issue_demand, 1))
            table.observe_demand(0x8000, issue_demand + 1919)
            self.assertEqual(dropped, [])
            table.observe_demand(0x8000, issue_demand + 1920)
            return dropped, table.report()

        epoch6_dropped, epoch6 = run(FEEDBACK_AGE_ENCODING_EPOCH6)
        epoch7_dropped, epoch7 = run(FEEDBACK_AGE_ENCODING_EPOCH7)

        self.assertEqual(epoch6_dropped, epoch7_dropped)
        self.assertEqual(
            epoch6["feedback_sweep_epoch_delta_min"],
            epoch7["feedback_sweep_epoch_delta_min"],
        )
        self.assertEqual(epoch6["feedback_epoch_bits"], 6)
        self.assertEqual(epoch7["feedback_epoch_bits"], 7)

    def test_epoch7_sweep_gives_useful_priority_at_epoch_timeout(self):
        resolved: list[tuple[int, str]] = []
        dropped: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(
                feedback_entries=1,
                feedback_ways=1,
                horizon=2048,
                feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
                feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7,
                feedback_epoch_timeout=30,
            ),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry, reason: dropped.append((entry.candidate_id, reason)),
        )
        self.assertTrue(table.insert(0x1000, 0, 1, 0, 1))
        table.observe_demand(0x1000, 1920)

        self.assertEqual(resolved, [(1, "useful")])
        self.assertEqual(dropped, [])
        self.assertEqual(table.report()["feedback_sweep_useful_priority"], 1)

    def test_sv48_reversible_layout_keeps_distinct_lines_distinct(self):
        resolved: list[tuple[int, str]] = []
        table = SampledFeedbackTable(
            config(
                feedback_entries=4,
                feedback_ways=4,
                feedback_address_layout=FEEDBACK_ADDRESS_LAYOUT_SV48,
                feedback_tag_bits=36,
            ),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry, reason: None,
        )
        first_line = 0x000000000000
        second_line = 1 << 45
        self.assertNotEqual(table._key(first_line), table._key(second_line))
        self.assertTrue(table.insert(first_line, 0, 1, 0, 1))
        self.assertTrue(table.insert(second_line, 0, 1, 0, 2))
        table.observe_demand(first_line, 1)
        self.assertEqual(resolved, [(1, "useful")])

    def test_truncated_sv48_tag_exposes_alias_coalescing_and_false_useful(self):
        table = SampledFeedbackTable(
            config(
                feedback_entries=4,
                feedback_ways=4,
                feedback_address_layout=FEEDBACK_ADDRESS_LAYOUT_SV48_TRUNCATED,
                feedback_tag_bits=18,
            ),
            lambda entry, status: None,
            lambda entry, reason: None,
        )
        # Deterministic positive Sv48 line numbers whose 24-bit compact
        # fingerprints collide under the reversible key.
        first_line = 1829748365685 << 6
        second_line = 739466019150 << 6
        self.assertEqual(table._key(first_line), table._key(second_line))
        self.assertTrue(table.insert(first_line, 0, 1, 0, 1))
        self.assertFalse(table.insert(second_line, 0, 1, 0, 2))
        table.observe_demand(second_line, 1)
        stats = table.report()
        self.assertEqual(stats["feedback_alias_coalesced"], 1)
        self.assertEqual(stats["feedback_false_useful"], 1)

    def test_capacity_eviction_drops_label_without_negative_update(self):
        resolved: list[tuple[int, str]] = []
        dropped: list[int] = []
        table = SampledFeedbackTable(
            config(feedback_entries=1),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry, reason: dropped.append(entry.candidate_id),
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
            lambda entry, reason: None,
        )
        self.assertTrue(table.insert(0x1000, 0, 1, 0, 1))
        self.assertFalse(table.insert(0x1000, 0, 1, 0, 2))
        table.observe_demand(0x1000, 1)
        self.assertEqual(resolved, [(1, "useful")])
        self.assertEqual(table.report()["feedback_coalesced"], 1)

    def test_expired_sample_is_dropped_without_negative_update(self):
        resolved: list[tuple[int, str]] = []
        dropped: list[int] = []
        table = SampledFeedbackTable(
            config(horizon=2),
            lambda entry, status: resolved.append((entry.candidate_id, status)),
            lambda entry, reason: dropped.append(entry.candidate_id),
        )
        table.insert(0x1000, 0, 1, 0, 1)
        table.observe_demand(0x2000, 3)
        self.assertEqual(resolved, [])
        self.assertEqual(dropped, [1])
        self.assertEqual(
            table.report()["feedback_expired_without_label"], 1,
        )


class DirectQualityReplayTest(unittest.TestCase):
    @staticmethod
    def _observe_candidate(
        runner: DirectQualityReplay, access_seq: int, pc: int, candidate: int,
    ) -> None:
        runner.observe_event_values(
            access_seq=access_seq,
            tick=access_seq,
            bop_kind="large",
            trigger_addr=pc,
            trigger_pc=pc,
            trigger_has_pc=True,
            best_offset_after=1,
            raw_candidate_valid=True,
            raw_candidate_addr=candidate,
            phase_id=0,
            selected=True,
        )

    @staticmethod
    def _different_quality_key_pc(
        controller: DirectQualityController, pc: int,
    ) -> int:
        original = controller._key(pc, "large")
        for candidate in range(pc + 2, pc + 0x10000, 2):
            if controller._key(candidate, "large") != original:
                return candidate
        raise AssertionError("could not find a distinct folded Quality key")

    def test_quality_key_owner_drops_evicted_key_on_feedback_resolve(self):
        cfg = config(
            quality_entries=1,
            quality_ways=1,
            quality_tag_bits=8,
            quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
            feedback_owner_layout=FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
            feedback_entries=4,
            feedback_ways=4,
            min_samples=1,
        )
        runner = DirectQualityReplay(cfg, replay.EvaluationWindow())
        runner.observe_demand_values(0x8000, True)
        first_pc = 0x1000
        self._observe_candidate(runner, 1, first_pc, 0x3000)
        replacement_pc = self._different_quality_key_pc(
            runner.controller, first_pc,
        )
        self._observe_candidate(runner, 2, replacement_pc, 0x4000)
        runner.observe_demand_values(0x3000, True)

        first = runner.controller.lookup(first_pc, "large", 1)
        self.assertEqual(first.useful, 0)
        self.assertEqual(runner.stats["feedback_unknown_owner_key_miss"], 1)

    def test_quality_key_owner_accepts_reinserted_logical_key(self):
        cfg = config(
            quality_entries=1,
            quality_ways=1,
            quality_tag_bits=8,
            quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
            feedback_owner_layout=FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
            feedback_entries=4,
            feedback_ways=4,
            min_samples=1,
        )
        runner = DirectQualityReplay(cfg, replay.EvaluationWindow())
        runner.observe_demand_values(0x8000, True)
        first_pc = 0x1000
        self._observe_candidate(runner, 1, first_pc, 0x3000)
        replacement_pc = self._different_quality_key_pc(
            runner.controller, first_pc,
        )
        self._observe_candidate(runner, 2, replacement_pc, 0x4000)
        self._observe_candidate(runner, 3, first_pc, 0x5000)
        runner.observe_demand_values(0x3000, True)

        first = runner.controller.lookup(first_pc, "large", 1)
        self.assertEqual(first.useful, 1)
        self.assertEqual(runner.stats["feedback_unknown_owner_key_miss"], 0)

    def test_tuple_hot_path_matches_object_wrappers(self):
        cfg = config(
            horizon=2,
            min_samples=1,
            feedback_entries=4,
            feedback_ways=1,
            feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
            feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH7,
            feedback_epoch_timeout=1,
        )
        object_runner = DirectQualityReplay(cfg, replay.EvaluationWindow())
        tuple_runner = DirectQualityReplay(cfg, replay.EvaluationWindow())
        demands = (
            replay.Demand(0, 0, 0x1000),
            replay.Demand(1, 1, 0x3000),
            replay.Demand(2, 2, 0x2000),
            replay.Demand(3, 3, 0x5000),
            replay.Demand(4, 4, 0x6000),
        )
        events = (
            replay.ReplayEvent(
                access_seq=1, order=1, bop_name="bop_large",
                bop_kind="large", tick=1, trigger_addr=0x1000,
                trigger_pc=0x400, trigger_has_pc=True, validation_hit=0,
                best_offset_changed=False, issue_enabled=True,
                validation_enabled=False, pc_confidence_enabled=False,
                pc_sampled=False, raw_candidate_valid=True,
                raw_candidate_addr=0x2000, policy_candidate_valid=True,
                policy_candidate_addr=0x2000,
            ),
            replay.ReplayEvent(
                access_seq=3, order=2, bop_name="bop_small",
                bop_kind="small", tick=3, trigger_addr=0x3000,
                trigger_pc=0x500, trigger_has_pc=True, validation_hit=0,
                best_offset_changed=False, issue_enabled=True,
                validation_enabled=False, pc_confidence_enabled=False,
                pc_sampled=False, raw_candidate_valid=True,
                raw_candidate_addr=0x4000, policy_candidate_valid=True,
                policy_candidate_addr=0x4000,
            ),
            replay.ReplayEvent(
                access_seq=4, order=3, bop_name="bop_large",
                bop_kind="large", tick=4, trigger_addr=0x5000,
                trigger_pc=0, trigger_has_pc=False, validation_hit=0,
                best_offset_changed=False, issue_enabled=True,
                validation_enabled=False, pc_confidence_enabled=False,
                pc_sampled=False, raw_candidate_valid=False,
                raw_candidate_addr=0, policy_candidate_valid=False,
                policy_candidate_addr=0,
            ),
        )
        event_index = 0
        for demand in demands:
            object_runner.observe_demand(demand)
            tuple_runner.observe_demand_values(demand.addr, True)
            while event_index < len(events) and events[event_index].access_seq == demand.access_seq:
                event = events[event_index]
                object_runner.observe_event(event)
                tuple_runner.observe_event_values(
                    access_seq=event.access_seq,
                    tick=event.tick,
                    bop_kind=event.bop_kind,
                    trigger_addr=event.trigger_addr,
                    trigger_pc=event.trigger_pc,
                    trigger_has_pc=event.trigger_has_pc,
                    best_offset_after=event.best_offset_after,
                    raw_candidate_valid=event.raw_candidate_valid,
                    raw_candidate_addr=event.raw_candidate_addr,
                    phase_id=event.phase_id,
                    selected=True,
                )
                event_index += 1

        self.assertEqual(object_runner.finish(), tuple_runner.finish())

    def test_horizon_expiry_updates_live_owner_as_unused(self):
        runner = DirectQualityReplay(
            config(horizon=1, min_samples=1),
            replay.EvaluationWindow(),
        )
        # Establish the reporting window before admitting the sampled
        # candidate, matching a normal trace whose first event is a demand.
        runner.observe_demand(replay.Demand(0, 0, 0x4000))
        runner.observe_event(replay.ReplayEvent(
            access_seq=1,
            order=1,
            bop_name="bop_large",
            bop_kind="large",
            tick=1,
            trigger_addr=0x1000,
            trigger_pc=0x2000,
            trigger_has_pc=True,
            validation_hit=0,
            best_offset_changed=False,
            issue_enabled=True,
            validation_enabled=False,
            pc_confidence_enabled=False,
            pc_sampled=False,
            raw_candidate_valid=True,
            raw_candidate_addr=0x3000,
            policy_candidate_valid=True,
            policy_candidate_addr=0x3000,
        ))
        runner.observe_demand(replay.Demand(1, 2, 0x4000))
        runner.observe_demand(replay.Demand(2, 3, 0x4000))

        lookup = runner.controller.lookup(0x2000, "large")
        self.assertEqual(lookup.unused, 1)
        self.assertEqual(lookup.state, "block")
        self.assertEqual(runner.controller.stats["samples:unused"], 1)


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

#!/usr/bin/env python3
"""Unit tests for the filter-free BOP demand-oracle replay contract."""

import sqlite3
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from bop_replay import (
    BOPConfig,
    Candidate,
    Demand,
    DemandOracle,
    EvaluationWindow,
    LearnerOutput,
    PCValidationController,
    ReplayEvent,
    ReplayDelayAction,
    ReplayPhase,
    evaluate_candidates,
    evaluate_window,
    load_trace_connection,
    run_quality,
    BOPLearner,
    compare_online_learner,
    replay_learner,
    replay_controller,
    replay_learner_controller,
    replay_learner_policy_metrics,
    resolve_evaluation_window,
    stream_direct_replay,
    _stats_block_final_tick,
    _learner_parameter_report,
    _apply_learner_overrides,
    _apply_controller_overrides,
    _RREntry,
)


class DemandOracleTest(unittest.TestCase):
    def test_fifo_duplicate_expiry_and_censoring(self):
        oracle = DemandOracle(horizon=2)
        oracle.observe_demand(Demand(1, 10, 0x10))
        oracle.emit("large", 1, 11, 0x100)
        oracle.emit("small", 1, 12, 0x100)
        oracle.emit("large", 1, 13, 0x200)
        oracle.emit("small", 1, 14, 0x300)

        oracle.observe_demand(Demand(2, 20, 0x100))
        oracle.observe_demand(Demand(3, 30, 0x200))
        oracle.observe_demand(Demand(4, 40, 0x400))
        oracle.emit("large", 4, 41, 0x500)
        candidates = oracle.finish()
        metrics = evaluate_candidates(
            [
                Demand(1, 10, 0x10),
                Demand(2, 20, 0x100),
                Demand(3, 30, 0x200),
                Demand(4, 40, 0x400),
            ],
            candidates,
            horizon=2,
        )

        self.assertEqual([candidate.status for candidate in candidates], [
            "useful", "redundant", "useful", "unused", "censored"
        ])
        self.assertEqual(metrics.useful, 2)
        self.assertEqual(metrics.redundant, 1)
        self.assertEqual(metrics.unused, 1)
        self.assertEqual(metrics.censored, 1)
        self.assertEqual(metrics.accuracy, 0.5)
        self.assertEqual(metrics.coverage, 0.5)
        self.assertEqual(candidates[0].demand_distance, 1)
        self.assertEqual(candidates[2].demand_distance, 2)

    def test_same_access_demand_is_not_a_future_label(self):
        oracle = DemandOracle(horizon=1)
        oracle.observe_demand(Demand(1, 10, 0x100))
        candidate = oracle.emit("large", 1, 11, 0x100)
        oracle.observe_demand(Demand(2, 20, 0x100))
        oracle.finish()

        self.assertEqual(candidate.status, "useful")
        self.assertEqual(candidate.matched_demand_seq, 2)
        self.assertEqual(candidate.demand_distance, 1)

    def test_candidate_between_demands_uses_access_sequence_order(self):
        metrics = evaluate_candidates(
            [Demand(1, 10, 0x10), Demand(3, 30, 0x100)],
            [
                Candidate(
                    candidate_id=0,
                    kind="large",
                    access_seq=2,
                    tick=20,
                    addr=0x100,
                    demand_index_at_issue=0,
                )
            ],
            horizon=1,
        )
        self.assertEqual(metrics.useful, 1)
        self.assertEqual(metrics.censored, 0)

    def test_stable_window_excludes_warmup_candidates_and_demands(self):
        demands = [
            Demand(1, 10, 0x10, phase_id=0),
            Demand(2, 20, 0x200, phase_id=1),
            Demand(3, 30, 0x100, phase_id=1),
        ]
        candidates = [
            Candidate(0, "large", 1, 11, 0x100, 0, phase_id=0),
            Candidate(1, "large", 2, 21, 0x100, 0, phase_id=1),
        ]
        report = evaluate_window(
            demands, candidates, horizon=2,
            window=EvaluationWindow(
                name="stable", source="trace_phase", phase_id=1,
            ),
        )
        self.assertEqual(report["combined"].candidates, 1)
        self.assertEqual(report["combined"].useful, 1)
        self.assertEqual(report["combined"].eligible_demands, 2)
        self.assertEqual(report["combined"].coverage, 0.5)

    def test_no_retention_oracle_reports_labels_without_history(self):
        statuses = []
        oracle = DemandOracle(
            horizon=1,
            keep_candidates=False,
            on_resolve=lambda candidate: statuses.append(candidate.status),
        )
        oracle.observe_demand(Demand(1, 10, 0x10))
        oracle.emit("large", 1, 11, 0x100)
        oracle.observe_demand(Demand(2, 20, 0x100))
        oracle.emit("small", 2, 21, 0x200)

        self.assertEqual(list(oracle.finish()), [])
        self.assertEqual(statuses, ["useful", "censored"])
        self.assertEqual(list(oracle.candidates), [])


class PCControllerTest(unittest.TestCase):
    def test_shared_controller_override_updates_both_bop_configs(self):
        config = BOPConfig(
            bop_name="root",
            pc_validation_entries=128,
            learner_configs={
                "system.bop_large": BOPConfig(
                    bop_name="system.bop_large", pc_validation_entries=128,
                ),
                "system.bop_small": BOPConfig(
                    bop_name="system.bop_small", pc_validation_entries=128,
                ),
            },
        )
        updated = _apply_controller_overrides(
            {
                "pc_validation_entries": 512,
                "pc_validation_medium_sample_period": 2,
                "global_bop_unused_threshold": 64,
            },
            config,
        )

        self.assertEqual(updated.pc_validation_entries, 512)
        self.assertEqual(updated.global_bop_unused_threshold, 64)
        self.assertEqual(updated.for_kind("large").pc_validation_entries, 512)
        self.assertEqual(updated.for_kind("small").pc_validation_entries, 512)
        self.assertEqual(
            updated.for_kind("large").pc_validation_medium_sample_period, 2
        )
        self.assertEqual(
            updated.for_kind("small").global_bop_unused_threshold, 64
        )

    def test_shared_controller_override_rejects_learner_fields(self):
        with self.assertRaisesRegex(ValueError, "unsupported shared controller"):
            _apply_controller_overrides(
                {"score_max": 24}, BOPConfig(bop_name="root")
            )

    def test_unvalidated_raw_candidate_is_admitted(self):
        controller = PCValidationController(BOPConfig(global_coverage_guard=False))
        event = ReplayEvent(
            access_seq=1,
            order=1,
            bop_name="large",
            bop_kind="large",
            tick=10,
            trigger_addr=0x100,
            trigger_pc=0,
            trigger_has_pc=False,
            validation_hit=-1,
            best_offset_changed=False,
            issue_enabled=True,
            validation_enabled=False,
            pc_confidence_enabled=False,
            pc_sampled=False,
            raw_candidate_valid=True,
            raw_candidate_addr=0x140,
            policy_candidate_valid=True,
            policy_candidate_addr=0x140,
        )
        self.assertTrue(controller.policy_candidate(event))

    def test_hit_updates_progress_low_medium_high(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_entries=4,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=2,
                pc_validation_initial=0,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=2,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
            )
        )
        lookup = controller.lookup(0x400, "large")
        self.assertEqual(lookup.state, "low")
        controller.submit_validation(lookup, 0x400, 0x20, True)
        controller.commit()

        lookup = controller.lookup(0x400, "large")
        self.assertEqual(lookup.state, "medium")
        controller.submit_validation(lookup, 0x400, 0x21, True)
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large").state, "high")

    def test_offset_contexts_isolate_confidence_and_train_on_change(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_entries=4,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=2,
                pc_validation_initial=0,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=2,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
                pc_validation_offset_context_slots=2,
            )
        )

        def train(offset: int, changed: bool) -> None:
            self.assertTrue(controller.policy_candidate_values(
                bop_kind="large",
                best_offset=offset,
                best_offset_changed=changed,
                raw_candidate_valid=True,
                pc_confidence_enabled=True,
                validation_enabled=True,
                validation_hit=1,
                trigger_addr=0x1000,
                trigger_pc=0x400,
                trigger_has_pc=True,
            ))
            controller.commit()

        train(1, False)
        self.assertEqual(controller.lookup(0x400, "large", 1).state, "medium")
        self.assertEqual(controller.lookup(0x400, "large", 2).state, "low")

        train(2, True)
        self.assertEqual(controller.lookup(0x400, "large", 1).state, "medium")
        self.assertEqual(controller.lookup(0x400, "large", 2).state, "medium")

    def test_offset_context_lru_replaces_only_the_oldest_slot(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_entries=4,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=2,
                pc_validation_initial=0,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=2,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
                pc_validation_offset_context_slots=2,
            )
        )
        for offset in (1, 2):
            lookup = controller.lookup(0x400, "large", offset)
            controller.submit_validation(lookup, 0x400, 0x20, True)
            controller.commit()

        self.assertEqual(controller.lookup(0x400, "large", 1).state, "medium")
        self.assertEqual(controller.lookup(0x400, "large", 3).state, "low")
        self.assertEqual(controller.lookup(0x400, "large", 1).state, "medium")
        self.assertEqual(controller.lookup(0x400, "large", 2).state, "low")

    def test_offset_context_stats_report_lookup_and_replacement_counts(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_entries=4,
                pc_validation_tag_bits=8,
                pc_validation_offset_context_slots=2,
            )
        )

        controller.lookup(0x400, "large", 1)
        controller.lookup(0x400, "large", 1)
        controller.lookup(0x400, "large", 2)
        controller.lookup(0x400, "large", 3)

        self.assertEqual(controller.stats(), {
            "offset_context_slots": 2,
            "table_lookups": 4,
            "table_hits": 3,
            "table_misses": 1,
            "table_replacements": 0,
            "offset_context_hits": 1,
            "offset_context_misses": 3,
            "offset_context_replacements": 1,
            "epoch_resets": 0,
            "offset_epoch_changes": 0,
            "rr_owner_valid_hits": 0,
            "rr_owner_invalid_hits": 0,
            "rr_owner_same_pc_hits": 0,
            "rr_owner_cross_pc_hits": 0,
            "producer_hit_updates": 0,
            "consumer_miss_updates": 0,
            "same_pc_hit_gate_checks": 0,
            "same_pc_hit_gate_issued": 0,
            "same_pc_hit_gate_suppressed": 0,
            "recovered_compatible_hits": 0,
            "recovered_producer_credits": 0,
            "recovered_producer_credit_suppressed": 0,
            "recovered_probation_armed": 0,
            "recovered_probation_confirmed": 0,
        })

    def test_producer_consumer_same_pc_hit_rewards_without_decay(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_producer_consumer=True,
                pc_validation_entries=4,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=3,
                pc_validation_initial=1,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=2,
                pc_validation_hit_increment=2,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
            )
        )
        self.assertTrue(controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x100, trigger_pc=0x400, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
        ))
        controller.commit()
        self.assertEqual(controller.lookup(0x400, "large", 1).state, "high")
        self.assertEqual(controller.stats()["consumer_miss_updates"], 0)

    def test_cross_pc_consumer_decays_and_is_suppressed(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_producer_consumer=True,
                pc_validation_entries=8,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=3,
                pc_validation_initial=1,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=3,
                pc_validation_hit_increment=1,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
            )
        )
        values = dict(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x100, trigger_pc=0x500, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
        )
        self.assertTrue(controller.policy_candidate_values(**values))
        controller.commit()
        self.assertEqual(controller.lookup(0x500, "large", 1).state, "low")
        self.assertFalse(controller.policy_candidate_values(**values))
        controller.commit()
        self.assertEqual(controller.stats()["rr_owner_cross_pc_hits"], 2)
        self.assertEqual(controller.stats()["consumer_miss_updates"], 2)

    def test_downstream_producer_credit_recovers_chained_consumer(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_producer_consumer=True,
                pc_validation_entries=8,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=3,
                pc_validation_initial=1,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=2,
                pc_validation_hit_increment=2,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
            )
        )
        self.assertTrue(controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x100, trigger_pc=0x500, trigger_has_pc=True,
            validation_owner_pc=0x400, validation_owner_valid=True,
        ))
        controller.commit()
        self.assertEqual(controller.lookup(0x500, "large", 1).state, "low")

        self.assertTrue(controller.policy_candidate_values(
            bop_kind="large", best_offset=1, best_offset_changed=False,
            raw_candidate_valid=True, pc_confidence_enabled=True,
            validation_enabled=True, validation_hit=1,
            trigger_addr=0x140, trigger_pc=0x600, trigger_has_pc=True,
            validation_owner_pc=0x500, validation_owner_valid=True,
        ))
        controller.commit()
        self.assertEqual(controller.lookup(0x500, "large", 1).state, "high")

    def test_global_window_uses_oracle_outcomes(self):
        controller = PCValidationController(
            BOPConfig(
                pc_validation_entries=4,
                pc_validation_tag_bits=8,
                pc_validation_counter_bits=2,
                pc_validation_initial=0,
                pc_validation_medium_threshold=1,
                pc_validation_high_threshold=2,
                pc_validation_medium_sample_period=1,
                pc_validation_miss_decay_period=1,
                global_coverage_guard=True,
                global_bop_unused_threshold=224,
                global_bop_min_resolved_coverage_shift=3,
            )
        )
        for _ in range(512):
            controller.note_issued()
            controller.note_outcome(True)
        self.assertTrue(controller.global_bypass)
        self.assertEqual(controller.global_unused_ewma, 224)


class PolicyStreamingMetricsTest(unittest.TestCase):
    @staticmethod
    def _output(
        access_seq: int, order: int, kind: str, phase_id: int, candidate_addr: int,
    ) -> LearnerOutput:
        event = ReplayEvent(
            access_seq=access_seq,
            order=order,
            bop_name=f"system.l2.bop_{kind}",
            bop_kind=kind,
            tick=access_seq * 10,
            trigger_addr=0x1000 + (access_seq * 64),
            trigger_pc=0x800 + (order * 4),
            trigger_has_pc=True,
            validation_hit=1,
            best_offset_changed=False,
            issue_enabled=True,
            validation_enabled=True,
            pc_confidence_enabled=True,
            pc_sampled=False,
            raw_candidate_valid=True,
            raw_candidate_addr=candidate_addr,
            policy_candidate_valid=True,
            policy_candidate_addr=candidate_addr,
            phase_id=phase_id,
        )
        return LearnerOutput(
            event=event,
            kind=kind,
            best_offset_before=1,
            best_offset_after=1,
            best_score=1,
            round=0,
            best_offset_changed=False,
            issue_enabled=True,
            raw_candidate_valid=True,
            raw_candidate_addr=candidate_addr,
            validation_enabled=True,
            validation_hit=1,
        )

    def test_streaming_policy_metrics_match_materialized_phase_window(self):
        config = BOPConfig(
            bop_name="root",
            delay_queue_enabled=False,
            offsets=(1,),
            pc_validation_entries=4,
            pc_validation_tag_bits=8,
            pc_validation_counter_bits=2,
            pc_validation_initial=0,
            pc_validation_medium_threshold=1,
            pc_validation_high_threshold=2,
            pc_validation_medium_sample_period=1,
            pc_validation_miss_decay_period=1,
            global_coverage_guard=True,
        )
        demands = [
            Demand(1, 10, 0x10, phase_id=0),
            Demand(2, 20, 0x100, phase_id=0),
            Demand(3, 30, 0x200, phase_id=1),
            Demand(4, 40, 0x300, phase_id=1),
            Demand(5, 50, 0x400, phase_id=1),
            Demand(6, 60, 0x500, phase_id=1),
            Demand(7, 70, 0x600, phase_id=1),
        ]
        outputs = [
            self._output(1, 1, "large", 0, 0x100),
            self._output(2, 2, "small", 0, 0x200),
            self._output(3, 3, "large", 1, 0x300),
            self._output(4, 4, "large", 1, 0x400),
            self._output(4, 5, "small", 1, 0x400),
            self._output(5, 6, "large", 1, 0x700),
        ]
        window = EvaluationWindow(
            name="stable", source="trace_phase", phase_id=1,
        )

        materialized = evaluate_window(
            demands,
            replay_learner_controller(demands, outputs, config, horizon=2),
            horizon=2,
            window=window,
        )
        streaming = replay_learner_policy_metrics(
            demands, outputs, config, horizon=2, window=window,
        )

        self.assertEqual(streaming, materialized)
        self.assertEqual(streaming["combined"].useful, 2)
        self.assertEqual(streaming["combined"].redundant, 1)
        self.assertEqual(streaming["combined"].censored, 1)


class StreamingDatabaseReplayTest(unittest.TestCase):
    @staticmethod
    def _v5_connection() -> sqlite3.Connection:
        connection = sqlite3.connect(":memory:")
        connection.executescript(
            "CREATE TABLE BOPReplayMeta("
            "SchemaVersion, BOPName, BlockSize, PCValidationEntries, "
            "PCValidationTagBits, PCValidationCounterBits, "
            "PCValidationInitial, PCValidationMediumThreshold, "
            "PCValidationHighThreshold, PCValidationHitIncrement, "
            "PCValidationMediumSamplePeriod, PCValidationMissDecayPeriod, "
            "PCValidationLowEntryMissStreakThreshold, PCValidationEpochBits, "
            "GlobalCoverageGuard, GlobalBOPUnusedThreshold, "
            "GlobalBOPMinResolvedCoverageShift);"
            "CREATE TABLE BOPReplayPhase(PhaseId, PhaseName, StartTick);"
            "CREATE TABLE L2DemandTrace(AccessSeq, PhaseId, Tick, Addr);"
            "CREATE TABLE BOPReplayEvent("
            "AccessSeq, BOPName, BOPKind, ReplayOrder, PhaseId, Tick, "
            "TriggerAddr, TriggerPC, TriggerHasPC, ValidationHit, "
            "BestOffsetChanged, IssueEnabled, ValidationEnabled, "
            "PCConfidenceEnabled, PCSampled, RawCandidateValid, "
            "RawCandidateAddr, PolicyCandidateValid, PolicyCandidateAddr);"
            "CREATE TABLE BOPReplayDelayAction("
            "BOPName, ReplayOrder, Action, Tick, Addr, ProcessTick, "
            "QueueSizeAfter);"
        )
        connection.executemany(
            "INSERT INTO BOPReplayMeta VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [
                (5, "system.l2.bop_large", 64, 4, 8, 2, 0, 1, 2, 1,
                 1, 1, 0, 4, 0, 38, 3),
                (5, "system.l2.bop_small", 64, 4, 8, 2, 0, 1, 2, 1,
                 1, 1, 0, 4, 0, 38, 3),
            ],
        )
        connection.executemany(
            "INSERT INTO BOPReplayPhase VALUES(?,?,?)",
            [(0, "trace_start", 0), (1, "stable", 20)],
        )
        connection.executemany(
            "INSERT INTO L2DemandTrace VALUES(?,?,?,?)",
            [
                (1, 0, 10, 0x10),
                (2, 1, 20, 0x100),
                (3, 1, 30, 0x200),
                (4, 1, 40, 0x400),
                (5, 1, 50, 0x500),
            ],
        )
        connection.executemany(
            "INSERT INTO BOPReplayEvent VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [
                (1, "system.l2.bop_large", "large", 1, 0, 11,
                 0x100, 0x800, 1, 1, 0, 1, 1, 1, 0, 1, 0x100, 1, 0x100),
                (2, "system.l2.bop_large", "large", 2, 1, 21,
                 0x140, 0x800, 1, 1, 0, 1, 1, 1, 0, 1, 0x200, 1, 0x200),
                (2, "system.l2.bop_small", "small", 3, 1, 22,
                 0x180, 0x804, 1, 1, 0, 1, 1, 1, 0, 1, 0x200, 1, 0x200),
                (3, "system.l2.bop_large", "large", 4, 1, 31,
                 0x1C0, 0x800, 1, 1, 0, 1, 1, 1, 0, 1, 0x333, 1, 0x333),
                (4, "system.l2.bop_small", "small", 5, 1, 41,
                 0x200, 0x804, 1, 1, 0, 1, 1, 1, 0, 1, 0x999, 1, 0x999),
            ],
        )
        return connection

    def test_controller_tuple_stream_matches_materialized_v5_window(self):
        connection = self._v5_connection()
        demands, events, config = load_trace_connection(connection)
        window = EvaluationWindow(
            name="stable", source="trace_phase", phase_id=1,
        )
        expected = {
            1: evaluate_window(
                demands,
                replay_controller(demands, events, config, horizon=1),
                horizon=1,
                window=window,
            )
        }

        result = stream_direct_replay(
            connection, config, [1], "replay-controller", window,
        )

        self.assertEqual(result.quality_by_horizon, expected)
        self.assertEqual((result.demands, result.events), (5, 5))
        self.assertEqual((result.window_demands, result.window_events), (4, 4))
        self.assertEqual(result.controller_stats[1]["table_lookups"], 5)
        self.assertEqual(result.quality_by_horizon[1]["combined"].useful, 1)
        self.assertEqual(result.quality_by_horizon[1]["combined"].redundant, 1)
        self.assertEqual(result.quality_by_horizon[1]["combined"].unused, 1)
        self.assertEqual(result.quality_by_horizon[1]["combined"].censored, 1)


class LearnerTest(unittest.TestCase):
    def test_v5_delay_actions_replay_callback_before_next_trigger(self):
        config = BOPConfig(
            schema_version=5,
            bop_name="large",
            block_size=64,
            score_max=20,
            round_max=50,
            bad_score=12,
            rr_entries=16,
            tag_bits=8,
            delay_queue_enabled=True,
            delay_queue_size=1,
            delay_ticks=10,
            clock_period_ticks=1,
            offsets=(1,),
            trace_delay_queue_enabled=True,
            trace_delay_queue_size=1,
            trace_delay_ticks=10,
        )
        actions = (
            ReplayDelayAction("large", 1, "enqueue", 10, 0x100, 20, 1),
            ReplayDelayAction("large", 2, "dequeue_to_rr", 20, 0x100, 20, 0),
            ReplayDelayAction("large", 3, "enqueue", 20, 0x140, 30, 1),
        )
        config = replace(config, replay_delay_actions={"large": actions})
        events = [
            ReplayEvent(1, 1, "large", "large", 10, 0x100, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        0, 0, 0, 0, replay_order=1),
            ReplayEvent(2, 2, "large", "large", 20, 0x140, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        0, 0, 0, 0, replay_order=3),
        ]
        outputs = replay_learner(events, config)
        self.assertEqual(outputs[1].best_score, outputs[0].best_score + 1)

    def test_v5_delay_actions_reject_queue_mismatch(self):
        config = BOPConfig(
            schema_version=5,
            bop_name="large",
            block_size=64,
            rr_entries=4,
            tag_bits=8,
            delay_queue_enabled=True,
            delay_queue_size=1,
            delay_ticks=10,
            clock_period_ticks=1,
            offsets=(1,),
            trace_delay_queue_enabled=True,
            trace_delay_queue_size=1,
            trace_delay_ticks=10,
        )
        config = replace(config, replay_delay_actions={"large": (
            ReplayDelayAction("large", 1, "enqueue", 10, 0x100, 20, 1),
            ReplayDelayAction("large", 2, "dequeue_to_rr", 20, 0x180, 20, 0),
        )})
        events = [
            ReplayEvent(1, 1, "large", "large", 10, 0x100, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        0, 0, 0, 0, replay_order=1),
            ReplayEvent(2, 2, "large", "large", 20, 0x140, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        0, 0, 0, 0, replay_order=3),
        ]
        with self.assertRaisesRegex(ValueError, "does not match queue head"):
            replay_learner(events, config)

    def test_baseline_replays_raw_candidate_state(self):
        config = BOPConfig(
            bop_name="large",
            block_size=64,
            score_max=1,
            round_max=10,
            bad_score=0,
            rr_entries=4,
            tag_bits=8,
            delay_queue_enabled=False,
            offsets=(1, 2),
        )
        learner = BOPLearner(config)
        events = [
            ReplayEvent(1, 1, "large", "large", 10, 0x100, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        1, 0, 0, 0),
            ReplayEvent(2, 1, "large", "large", 20, 0x180, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        1, 0, 0, 0),
        ]
        outputs = [learner.process(event) for event in events]
        self.assertEqual(outputs[0].best_offset_before, 2)
        self.assertFalse(outputs[0].raw_candidate_valid)
        self.assertEqual(outputs[1].best_offset_after, 1)
        self.assertTrue(outputs[1].raw_candidate_valid)

    def test_compare_reports_first_online_mismatch(self):
        event = ReplayEvent(1, 1, "large", "large", 10, 0x100, 0, False, -1,
                            False, False, False, False, False, False, 0,
                            False, 2, 0, 0, 0)
        learner = BOPLearner(BOPConfig(offsets=(1,), delay_queue_enabled=False))
        output = learner.process(event)
        result = compare_online_learner([event], [output])
        self.assertFalse(result["pass"])
        self.assertEqual(result["first_mismatch"]["field"], "best_offset_before")

    def test_zero_tag_matches_value_initialized_rr_entry(self):
        learner = BOPLearner(
            BOPConfig(
                block_size=64,
                score_max=1,
                round_max=8,
                bad_score=0,
                rr_entries=4,
                tag_bits=8,
                delay_queue_enabled=False,
                offsets=(1,),
            )
        )
        event = ReplayEvent(
            1, 1, "large", "large", 0x100, 0x100, 0, False, -1,
            False, False, False, False, False, False, 0, False,
            0, 0, 0, 0,
        )

        output = learner.process(event)
        self.assertTrue(output.issue_enabled)
        self.assertTrue(output.raw_candidate_valid)
        self.assertEqual(output.raw_candidate_addr, 0x140)

    def test_untouched_right_rr_bank_can_satisfy_validation(self):
        learner = BOPLearner(
            BOPConfig(
                rr_entries=4,
                tag_bits=8,
                delay_queue_enabled=False,
                offsets=(1,),
            )
        )
        # Replace the matching Left entry with a non-zero tag. Right remains
        # value-initialized and must still match a tag-zero validation lookup.
        learner.rr_left[learner._hash(0)] = _RREntry(0, 1)
        self.assertTrue(learner._test_rr(0))

    def test_delay_queue_moves_one_due_entry_per_callback(self):
        learner = BOPLearner(
            BOPConfig(
                rr_entries=4,
                tag_bits=8,
                delay_queue_enabled=True,
                delay_queue_size=4,
                delay_ticks=5,
                clock_period_ticks=10,
                offsets=(1,),
            )
        )
        learner._insert_training(0x200, 100)
        learner._insert_training(0x240, 100)

        learner._run_delay_events_before(105)
        self.assertEqual(len(learner.delay_queue), 2)
        self.assertEqual(learner.delay_event_tick, 105)

        learner._run_delay_events_before(106)
        self.assertEqual(len(learner.delay_queue), 1)
        self.assertEqual(learner.delay_event_tick, 120)
        self.assertTrue(learner._test_rr(0x200))
        self.assertFalse(learner._test_rr(0x240))

        learner._run_delay_events_before(120)
        self.assertEqual(len(learner.delay_queue), 1)
        learner._run_delay_events_before(121)
        self.assertEqual(len(learner.delay_queue), 0)
        self.assertTrue(learner._test_rr(0x240))

    def test_delay_queue_preserves_rr_producer_owner(self):
        learner = BOPLearner(
            BOPConfig(
                rr_entries=4,
                tag_bits=8,
                delay_queue_enabled=True,
                delay_queue_size=1,
                delay_ticks=5,
                clock_period_ticks=10,
                offsets=(1,),
            )
        )
        event = ReplayEvent(
            access_seq=1, order=1, bop_name="large", bop_kind="large",
            tick=100, trigger_addr=0x200, trigger_pc=0x400,
            trigger_has_pc=True, validation_hit=-1,
            best_offset_changed=False, issue_enabled=False,
            validation_enabled=False, pc_confidence_enabled=False,
            pc_sampled=False, raw_candidate_valid=False,
            raw_candidate_addr=0, policy_candidate_valid=False,
            policy_candidate_addr=0,
        )
        learner._insert_training(learner._trigger_rr_entry(event), 100)
        learner._run_delay_events_before(106)
        entry = learner._test_rr_entry(0x200)
        self.assertIsNotNone(entry)
        assert entry is not None
        self.assertTrue(entry.owner_valid)
        self.assertEqual(entry.owner_pc, 0x400)

    def test_adaptive_depth_updates_active_best_offset(self):
        learner = BOPLearner(
            BOPConfig(
                score_max=255,
                round_max=255,
                rr_entries=4,
                tag_bits=8,
                delay_queue_enabled=False,
                adapt_offset=True,
                offsets=(1,),
            )
        )
        output = None
        for sequence in range(1, 7):
            addr = 0x140 + (sequence - 1) * 0x100
            learner._insert_rr(addr - 0x40)
            output = learner.process(
                ReplayEvent(
                    access_seq=sequence,
                    order=1,
                    bop_name="large",
                    bop_kind="large",
                    tick=sequence * 10,
                    trigger_addr=addr,
                    trigger_pc=0,
                    trigger_has_pc=False,
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
                    late=True,
                )
            )
        assert output is not None
        self.assertEqual(output.best_offset_before, 1)
        self.assertEqual(output.best_offset_after, 2)
        self.assertTrue(output.best_offset_changed)

    def test_baseline_compare_accepts_static_golden_sequence(self):
        config = BOPConfig(
            block_size=64,
            score_max=1,
            round_max=8,
            bad_score=0,
            rr_entries=4,
            tag_bits=8,
            delay_queue_enabled=False,
            offsets=(1,),
        )
        events = [
            ReplayEvent(1, 1, "large", "large", 10, 0x200, 0, False, -1,
                        False, False, False, False, False, False, 0, False,
                        1, 1, 0, 1),
            ReplayEvent(2, 1, "large", "large", 20, 0x240, 0, False, -1,
                        False, True, False, False, False, False, 0, True,
                        1, 1, 0, 0),
        ]
        outputs = [BOPLearner(config).process(events[0])]
        learner = BOPLearner(config)
        outputs = [learner.process(event) for event in events]
        golden = [
            replace(
                events[0], best_offset_before=1, best_offset_after=1,
                best_score=0, round=1, best_offset_changed=False,
                issue_enabled=False, raw_candidate_valid=False, raw_candidate_addr=0,
            ),
            replace(
                events[1], best_offset_before=1, best_offset_after=1,
                best_score=0, round=0, best_offset_changed=False,
                issue_enabled=True, raw_candidate_valid=True, raw_candidate_addr=0x280,
            ),
        ]
        self.assertTrue(compare_online_learner(golden, outputs)["pass"])

    def test_negative_offsets_are_not_expanded_twice(self):
        learner = BOPLearner(
            BOPConfig(
                delay_queue_enabled=False,
                negative_offsets_enable=True,
                offsets=(1, -1),
            )
        )
        self.assertEqual([item[0] for item in learner.offsets], [1, -1])

    def test_overrides_find_full_simobject_names(self):
        large = BOPConfig(bop_name="system.l2.bop_large", offsets=(1,))
        small = BOPConfig(bop_name="system.l2.bop_small", offsets=(2,))
        config = BOPConfig(
            learner_configs={large.bop_name: large, small.bop_name: small}
        )
        overridden = _apply_learner_overrides(
            {
                "large": {"score_max": 24, "offsets": [3, -3]},
                "small": {"bad_score": 7},
            },
            config,
        )
        self.assertEqual(overridden.for_kind("large").score_max, 24)
        self.assertEqual(overridden.for_kind("large").offsets, (3, -3))
        self.assertEqual(overridden.for_kind("small").bad_score, 7)

    def test_learner_validation_drives_strict_policy_stage(self):
        config = BOPConfig(issue_validation=True, delay_queue_enabled=False, offsets=(1,))
        event = ReplayEvent(
            access_seq=1,
            order=1,
            bop_name="large",
            bop_kind="large",
            tick=10,
            trigger_addr=0x100,
            trigger_pc=0,
            trigger_has_pc=False,
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
        )
        denied = BOPLearner(config).process(event)
        denied = replace(
            denied,
            issue_enabled=True,
            raw_candidate_valid=True,
            raw_candidate_addr=0x140,
            validation_enabled=True,
            validation_hit=0,
        )
        allowed = replace(denied, validation_hit=1)

        self.assertEqual(
            replay_learner_controller([], [denied], config, horizon=1), []
        )
        candidates = replay_learner_controller([], [allowed], config, horizon=1)
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].addr, 0x140)

    def test_policy_stage_requires_shared_controller_parameters(self):
        large = BOPConfig(
            bop_name="system.l2.bop_large", delay_queue_enabled=False,
            offsets=(1,), pc_validation_entries=4,
        )
        small = replace(large, bop_name="system.l2.bop_small", pc_validation_entries=8)
        config = BOPConfig(
            learner_configs={large.bop_name: large, small.bop_name: small}
        )
        with self.assertRaisesRegex(ValueError, "pc_validation_entries"):
            replay_learner_controller([], [], config, horizon=1)

    def test_parameter_report_uses_effective_large_small_configs(self):
        large = BOPConfig(bop_name="system.l2.bop_large", offsets=(1, -1))
        small = BOPConfig(bop_name="system.l2.bop_small", offsets=(2, -2))
        report = _learner_parameter_report(
            BOPConfig(learner_configs={large.bop_name: large, small.bop_name: small})
        )
        self.assertEqual(report["large"]["offsets"], [1, -1])
        self.assertEqual(report["small"]["offsets"], [2, -2])


class TraceLoadingTest(unittest.TestCase):
    def test_sqlite_signed_addresses_are_normalized_to_u64(self):
        connection = sqlite3.connect(":memory:")
        connection.executescript(
            "CREATE TABLE BOPReplayMeta("
            "SchemaVersion, BOPName, BlockSize, PCValidationEntries, "
            "PCValidationTagBits, PCValidationCounterBits, "
            "PCValidationInitial, PCValidationMediumThreshold, "
            "PCValidationHighThreshold, PCValidationHitIncrement, "
            "PCValidationMediumSamplePeriod, PCValidationMissDecayPeriod, "
            "PCValidationLowEntryMissStreakThreshold, PCValidationEpochBits, "
            "GlobalCoverageGuard, GlobalBOPUnusedThreshold, "
            "GlobalBOPMinResolvedCoverageShift, ClockPeriodTicks);"
            "CREATE TABLE L2DemandTrace(AccessSeq, Tick, Addr);"
            "CREATE TABLE BOPReplayEvent("
            "AccessSeq, BOPName, BOPKind, Tick, TriggerAddr, TriggerPC, TriggerHasPC, "
            "ValidationHit, BestOffsetChanged, IssueEnabled, ValidationEnabled, "
            "PCConfidenceEnabled, PCSampled, RawCandidateValid, RawCandidateAddr, "
            "PolicyCandidateValid, PolicyCandidateAddr);"
        )
        connection.execute(
            "INSERT INTO BOPReplayMeta VALUES(4,'large',64,4,8,2,0,1,2,1,1,1,0,4,0,38,3,1000)"
        )
        connection.execute("INSERT INTO L2DemandTrace VALUES(1,10,-64)")
        connection.execute(
            "INSERT INTO BOPReplayEvent VALUES(1,'large','large',11,-128,-256,1,"
            "-1,0,1,1,1,0,1,-192,1,-320)"
        )

        demands, events, _ = load_trace_connection(connection)
        self.assertEqual(demands[0].addr, (1 << 64) - 64)
        self.assertEqual(events[0].trigger_addr, (1 << 64) - 128)
        self.assertEqual(events[0].trigger_pc, (1 << 64) - 256)
        self.assertEqual(events[0].raw_candidate_addr, (1 << 64) - 192)
        self.assertEqual(events[0].policy_candidate_addr, (1 << 64) - 320)

    def test_load_and_report_recorded_policy(self):
        connection = sqlite3.connect(":memory:")
        connection.executescript(
            "CREATE TABLE BOPReplayMeta("
            "SchemaVersion, BOPName, BlockSize, PCValidationEntries, "
            "PCValidationTagBits, PCValidationCounterBits, "
            "PCValidationInitial, PCValidationMediumThreshold, "
            "PCValidationHighThreshold, PCValidationHitIncrement, "
            "PCValidationMediumSamplePeriod, PCValidationMissDecayPeriod, "
            "PCValidationLowEntryMissStreakThreshold, PCValidationEpochBits, "
            "GlobalCoverageGuard, GlobalBOPUnusedThreshold, "
            "GlobalBOPMinResolvedCoverageShift);"
            "CREATE TABLE L2DemandTrace(AccessSeq, Tick, Addr);"
            "CREATE TABLE BOPReplayEvent("
            "AccessSeq, BOPName, BOPKind, Tick, TriggerAddr, TriggerPC, TriggerHasPC, "
            "ValidationHit, BestOffsetChanged, IssueEnabled, ValidationEnabled, "
            "PCConfidenceEnabled, PCSampled, RawCandidateValid, RawCandidateAddr, "
            "PolicyCandidateValid, PolicyCandidateAddr);"
        )
        connection.execute(
            "INSERT INTO BOPReplayMeta VALUES(1,'large',64,4,8,2,0,1,2,1,1,1,0,4,0,38,3)"
        )
        connection.executemany(
            "INSERT INTO L2DemandTrace VALUES(?,?,?)",
            [(1, 10, 0x10), (2, 20, 0x100)],
        )
        connection.execute(
            "INSERT INTO BOPReplayEvent VALUES(1,'large','large',11,0x10,0x400,1,1,0,1,1,1,0,1,0x100,1,0x100)"
        )

        demands, events, config = load_trace_connection(connection)
        report = run_quality(demands, events, config, horizon=1, mode="recorded")
        self.assertEqual(report["large"].useful, 1)
        self.assertEqual(report["large"].coverage, 0.5)
        self.assertEqual(report["small"].candidates, 0)

    def test_v3_metadata_reads_clock_period(self):
        connection = sqlite3.connect(":memory:")
        connection.executescript(
            "CREATE TABLE BOPReplayMeta("
            "SchemaVersion, BOPName, BlockSize, PCValidationEntries, "
            "PCValidationTagBits, PCValidationCounterBits, "
            "PCValidationInitial, PCValidationMediumThreshold, "
            "PCValidationHighThreshold, PCValidationHitIncrement, "
            "PCValidationMediumSamplePeriod, PCValidationMissDecayPeriod, "
            "PCValidationLowEntryMissStreakThreshold, PCValidationEpochBits, "
            "GlobalCoverageGuard, GlobalBOPUnusedThreshold, "
            "GlobalBOPMinResolvedCoverageShift, ClockPeriodTicks);"
            "CREATE TABLE L2DemandTrace(AccessSeq, Tick, Addr);"
            "CREATE TABLE BOPReplayEvent("
            "AccessSeq, BOPName, BOPKind, Tick, TriggerAddr, TriggerPC, TriggerHasPC, "
            "ValidationHit, BestOffsetChanged, IssueEnabled, ValidationEnabled, "
            "PCConfidenceEnabled, PCSampled, RawCandidateValid, RawCandidateAddr, "
            "PolicyCandidateValid, PolicyCandidateAddr);"
        )
        connection.execute(
            "INSERT INTO BOPReplayMeta VALUES(3,'large',64,4,8,2,0,1,2,1,1,1,0,4,0,38,3,1000)"
        )
        connection.execute(
            "INSERT INTO BOPReplayEvent VALUES(1,'large','large',11,0x10,0x400,1,1,0,1,1,1,0,1,0x100,1,0x100)"
        )

        _, _, config = load_trace_connection(connection)
        self.assertEqual(config.schema_version, 3)
        self.assertEqual(config.clock_period_ticks, 1000)

    def test_v4_phase_rows_are_loaded(self):
        connection = sqlite3.connect(":memory:")
        connection.executescript(
            "CREATE TABLE BOPReplayMeta("
            "SchemaVersion, BOPName, BlockSize, PCValidationEntries, "
            "PCValidationTagBits, PCValidationCounterBits, "
            "PCValidationInitial, PCValidationMediumThreshold, "
            "PCValidationHighThreshold, PCValidationHitIncrement, "
            "PCValidationMediumSamplePeriod, PCValidationMissDecayPeriod, "
            "PCValidationLowEntryMissStreakThreshold, PCValidationEpochBits, "
            "GlobalCoverageGuard, GlobalBOPUnusedThreshold, "
            "GlobalBOPMinResolvedCoverageShift, ClockPeriodTicks);"
            "CREATE TABLE BOPReplayPhase(PhaseId, PhaseName, StartTick);"
            "CREATE TABLE L2DemandTrace(AccessSeq, PhaseId, Tick, Addr);"
            "CREATE TABLE BOPReplayEvent("
            "AccessSeq, BOPName, BOPKind, PhaseId, Tick, TriggerAddr, TriggerPC, TriggerHasPC, "
            "ValidationHit, BestOffsetChanged, IssueEnabled, ValidationEnabled, "
            "PCConfidenceEnabled, PCSampled, RawCandidateValid, RawCandidateAddr, "
            "PolicyCandidateValid, PolicyCandidateAddr);"
        )
        connection.execute(
            "INSERT INTO BOPReplayMeta VALUES(4,'large',64,4,8,2,0,1,2,1,1,1,0,4,0,38,3,1000)"
        )
        connection.executemany(
            "INSERT INTO BOPReplayPhase VALUES(?,?,?)",
            [(0, "trace_start", 0), (1, "stable", 100)],
        )
        connection.execute("INSERT INTO L2DemandTrace VALUES(1,1,100,0x100)")
        connection.execute(
            "INSERT INTO BOPReplayEvent VALUES(1,'large','large',1,100,0x10,0x400,1,1,0,1,1,1,0,1,0x100,1,0x100)"
        )

        from bop_replay import _load_trace_connection
        demands, events, config, phases = _load_trace_connection(connection)
        self.assertEqual(config.schema_version, 4)
        self.assertEqual(demands[0].phase_id, 1)
        self.assertEqual(events[0].phase_id, 1)
        self.assertEqual(phases, [
            ReplayPhase(0, "trace_start", 0), ReplayPhase(1, "stable", 100)
        ])

    def test_stats_boundary_and_phase_window_resolution(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            stats_path = Path(temp_dir) / "stats.txt"
            stats_path.write_text(
                "---------- Begin Simulation Statistics ----------\n"
                "finalTick 100 # warmup\n"
                "---------- End Simulation Statistics   ----------\n"
                "---------- Begin Simulation Statistics ----------\n"
                "finalTick 200 # stable\n"
                "---------- End Simulation Statistics   ----------\n"
            )
            self.assertEqual(_stats_block_final_tick(stats_path, 1), 100)
            self.assertEqual(_stats_block_final_tick(stats_path, 2), 200)
            stats_window = resolve_evaluation_window(
                phase_name="full", start_tick=None, stats_path=stats_path,
                stats_block=1, phases=[],
            )
            self.assertEqual(stats_window.start_tick, 100)
            self.assertEqual(stats_window.source, "stats_file")

        phase_window = resolve_evaluation_window(
            phase_name="stable", start_tick=None, stats_path=None,
            stats_block=1,
            phases=[ReplayPhase(0, "trace_start", 0), ReplayPhase(1, "stable", 100)],
        )
        self.assertEqual(phase_window.phase_id, 1)
        self.assertEqual(phase_window.start_tick, 100)

class TeacherStudentBOPReplayTest(unittest.TestCase):
    @staticmethod
    def _event(access_seq: int, addr: int) -> ReplayEvent:
        return ReplayEvent(
            access_seq=access_seq,
            order=access_seq,
            bop_name="system.l2.bop_small",
            bop_kind="small",
            tick=access_seq * 10,
            trigger_addr=addr,
            trigger_pc=0x800,
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
        )

    def test_student_can_issue_when_teacher_is_disabled(self):
        config = BOPConfig(
            bop_name="system.l2.bop_small",
            block_size=64,
            score_max=1,
            round_max=2,
            # The teacher identifies +1 but does not open its own issue path.
            bad_score=3,
            rr_entries=16,
            tag_bits=4,
            delay_queue_enabled=False,
            cross_page=True,
            adapt_offset=False,
            offsets=(1, 2),
            student_cover_enabled=True,
            student_pool_size=2,
            student_conf_alpha=0.0,
            student_cov_threshold=0.5,
            student_filter_entries=8,
            student_hash_mode="splitmix",
            student_hash_count=1,
            student_delay_queue_enabled=False,
        )
        learner = BOPLearner(config)
        base = 0x1000
        learner._insert_rr(base - 64)
        outputs = [
            learner.process(self._event(index, base + index * 64))
            for index in range(4)
        ]

        output = outputs[-1]
        self.assertFalse(learner.issue_enabled)
        self.assertTrue(learner.student_selected_valid)
        self.assertTrue(learner.student_selected_enabled)
        self.assertTrue(output.issue_enabled)
        self.assertTrue(output.raw_candidate_valid)
        self.assertEqual(output.selected_offset, 1)
        self.assertEqual(output.raw_candidate_addr, base + 4 * 64)


if __name__ == "__main__":
    unittest.main(verbosity=2)

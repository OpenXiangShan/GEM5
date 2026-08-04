#!/usr/bin/env python3
"""Focused tests for stable raw-PC oracle threshold selection."""

from __future__ import annotations

import sqlite3
import unittest

import analyze_bop_pc_oracle_threshold as oracle
import bop_replay as replay


def _row(pc: str | None, accuracy: float | None, candidates: int) -> dict[str, object]:
    return {
        "issuer_has_pc": pc is not None,
        "issuer_trigger_pc": pc,
        "raw": {
            "accuracy": accuracy,
            "candidates": candidates,
            "useful": 0,
            "unused": candidates,
            "redundant": 0,
            "censored": 0,
        },
        "current": {
            "accuracy": accuracy,
            "candidates": candidates,
            "useful": 0,
            "unused": candidates,
            "redundant": 0,
            "censored": 0,
        },
    }


class OracleThresholdTest(unittest.TestCase):
    def test_parse_thresholds_accepts_percent_and_fraction_forms(self):
        self.assertEqual(oracle.parse_thresholds("5,0.1,20"), (0.05, 0.1, 0.2))
        with self.assertRaisesRegex(ValueError, "unique"):
            oracle.parse_thresholds("5,0.05")

    def test_selection_keeps_no_pc_and_threshold_equal_pc(self):
        labels, report = oracle.pc_threshold_selection([
            _row("0x100", 0.05, 10),
            _row("0x200", 0.049, 20),
            _row(None, 0.0, 30),
            _row("0x300", None, 4),
        ], 0.05)
        self.assertTrue(labels[(True, 0x100)])
        self.assertFalse(labels[(True, 0x200)])
        self.assertTrue(labels[(True, 0x300)])
        self.assertEqual(report["suppressed_pc_count"], 1)
        self.assertEqual(report["suppressed_raw_pc_sum"]["candidates"], 20)

    def test_admission_masks_match_strict_below_threshold_policy(self):
        thresholds = (0.05, 0.1, 0.2)
        masks, full_mask = oracle._pc_admission_masks([
            _row("0x100", 0.05, 1),
            _row("0x200", 0.15, 1),
            _row("0x300", 0.2, 1),
            _row("0x400", None, 1),
            _row(None, 0.0, 1),
        ], thresholds)
        self.assertEqual(full_mask, 0b111)
        self.assertEqual(masks[(True, 0x100)], 0b001)
        self.assertEqual(masks[(True, 0x200)], 0b011)
        self.assertNotIn((True, 0x300), masks)
        self.assertNotIn((True, 0x400), masks)

    def test_second_pass_exactly_suppresses_labeled_raw_pc(self):
        connection = sqlite3.connect(":memory:")
        connection.executescript("""
            CREATE TABLE L2DemandTrace (AccessSeq, PhaseId, Tick, Addr);
            CREATE TABLE BOPReplayEvent (
                AccessSeq, BOPName, BOPKind, ReplayOrder, PhaseId, Tick,
                TriggerAddr, TriggerPC, TriggerHasPC, ValidationHit,
                BestOffsetChanged, IssueEnabled, ValidationEnabled,
                PCConfidenceEnabled, PCSampled, RawCandidateValid,
                RawCandidateAddr, PolicyCandidateValid, PolicyCandidateAddr,
                Late, BestOffsetBefore, BestOffsetAfter, BestScore, Round,
                TriggerIsDemand, TriggerIsRead
            );
        """)
        connection.execute(
            "INSERT INTO BOPReplayEvent VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (1, "system.l2.bop_large", "large", 1, 1, 10, 0x1000, 0x100,
             1, 0, 0, 1, 1, 1, 0, 1, 0x2000, 0, 0, 0, 1, 1, 1, 0, 1, 1),
        )
        connection.execute("INSERT INTO L2DemandTrace VALUES (2,1,20,0x2000)")
        window = replay.EvaluationWindow(phase_id=1, phase_name="stable")
        quality, _ = oracle.replay_oracle_thresholds(
            connection, [_row("0x100", 0.0, 1)], (0.05,), window,
        )
        self.assertEqual(quality[0.0]["combined"]["useful"], 1)
        self.assertEqual(quality[0.05]["combined"]["candidates"], 0)
        self.assertEqual(quality[0.05]["combined"]["covered_demands"], 0)


if __name__ == "__main__":
    unittest.main()

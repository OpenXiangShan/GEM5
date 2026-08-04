#!/usr/bin/env python3
"""Focused causal tests for direct-quality reopen attribution."""

from __future__ import annotations

import unittest

import bop_replay as replay
from analyze_bop_direct_quality_reopens import ReopenAuditReplay
from replay_bop_direct_quality_gate import DirectQualityConfig


def config(**overrides: object) -> DirectQualityConfig:
    values: dict[str, object] = {
        "quality_entries": 4,
        "quality_ways": 1,
        "quality_tag_bits": 8,
        "feedback_entries": 8,
        "feedback_ways": 1,
        "feedback_tag_bits": 8,
        "horizon": 1,
        "observe_sample_period": 1,
        "observe_issue_all": True,
        "open_sample_period": 1,
        "block_probe_period": 1,
        "borderline_block_probe_period": 1,
        "min_samples": 4,
        "unused_per_useful": 2,
        "block_guard": 0,
        "strict_unused_per_useful": 4,
        "strict_block_guard": 0,
        "reopen_unused_per_useful": 4,
        "reopen_guard": 0,
        "decay_period": 0,
    }
    values.update(overrides)
    return DirectQualityConfig(**values)


def event(access_seq: int, addr: int) -> replay.ReplayEvent:
    return replay.ReplayEvent(
        access_seq=access_seq,
        order=access_seq,
        bop_name="large",
        bop_kind="large",
        tick=access_seq,
        trigger_addr=addr,
        trigger_pc=0x1000,
        trigger_has_pc=True,
        validation_hit=0,
        best_offset_changed=False,
        issue_enabled=True,
        validation_enabled=True,
        pc_confidence_enabled=True,
        pc_sampled=True,
        raw_candidate_valid=True,
        raw_candidate_addr=addr,
        policy_candidate_valid=False,
        policy_candidate_addr=0,
    )


class ReopenAuditReplayTest(unittest.TestCase):
    def test_only_post_reopen_candidates_are_attributed_to_epoch(self):
        runner = ReopenAuditReplay(
            config(), replay.EvaluationWindow(), candidate_windows=(1, 4),
        )
        # Enter the reporting window before any sampled candidate is issued.
        # The production replay intentionally discards a warmup sample at its
        # first stable demand boundary.
        runner.observe_demand(replay.Demand(0, 0, 0))
        # Four sampled candidates expire and train this context into BLOCK.
        for index in range(4):
            runner.observe_event(event(index * 3 + 1, 0x2000 + index * 0x100))
            runner.observe_demand(replay.Demand(index * 3 + 2, index * 3 + 2, 0))
            runner.observe_demand(replay.Demand(index * 3 + 3, index * 3 + 3, 0))
        self.assertEqual(runner.controller.lookup(0x1000, "large").state, "block")

        # This BLOCK probe is useful and opens the context, but it predates
        # the epoch and must not be counted as post-reopen traffic.
        runner.observe_event(event(20, 0x5000))
        runner.observe_demand(replay.Demand(21, 21, 0x5000))
        self.assertEqual(runner.controller.lookup(0x1000, "large").state, "open")

        # The first unrestricted OPEN candidate belongs to the new epoch.
        runner.observe_event(event(22, 0x6000))
        runner.observe_demand(replay.Demand(23, 23, 0x6000))
        report = runner.finish(top=10)
        epochs = report["reopen_epochs"]
        self.assertEqual(epochs["count"], 1)
        self.assertEqual(epochs["full_open_quality"]["candidates"], 1)
        self.assertEqual(epochs["full_open_quality"]["useful"], 1)
        self.assertEqual(
            epochs["first_issued_candidate_quality"]["1"]["candidates"], 1,
        )


if __name__ == "__main__":
    unittest.main()

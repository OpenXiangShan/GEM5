#!/usr/bin/env python3
"""V5 compact DirectQuality trace-certifier regressions."""

from __future__ import annotations

import sqlite3
import tempfile
import unittest
from pathlib import Path

from certify_bop_direct_quality_trace import RawTraceReplay, _config_from_meta, certify
from replay_bop_direct_quality_gate import (
    DirectQualityConfig,
    FEEDBACK_ADDRESS_LAYOUT_SV48_TRUNCATED,
    FEEDBACK_AGE_ENCODING_EPOCH6,
    FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
    FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
    QUALITY_HASH_LAYOUT_XOR_FOLD,
)


def cqf_config() -> DirectQualityConfig:
    return DirectQualityConfig(
        quality_entries=256,
        quality_ways=4,
        quality_tag_bits=8,
        quality_hash_layout=QUALITY_HASH_LAYOUT_XOR_FOLD,
        feedback_owner_layout=FEEDBACK_OWNER_LAYOUT_QUALITY_KEY,
        offset_context_slots=1,
        feedback_entries=256,
        feedback_ways=4,
        feedback_tag_bits=14,
        feedback_address_layout=FEEDBACK_ADDRESS_LAYOUT_SV48_TRUNCATED,
        horizon=2048,
        feedback_expiry_mode=FEEDBACK_EXPIRY_MODE_ROUND_ROBIN,
        feedback_age_encoding=FEEDBACK_AGE_ENCODING_EPOCH6,
        feedback_epoch_timeout=30,
        observe_sample_period=16,
        observe_issue_all=True,
        open_sample_period=16,
        block_probe_period=64,
        borderline_block_probe_period=8,
        min_samples=32,
        unused_per_useful=10,
        block_guard=4,
        strict_unused_per_useful=20,
        strict_block_guard=4,
        reopen_unused_per_useful=10,
        reopen_guard=4,
        reopen_probe_period=64,
        reopen_confirm_samples=0,
        decay_period=64,
    )


class V5CertifierTest(unittest.TestCase):
    def test_v6_metadata_recovers_explicit_e5_s7_tuple(self):
        with sqlite3.connect(":memory:") as connection:
            connection.row_factory = sqlite3.Row
            connection.executescript("""
                CREATE TABLE BOPDirectQualityMeta(
                    SchemaVersion INT, Profile TEXT, QualityEntries INT,
                    QualityWays INT, QualityTagBits INT, QualityHashLayout TEXT,
                    FeedbackEntries INT, FeedbackWays INT, FeedbackTagBits INT,
                    FeedbackAddressLayout TEXT, FeedbackOwnerLayout TEXT,
                    FeedbackExpiryMode TEXT, FeedbackAgeEncoding TEXT,
                    FeedbackEpochBits INT, FeedbackEpochShift INT,
                    FeedbackEpochTimeout INT, Horizon INT, MinSamples INT,
                    ObserveSamplePeriod INT, OpenSamplePeriod INT,
                    BlockProbePeriod INT, BorderlineBlockProbePeriod INT,
                    UnusedPerUseful INT, BlockGuard INT,
                    StrictUnusedPerUseful INT, StrictBlockGuard INT,
                    ReopenUnusedPerUseful INT, ReopenGuard INT,
                    ReopenProbePeriod INT, ReopenConfirmSamples INT,
                    DecayPeriod INT);
            """)
            connection.execute(
                "INSERT INTO BOPDirectQualityMeta VALUES("
                + ",".join("?" for _ in range(31)) + ")",
                (
                    6, "BOP-CQF-DSE", 64, 4, 8, "xor_fold", 64, 4, 14,
                    "sv48_truncated_tag", "quality_key", "round_robin",
                    "epoch5", 5, 7, 15, 2048, 32, 16, 16, 64, 8, 10, 4,
                    20, 4, 10, 4, 64, 0, 64,
                ),
            )
            config = _config_from_meta(
                connection.execute("SELECT * FROM BOPDirectQualityMeta").fetchone()
            )
        self.assertEqual(config.feedback_age_encoding, "epoch5")
        self.assertEqual(config.epoch_bits, 5)
        self.assertEqual(config.epoch_shift, 7)
        self.assertEqual(config.feedback_epoch_timeout, 15)

    def test_v5_cqf_metadata_reconstructs_logical_owner_epoch_sweep(self):
        config = cqf_config()
        expected: list[dict[str, object]] = []
        runner = RawTraceReplay(config, expected.append)

        candidates = []
        for trigger_line in range(0x2000, 0x10000, 64):
            candidate = {
                "PC": 0x1000,
                "Kind": 1,
                "TriggerLine": trigger_line,
                "CandidateLine": 0x9000,
            }
            runner.candidate(candidate)  # sqlite Row-compatible mapping
            candidates.append(candidate)
            if any(event["type"] == "issue" for event in expected):
                break
        self.assertTrue(any(event["type"] == "issue" for event in expected))
        runner.demand({"Line": 0x9000})

        with tempfile.TemporaryDirectory() as directory:
            database = Path(directory) / "cqf-v5.db"
            with sqlite3.connect(database) as connection:
                connection.executescript("""
                    CREATE TABLE BOPDirectQualityMeta(
                        SchemaVersion INT PRIMARY KEY, Profile TEXT,
                        QualityEntries INT, QualityWays INT, QualityTagBits INT,
                        QualityHashLayout TEXT, FeedbackEntries INT,
                        FeedbackWays INT, FeedbackTagBits INT,
                        FeedbackAddressLayout TEXT, FeedbackOwnerLayout TEXT,
                        FeedbackExpiryMode TEXT, FeedbackAgeEncoding TEXT,
                        FeedbackEpochTimeout INT, Horizon INT, MinSamples INT,
                        ObserveSamplePeriod INT, OpenSamplePeriod INT,
                        BlockProbePeriod INT, BorderlineBlockProbePeriod INT,
                        UnusedPerUseful INT, BlockGuard INT,
                        StrictUnusedPerUseful INT, StrictBlockGuard INT,
                        ReopenUnusedPerUseful INT, ReopenGuard INT,
                        ReopenProbePeriod INT, ReopenConfirmSamples INT,
                        DecayPeriod INT);
                    CREATE TABLE BOPDirectQualityCandidate(
                        EventSequence INT PRIMARY KEY, Tick INT, PC INT,
                        Kind INT, TriggerLine INT, CandidateLine INT,
                        State INT, Allowed INT, Sampled INT);
                    CREATE TABLE BOPDirectQualityIssue(
                        EventSequence INT PRIMARY KEY, FeedbackId INT,
                        IssueDemandSequence INT, Tick INT, Line INT, Kind INT);
                    CREATE TABLE BOPDirectQualityDemand(
                        EventSequence INT PRIMARY KEY, DemandSequence INT,
                        Tick INT, Line INT);
                    CREATE TABLE BOPDirectQualityOutcome(
                        EventSequence INT PRIMARY KEY, FeedbackId INT,
                        ResolveDemandSequence INT, Tick INT, Line INT,
                        Outcome TEXT);
                """)
                connection.execute(
                    "INSERT INTO BOPDirectQualityMeta VALUES(?,?,?,?,?,?,?,?,?"
                    ",?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (
                        5, "BOP-CQF14E6T30", config.quality_entries,
                        config.quality_ways, config.quality_tag_bits,
                        config.quality_hash_layout, config.feedback_entries,
                        config.feedback_ways, config.feedback_tag_bits,
                        config.feedback_address_layout,
                        config.feedback_owner_layout,
                        config.feedback_expiry_mode,
                        config.feedback_age_encoding,
                        config.feedback_epoch_timeout, config.horizon,
                        config.min_samples, config.observe_sample_period,
                        config.open_sample_period, config.block_probe_period,
                        config.borderline_block_probe_period,
                        config.unused_per_useful, config.block_guard,
                        config.strict_unused_per_useful,
                        config.strict_block_guard,
                        config.reopen_unused_per_useful, config.reopen_guard,
                        config.reopen_probe_period,
                        config.reopen_confirm_samples, config.decay_period,
                    ),
                )
                for event in expected:
                    if event["type"] == "candidate":
                        connection.execute(
                            "INSERT INTO BOPDirectQualityCandidate VALUES(?,?,?,?,?,?,?,?,?)",
                            (event["event_sequence"], event["event_sequence"],
                             event["pc"], event["kind"], event["trigger_line"],
                             event["candidate_line"], event["state"],
                             event["allowed"], event["sampled"]),
                        )
                    elif event["type"] == "issue":
                        connection.execute(
                            "INSERT INTO BOPDirectQualityIssue VALUES(?,?,?,?,?,?)",
                            (event["event_sequence"], event["feedback_id"],
                             event["issue_demand_sequence"], event["event_sequence"],
                             event["line"], event["kind"]),
                        )
                    elif event["type"] == "demand":
                        connection.execute(
                            "INSERT INTO BOPDirectQualityDemand VALUES(?,?,?,?)",
                            (event["event_sequence"], event["demand_sequence"],
                             event["event_sequence"], event["line"]),
                        )
                    elif event["type"] == "outcome":
                        connection.execute(
                            "INSERT INTO BOPDirectQualityOutcome VALUES(?,?,?,?,?,?)",
                            (event["event_sequence"], event["feedback_id"],
                             event["resolve_demand_sequence"],
                             event["event_sequence"], event["line"],
                             event["outcome"]),
                        )

            report = certify(database)
        self.assertTrue(report["pass"], report["mismatches"])
        self.assertEqual(report["config"]["feedback_tag_bits"], 14)
        self.assertEqual(report["config"]["feedback_owner_layout"], "quality_key")
        self.assertEqual(report["config"]["feedback_age_encoding"], "epoch6")


if __name__ == "__main__":
    unittest.main()

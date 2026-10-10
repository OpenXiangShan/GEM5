"""Diagnostic ranking tests with overlapping victims and commit groups."""

import sqlite3
import tempfile
from pathlib import Path
import unittest

import perfcct_query as query
from perfcct_diagnose import (
    fetch_counter_check,
    investigation_hints,
    paired_wait_overlap,
    stats_roi,
    resolve_window,
)


class DiagnoseTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            """
            CREATE TABLE PerfCCTMeta(Key TEXT PRIMARY KEY, Value TEXT);
            CREATE TABLE PerfCCTInst(Cpu TEXT,TID INT,SeqNum INT,PC INT,
                Disasm TEXT,BornTick INT,CommitID INT,EndTick INT,EndKind TEXT,
                PRIMARY KEY(Cpu,TID,SeqNum));
            CREATE TABLE LifeTimeCommitTrace(ID INTEGER PRIMARY KEY,
                AtIssueQue INT,AtFU INT,AtWriteVal INT,AtCommit INT);
            CREATE TABLE PerfCCTEvent(ID INTEGER PRIMARY KEY,Cpu TEXT,TID INT,
                SeqNum INT,Tick INT,Attempt INT,Event TEXT,Detail TEXT,
                RelatedSeq INT,ReasonMask INT);
            CREATE INDEX events_inst ON PerfCCTEvent(Cpu,TID,SeqNum,Tick,ID);
            CREATE TABLE PerfCCTCommitSpan(ID INTEGER PRIMARY KEY,Cpu TEXT,TID INT,
                StartTick INT,EndTick INT,HeadSeq INT,BlockerSeq INT,Reason TEXT,
                Committed INT,SampleCycles INT,EndKind TEXT);
            INSERT INTO PerfCCTInst VALUES
                ('cpu',0,1,100,'add',1,1,100,'commit'),
                ('cpu',0,2,100,'add',1,2,110,'commit'),
                ('cpu',0,3,200,'load',1,3,120,'commit'),
                ('cpu',0,4,300,'load',1,4,130,'commit'),
                ('cpu',0,5,400,'add',1,99,100,'commit'),
                ('cpu',1,1,500,'add',1,5,100,'commit');
            INSERT INTO LifeTimeCommitTrace VALUES
                (1,10,20,30,100),(2,20,30,40,110),(3,20,10,30,120),
                (4,0,40,60,130),(5,1,2,3,100);
            INSERT INTO PerfCCTCommitSpan VALUES
                (1,'cpu',0,30,80,1,4,'group_not_ready',0,5,'closed'),
                (2,'cpu',0,40,60,1,4,'group_not_ready',0,2,'closed'),
                (3,'cpu',0,70,90,2,4,'group_not_ready',0,2,'closed'),
                (4,'cpu',0,90,100,2,1,'group_not_ready',2,1,'closed'),
                (5,'cpu',0,100,110,2,999,'commit_head_blocked',0,1,'trace_end'),
                (6,'cpu',0,110,NULL,2,999,'commit_head_blocked',0,1,'trace_end'),
                (7,'cpu',0,10,20,0,0,'empty',0,1,'closed');
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,5,0,'dependency','register',7,0),
                (2,'cpu',0,4,35,1,'attempt_begin','issue_queue',0,0),
                (3,'cpu',0,4,40,1,'replay','CacheMissReplay',0,2),
                (4,'cpu',0,4,60,2,'attempt_begin','replay_queue',0,0),
                (5,'cpu',0,4,65,2,'replay','CacheMissReplay',0,2),
                (6,'cpu',0,4,70,2,'wake','cache_hint',0,0);
        """
        )

    def tearDown(self):
        self.db.close()

    def run_query(self, *args):
        parsed = query.parser().parse_args(
            [
                "unused",
                "diagnose",
                "--cpu",
                "cpu",
                "--tid",
                "0",
                "--start",
                "1",
                "--end",
                "200",
                *args,
            ]
        )
        return query.query(self.db, parsed)

    def test_separate_rankings_and_true_group_blocker(self):
        result = self.run_query()
        symptoms = result["symptom_rankings"]
        self.assertEqual([r["PC"] for r in symptoms], [100, 300])
        self.assertEqual(symptoms[0]["instruction_ticks"], 180)
        self.assertEqual(symptoms[0]["rob_drain"], 140)
        blockers = result["commit_blocker_rankings"]
        self.assertEqual(
            [(r["PC"], r["observed_ticks"]) for r in blockers],
            [(300, 60), (None, 10)],
        )
        self.assertEqual(
            blockers[0]["duration_samples"],
            {
                "p50": {"ticks": 20, "span_id": 3},
                "p95": {"ticks": 50, "span_id": 1},
                "max": {"ticks": 50, "span_id": 1},
            },
        )
        self.assertEqual(
            blockers[1]["duration_samples"]["p50"], {"ticks": 10, "span_id": 5}
        )
        cohorts = blockers[0]["duration_cohorts"]
        self.assertEqual(cohorts["population_spans"], 3)
        self.assertEqual(cohorts["summed_span_ticks"], 90)
        self.assertEqual(
            [
                (
                    cohorts["bins"][name]["spans"],
                    cohorts["bins"][name]["span_ticks"],
                )
                for name in ("short", "middle", "long")
            ],
            [(2, 40), (1, 50), (0, 0)],
        )
        screen = result["top_blocker_evidence_sample"]
        self.assertEqual(
            (screen["sampled_spans"], screen["population_spans"]), (3, 3)
        )
        self.assertEqual(
            screen["replay_reasons"]["CacheMissReplay"],
            {"lifetime_spans": 3, "within_span_spans": 2},
        )
        self.assertEqual(
            screen["spans_without_time_aligned_replay_or_paired_wait"], 1
        )
        typical = next(
            e
            for e in result["examples"]
            if e["source"] == "commit_blocker"
            and e["selection"] == "p50_duration"
        )
        self.assertEqual(typical["observed_span"]["ID"], 3)
        self.assertEqual(typical["route"]["evidence"]["identity"]["SeqNum"], 4)
        typical_replay = next(
            row
            for row in typical["event_timestamp_counts"]
            if row["Event"] == "replay"
        )
        self.assertEqual(typical_replay["within_span_count"], 0)
        self.assertEqual(
            typical["instance_evidence_level"],
            "no_aligned_replay_or_paired_wait",
        )
        longest = next(
            e
            for e in result["examples"]
            if e["source"] == "commit_blocker"
            and e["selection"] == "max_duration"
            and e["PC"] == 300
        )
        longest_replay = next(
            row
            for row in longest["event_timestamp_counts"]
            if row["Event"] == "replay"
        )
        self.assertEqual(longest_replay["within_span_count"], 2)
        self.assertEqual(
            longest["instance_evidence_level"], "replay_timestamp_in_blocker"
        )
        route = result["examples"][0]["route"]
        self.assertEqual(route["entry"], "commit_blocker")
        self.assertEqual(route["evidence"]["identity"]["SeqNum"], 4)
        self.assertEqual(route["next_entry"], "load_replay_cache")
        self.assertEqual(route["critical_predecessor"], "unknown")
        self.assertEqual(route["cache"]["status"], "cache_events_unavailable")
        self.assertEqual(route["predecessor_candidates"][0]["instruction"], [])

    def test_missing_nonmonotonic_and_unknown_end_coverage(self):
        result = self.run_query()
        coverage = result["lifetime_coverage"]
        self.assertEqual(coverage["committed_instances"], 5)
        self.assertEqual(coverage["missing_lifetimes"], 1)
        self.assertEqual(coverage["nonmonotonic_instances"], 1)
        self.assertEqual(coverage["iq_to_fu_missing"], 2)
        self.assertEqual(result["commit_coverage"]["unknown_end_spans"], 1)
        unknown = next(
            e
            for e in result["examples"]
            if e["source"] == "commit_blocker" and e["PC"] is None
        )["route"]["evidence"]
        self.assertIsNone(unknown["instruction"])
        self.assertEqual(unknown["events"], [])

    def test_top_n_audits_keep_the_full_tail_and_distinct_units(self):
        for index in range(15):
            seq = 20 + index
            self.db.execute(
                """INSERT INTO PerfCCTInst VALUES
                ('cpu',0,?,?,'add',1,?,150,'commit')""",
                (seq, 600 + index, seq),
            )
            self.db.execute(
                "INSERT INTO LifeTimeCommitTrace VALUES(?,100,110,120,150)",
                (seq,),
            )
        full = self.run_query("--top", "100")
        five = self.run_query("--top", "5")
        ten = self.run_query("--top", "10")
        five_audit = five["selection_audits"]["symptom"]
        ten_audit = ten["selection_audits"]["symptom"]
        self.assertEqual(five_audit["population_ranked_pcs"], 17)
        self.assertEqual(five_audit["omitted_ranked_pcs"], 12)
        self.assertEqual(ten_audit["omitted_ranked_pcs"], 7)
        self.assertEqual(
            five_audit["total_observed_units"],
            sum(r["instruction_ticks"] for r in full["symptom_rankings"]),
        )
        self.assertEqual(
            five_audit["total_observed_units"],
            ten_audit["total_observed_units"],
        )
        self.assertGreater(
            ten_audit["returned_fraction_of_ranked_units"],
            five_audit["returned_fraction_of_ranked_units"],
        )
        self.assertEqual(
            five_audit["returned_observed_units"],
            sum(r["instruction_ticks"] for r in five["symptom_rankings"]),
        )
        zero = five["selection_audits"]["zero_commit"]
        partial = five["selection_audits"]["partial_commit"]
        self.assertEqual(
            (zero["total_observed_units"], partial["total_observed_units"]),
            (70, 10),
        )
        self.assertNotEqual(zero["unit"], partial["unit"])
        self.assertNotEqual(five_audit["unit"], zero["unit"])
        self.assertEqual(
            five["selection_audits"]["fetch_partial"]["status"],
            "unavailable_old_db",
        )

    def test_zero_commit_audit_does_not_add_cross_pc_overlap_as_runtime(self):
        self.db.execute(
            """INSERT INTO PerfCCTCommitSpan VALUES
            (20,'cpu',0,40,80,1,1,'group_not_ready',0,4,'closed')"""
        )
        result = self.run_query("--top", "1")
        audit = result["selection_audits"]["zero_commit"]
        self.assertEqual(audit["population_ranked_pcs"], 3)
        self.assertEqual(audit["total_observed_units"], 110)
        self.assertEqual(audit["all_pc_interval_union_ticks"], 70)
        self.assertEqual(audit["returned_observed_units"], 60)
        self.assertAlmostEqual(
            audit["returned_fraction_of_ranked_units"], 60 / 110
        )
        self.assertAlmostEqual(
            audit["all_pc_interval_union_fraction_of_roi"], 70 / 199
        )

    def test_partial_top_candidate_routes_typical_and_tail_spans(self):
        self.db.execute("DELETE FROM PerfCCTCommitSpan WHERE Committed>0")
        self.db.executemany(
            """INSERT INTO PerfCCTCommitSpan VALUES
            (?,'cpu',0,?,?,1,1,'group_not_ready',2,1,'closed')""",
            [(20, 20, 21), (21, 22, 25), (22, 25, 35)],
        )
        result = self.run_query()
        rank = result["partial_commit_rankings"][0]
        self.assertEqual(
            rank["duration_samples"],
            {
                "p50": {"ticks": 3, "span_id": 21},
                "p95": {"ticks": 10, "span_id": 22},
                "max": {"ticks": 10, "span_id": 22},
            },
        )
        examples = [
            e
            for e in result["examples"]
            if e["source"] == "partial_commit_blocker"
        ]
        self.assertEqual(
            [e["observed_span"]["ID"] for e in examples], [21, 22]
        )
        self.assertTrue(
            all(e["observed_span"]["Committed"] == 2 for e in examples)
        )
        self.assertTrue(
            all(
                e["route"]["critical_predecessor"] == "unknown"
                for e in examples
            )
        )
        self.assertEqual(
            result["selection_audits"]["partial_commit"][
                "total_observed_units"
            ],
            14,
        )
        self.assertEqual(result["scope"]["critical_path"], "not_computed")
        self.assertEqual(
            result["investigation_hints"]["zero_commit_candidate_check"][
                "root_cause_status"
            ],
            "not_established",
        )

    def test_other_zero_commit_pcs_use_p95_instead_of_outlier(self):
        self.db.executemany(
            """INSERT INTO PerfCCTCommitSpan VALUES
            (?,'cpu',0,1,2,1,1,'group_not_ready',0,1,'closed')""",
            [(100 + index,) for index in range(20)],
        )
        self.db.execute(
            """INSERT INTO PerfCCTCommitSpan VALUES
            (120,'cpu',0,100,120,1,1,'group_not_ready',0,2,'closed')"""
        )
        result = self.run_query()
        rank = next(
            r for r in result["commit_blocker_rankings"] if r["PC"] == 100
        )
        self.assertEqual(rank["duration_samples"]["max"]["span_id"], 120)
        self.assertEqual(rank["representative_selection"], "p95_duration")
        self.assertEqual(rank["representative_span_id"], 119)
        example = next(
            e
            for e in result["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 100
        )
        self.assertEqual(example["observed_span"]["ID"], 119)

    def test_fetch_selection_audit_uses_only_eligible_empty_slots(self):
        self.db.executescript(
            """
            CREATE TABLE PerfCCTFetchTransfer(Tick INT,Cpu TEXT,TID INT,
                AnchorPC INT,AnchorSeq INT,FTQID INT,EmptySlots INT,
                TopdownEligible INT);
            INSERT INTO PerfCCTFetchTransfer VALUES
                (10,'cpu',0,100,1,1,7,1),
                (11,'cpu',0,100,2,2,6,0),
                (12,'cpu',0,200,3,3,3,1),
                (13,'cpu',1,300,4,4,7,1),
                (200,'cpu',0,400,5,5,7,1);
        """
        )
        result = self.run_query("--top", "1")
        audit = result["selection_audits"]["fetch_partial"]
        self.assertEqual(audit["population_ranked_pcs"], 2)
        self.assertEqual(audit["omitted_ranked_pcs"], 1)
        self.assertEqual(audit["total_observed_units"], 10)
        self.assertEqual(audit["returned_observed_units"], 7)
        self.assertEqual(audit["returned_fraction_of_ranked_units"], 0.7)
        self.assertEqual(
            result["fetch_partial_transfers"]["coverage"]["all_empty_slots"],
            16,
        )

    def wait_overlap(self):
        return paired_wait_overlap(
            self.db, {"Cpu": "cpu", "TID": 0, "SeqNum": 4}, 1, 200
        )

    def test_translation_wait_cannot_close_with_cache_hint(self):
        self.db.executescript(
            """
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,10,1,'wait_begin','translation',0,0),
                (2,'cpu',0,4,20,1,'wake','cache_hint',0,0);
        """
        )
        wait = self.wait_overlap()
        self.assertEqual(wait["paired_intervals"], 0)
        self.assertEqual(wait["overlap_ticks_union"], 0)
        self.assertEqual(
            (wait["unmatched_begins"], wait["unmatched_wakes"]), (1, 1)
        )

    def test_stlf_wait_requires_the_same_known_store(self):
        self.db.executescript(
            """
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,10,1,'wait_begin','stlf',7,0),
                (2,'cpu',0,4,20,1,'wake','stlf',8,0),
                (3,'cpu',0,4,30,1,'wake','stlf',7,0),
                (4,'cpu',0,4,40,2,'wait_begin','stlf',0,0),
                (5,'cpu',0,4,50,2,'wake','stlf',0,0);
        """
        )
        wait = self.wait_overlap()
        self.assertEqual(wait["paired_intervals"], 1)
        self.assertEqual(wait["examples"][0]["wake_id"], 3)
        self.assertEqual(wait["examples"][0]["related_seq"], 7)
        self.assertEqual(
            (wait["unmatched_begins"], wait["unmatched_wakes"]), (1, 2)
        )

    def test_mixed_waits_match_reason_and_attempt_without_fifo_pairing(self):
        self.db.executescript(
            """
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,10,1,'wait_begin','translation',0,0),
                (2,'cpu',0,4,15,1,'wait_begin','cache_refill',0,0),
                (3,'cpu',0,4,20,2,'wake','cache_hint',0,0),
                (4,'cpu',0,4,25,1,'wake','cache_hint',0,0),
                (5,'cpu',0,4,30,1,'wake','translation_observed_complete',0,0),
                (6,'cpu',0,4,40,1,'wait_begin','future_reason',0,0),
                (7,'cpu',0,4,50,1,'wake','future_wake',0,0);
        """
        )
        wait = self.wait_overlap()
        self.assertEqual(
            [(e["begin_id"], e["wake_id"]) for e in wait["examples"]],
            [(2, 4), (1, 5)],
        )
        self.assertEqual(wait["overlap_ticks_union"], 20)
        self.assertEqual(
            (wait["unmatched_begins"], wait["unmatched_wakes"]), (1, 2)
        )

    def test_repeated_wait_begins_use_latest_anchor_and_leave_old_unmatched(
        self,
    ):
        self.db.executescript(
            """
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,10,1,'wait_begin','cache_refill',0,0),
                (2,'cpu',0,4,20,1,'wait_begin','cache_refill',0,0),
                (3,'cpu',0,4,30,1,'wake','cache_hint',0,0),
                (4,'cpu',0,4,40,1,'wake','cache_response',0,0);
        """
        )
        wait = self.wait_overlap()
        self.assertEqual(wait["paired_intervals"], 1)
        self.assertEqual(wait["examples"][0]["begin_id"], 2)
        self.assertEqual(wait["overlap_ticks_union"], 10)
        self.assertEqual(wait["superseded_begins"], 1)
        self.assertEqual(wait["unmatched_begins"], 1)
        self.assertEqual(wait["unmatched_wakes"], 1)

    def test_store_address_and_wait_route_without_fake_load_cache_query(self):
        self.db.executescript(
            """
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,35,1,'attempt_begin','store_address',0,0),
                (2,'cpu',0,4,40,1,'wait_begin','translation',0,0),
                (3,'cpu',0,4,50,1,'wake','translation_observed_complete',0,0);
        """
        )
        example = next(
            e
            for e in self.run_query()["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 300
        )
        self.assertEqual(
            example["route"]["next_entry"], "store_translation_or_completion"
        )
        self.assertNotIn("cache", example["route"])
        self.db.execute("DELETE FROM PerfCCTEvent WHERE Event='attempt_begin'")
        self.db.execute(
            """INSERT INTO PerfCCTEvent VALUES
            (4,'cpu',0,4,35,1,'store_data_ready','executed',0,0)"""
        )
        example = next(
            e
            for e in self.run_query()["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 300
        )
        self.assertEqual(
            example["route"]["next_entry"], "store_translation_or_completion"
        )
        self.assertNotIn("cache", example["route"])

    def test_untyped_memory_wait_and_legacy_store_role_are_not_assumed_loads(
        self,
    ):
        self.db.executescript(
            """
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,4,40,1,'wait_begin','translation',0,0);
        """
        )
        example = next(
            e
            for e in self.run_query()["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 300
        )
        self.assertEqual(
            example["route"]["next_entry"], "memory_wait_observed"
        )
        self.assertNotIn("cache", example["route"])
        self.db.execute(
            """INSERT INTO PerfCCTEvent VALUES
            (2,'cpu',0,4,5,0,'dependency','src=0;role=store_address',1,0)"""
        )
        example = next(
            e
            for e in self.run_query()["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 300
        )
        self.assertEqual(
            example["route"]["next_entry"], "store_translation_or_completion"
        )
        self.assertNotIn("cache", example["route"])

    def test_roi_clipping_and_event_limit_preserve_replays(self):
        result = self.run_query("--start", "50", "--end", "120")
        self.assertEqual(
            result["symptom_rankings"][0]["instruction_ticks"], 110
        )
        self.assertEqual(
            result["commit_blocker_rankings"][0]["observed_ticks"], 40
        )
        evidence = result["examples"][0]["route"]["evidence"]
        replays = [e for e in evidence["events"] if e["Event"] == "replay"]
        self.assertEqual([e["Attempt"] for e in replays], [1, 2])
        self.assertEqual(evidence["events"][0]["Tick"], 5)
        limited = self.run_query("--limit", "2", "--top", "1")
        evidence = limited["examples"][0]["route"]["evidence"]
        self.assertTrue(evidence["events_truncated"])
        self.assertEqual([e["ID"] for e in evidence["events"]], [2, 3])

    def test_late_replay_windows_and_same_tick_pagination(self):
        self.db.executescript(
            """
            UPDATE PerfCCTInst SET EndTick=950 WHERE TID=0 AND SeqNum=1;
            UPDATE LifeTimeCommitTrace SET AtCommit=950 WHERE ID=1;
            UPDATE PerfCCTInst SET EndTick=1100 WHERE SeqNum=4;
            UPDATE LifeTimeCommitTrace SET AtCommit=1100 WHERE ID=4;
            UPDATE PerfCCTCommitSpan SET StartTick=900,EndTick=1000 WHERE ID=1;
        """
        )
        self.db.executemany(
            """INSERT INTO PerfCCTEvent VALUES
            (?,'cpu',0,4,?,1,'replay','CacheMissReplay',0,2)""",
            [(100 + tick, tick) for tick in range(100, 500)],
        )
        self.db.executemany(
            """INSERT INTO PerfCCTEvent VALUES
            (?,'cpu',0,4,?,99,'replay','CacheMissReplay',0,2)""",
            [(1000, 920), (1001, 960), (1002, 960), (1003, 960), (1004, 970)],
        )
        result = self.run_query("--end", "1200", "--limit", "6")
        victim = next(
            e
            for e in result["examples"]
            if e["source"] == "symptom" and e["PC"] == 100
        )
        blocker = next(
            e
            for e in result["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 300
        )
        early = victim["route"]["evidence"]
        late = blocker["route"]["evidence"]
        self.assertEqual(
            early["seed_window"], {"start_tick": 900, "end_tick": 950}
        )
        self.assertEqual(
            late["seed_window"], {"start_tick": 900, "end_tick": 1000}
        )
        self.assertEqual(early["event_groups"]["window"], [1000])
        self.assertIn(1001, late["event_groups"]["window"])
        self.assertLessEqual(len(late["events"]), 6)
        self.assertTrue(late["omitted"]["before_anchor"])
        self.assertEqual(late["event_groups"]["dependency_context"], [1])
        page = late["next_window_page"]
        continued = [
            dict(e) for e in self.db.execute(page["sql"], page["parameters"])
        ]
        self.assertEqual([e["ID"] for e in continued], [1004])
        small = self.run_query("--end", "1200", "--limit", "2")
        late = next(
            e
            for e in small["examples"]
            if e["source"] == "commit_blocker" and e["PC"] == 300
        )["route"]["evidence"]
        self.assertEqual(late["event_groups"]["window"], [1000, 1001])
        page = late["next_window_page"]
        continued = [
            dict(e) for e in self.db.execute(page["sql"], page["parameters"])
        ]
        self.assertEqual([e["ID"] for e in continued], [1002, 1003])

    def test_load_identity_and_rejection_owner_context(self):
        self.db.executescript(
            """
            CREATE TABLE LoadLifeTimeCommitTrace(ID INTEGER PRIMARY KEY);
            INSERT INTO LoadLifeTimeCommitTrace VALUES(4);
            DELETE FROM PerfCCTEvent;
            INSERT INTO PerfCCTMeta VALUES
                ('cpu.cpu.data_requestor','1'),('cpu.cpu.context.0','2');
            CREATE TABLE PerfCCTCacheEvent(ID INTEGER PRIMARY KEY,Cache TEXT,
                Tick INT,Event TEXT,MSHR INT,Requestor INT,Context INT,
                SeqNum INT,ParentID INT);
            INSERT INTO PerfCCTCacheEvent VALUES
                (1,'l1d',45,'reject',0,1,2,4,0),
                (2,'l1d',45,'target_owner',1,1,2,1,1);
        """
        )
        route = self.run_query()["examples"][0]["route"]
        self.assertEqual(route["next_entry"], "load_replay_cache")
        owners = route["cache"]["rejection_owners"][0]
        self.assertEqual(owners["reject_id"], 1)
        self.assertEqual(
            owners["events"][0]["request_identity"]["identity"]["SeqNum"], 1
        )
        self.assertFalse(owners["events_truncated"])
        bounded = self.run_query("--limit", "1")["examples"][0]["route"]
        self.assertTrue(
            bounded["cache"]["rejection_owners"][0]["events_truncated"]
        )

    def test_predecessor_candidates_are_explicitly_bounded(self):
        self.db.executescript(
            """
            INSERT INTO PerfCCTEvent VALUES
                (7,'cpu',0,4,6,0,'dependency','register',8,0),
                (8,'cpu',0,4,7,0,'dependency','register',9,0);
        """
        )
        route = self.run_query()["examples"][0]["route"]
        self.assertEqual(len(route["predecessor_candidates"]), 2)
        self.assertEqual(route["candidate_count_in_returned_events"], 3)
        self.assertTrue(route["candidates_truncated"])

    def test_paired_wait_overlap_and_adjacent_partial_chain(self):
        self.db.executemany(
            """INSERT INTO PerfCCTEvent VALUES
            (?,'cpu',0,4,?,?,'wait_begin','cache_refill',0,0)""",
            [(10, 25, 3), (12, 75, 4)],
        )
        self.db.executemany(
            """INSERT INTO PerfCCTEvent VALUES
            (?,'cpu',0,4,?,?,'wake','cache_hint',0,0)""",
            [(11, 45, 3), (13, 95, 4)],
        )
        self.db.execute(
            """INSERT INTO PerfCCTEvent VALUES
            (14,'cpu',0,1,5,0,'dependency','src=0',4,0)"""
        )
        self.db.execute(
            """INSERT INTO PerfCCTEvent VALUES
            (15,'cpu',0,4,50,5,'wait_begin','cache_refill',0,0)"""
        )
        self.db.execute(
            """INSERT INTO PerfCCTEvent VALUES
            (16,'cpu',0,4,40,6,'wait_begin','cache_refill',0,0)"""
        )
        self.db.execute(
            """INSERT INTO PerfCCTEvent VALUES
            (17,'cpu',0,4,60,6,'wake','cache_hint',0,0)"""
        )
        result = self.run_query()
        longest = next(
            e
            for e in result["examples"]
            if e.get("selection") == "max_duration" and e["PC"] == 300
        )
        wait = longest["paired_wait_overlap"]
        self.assertEqual(
            longest["instance_evidence_level"], "paired_wait_overlaps_blocker"
        )
        self.assertEqual(wait["paired_intervals"], 3)
        self.assertEqual(wait["unmatched_begins"], 1)
        self.assertEqual(wait["unmatched_wakes"], 1)
        self.assertEqual(wait["overlap_ticks_union"], 35)
        self.assertEqual(wait["ticks_outside_observed_paired_wait"], 15)
        typical = next(
            e
            for e in result["examples"]
            if e.get("selection") == "p50_duration"
        )
        chain = typical["adjacent_commit_chain"]
        self.assertEqual(
            [(s["ID"], s["Committed"]) for s in chain["spans"]],
            [(3, 0), (4, 2), (5, 0), (6, 0)],
        )
        self.assertEqual(
            [
                (e["producer_seq"], e["consumer_seq"])
                for e in chain["dependency_edges"]
            ],
            [(4, 1)],
        )

    def test_empty_window_and_missing_stage_columns(self):
        empty = self.run_query("--start", "200", "--end", "300")
        self.assertEqual(empty["symptom_rankings"], [])
        self.assertEqual(empty["lifetime_coverage"]["iq_to_fu_missing"], 0)
        for name in ("symptom", "zero_commit", "partial_commit"):
            audit = empty["selection_audits"][name]
            self.assertEqual(audit["population_ranked_pcs"], 0)
            self.assertEqual(audit["total_observed_units"], 0)
            self.assertIsNone(audit["returned_fraction_of_ranked_units"])
        self.db.execute(
            "ALTER TABLE LifeTimeCommitTrace RENAME COLUMN AtFU TO hidden_fu"
        )
        partial = self.run_query()
        self.assertEqual(
            partial["lifetime_coverage"]["missing_columns"], ["AtFU"]
        )
        self.assertEqual(partial["symptom_rankings"][0]["iq_to_fu"], 0)
        self.assertEqual(
            partial["symptom_rankings"][0]["execution_or_memory"], 0
        )

    def test_stats_uses_last_complete_section_and_rejects_partial(self):
        begin = "---------- Begin Simulation Statistics ----------\n"
        end = "---------- End Simulation Statistics ----------\n"
        warmup = begin + "finalTick 100\nsimTicks 100\n" + end
        measured = begin + "finalTick 300\nsimTicks 200\n" + end
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.txt"
            path.write_text(warmup + measured + "ordinary log\n")
            self.assertEqual(stats_roi(path), (100, 300))
            args = query.parser().parse_args(
                [
                    "unused",
                    "diagnose",
                    "--cpu",
                    "cpu",
                    "--tid",
                    "0",
                    "--stats",
                    str(path),
                ]
            )
            result = query.query(self.db, args)
            self.assertEqual(
                result["query_window"], {"start_tick": 100, "end_tick": 300}
            )
            args.start = 100
            with self.assertRaisesRegex(ValueError, "cannot be combined"):
                resolve_window(args)
            for invalid in (
                warmup + begin + "finalTick 300\n",
                begin + "finalTick 3.0\nsimTicks 2\n" + end,
                begin + "finalTick 10\n" + end,
                begin + "finalTick 10\nsimTicks 20\n" + end,
            ):
                path.write_text(invalid)
                with self.assertRaises(ValueError):
                    stats_roi(path)

    def test_fetch_partial_anchor_and_topdown_use_last_stats_section(self):
        self.db.executescript(
            """
            CREATE TABLE PerfCCTFetchTransfer(Tick INT,Cpu TEXT,TID INT,
                AnchorPC INT,AnchorSeq INT,FTQID INT,EmptySlots INT,
                TopdownEligible INT);
            INSERT INTO PerfCCTFetchTransfer VALUES
                (100,'cpu',0,4096,10,3,2,1),
                (110,'cpu',0,8192,11,4,3,0),
                (120,'cpu',1,4096,12,5,4,1),
                (300,'cpu',0,4096,13,6,2,1);
        """
        )
        begin = "---------- Begin Simulation Statistics ----------\n"
        end = "---------- End Simulation Statistics ----------\n"
        warmup = (
            begin
            + "finalTick 100\nsimTicks 100\ncpu.frontendBound 0.9\n"
            + end
        )
        measured = (
            begin
            + "\n".join(
                [
                    "finalTick 300",
                    "simTicks 200",
                    "cpu.baseRetiring 0.6",
                    "cpu.frontendBound 0.3",
                    "cpu.badSpecBound 0.05",
                    "cpu.backendBound 0.05",
                    "cpu.frontendLatencyBound 0.1",
                    "cpu.frontendBandwidthBound 0.2",
                    "cpu.fetch.fetchBubbles 10",
                    "cpu.fetch.fetchBubbles_max 1",
                    "cpu.fetch.instsSentToDecodePerCycle::8 0",
                    "cpu.fetch.instsSentToDecodePerCycle::max_value 7",
                    "cpu.iew.fetchStallReason::FetchFragStall 5",
                    "cpu.iew.dispatchStallReason::NoStall 70",
                    "cpu.iew.dispatchStallReason::FetchFragStall 20",
                    "cpu.iew.dispatchStallReason::LoadL1Bound 10",
                    "cpu.iew.dispatchStallReason::total 100",
                ]
            )
            + "\n"
            + end
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "stats.txt"
            path.write_text(warmup + measured)
            args = query.parser().parse_args(
                [
                    "unused",
                    "diagnose",
                    "--cpu",
                    "cpu",
                    "--tid",
                    "0",
                    "--stats",
                    str(path),
                ]
            )
            result = query.query(self.db, args)
        self.assertEqual(result["topdown"]["level1"]["frontendBound"], 0.3)
        self.assertEqual(
            result["topdown"]["frontend_level2"]["frontendBandwidthBound"], 0.2
        )
        dispatch = result["dispatch_stall_reasons"]
        self.assertTrue(dispatch["bins_match_total"])
        self.assertEqual(dispatch["no_stall"]["fraction"], 0.7)
        self.assertEqual(
            dispatch["top_non_nostall"][0],
            {"reason": "FetchFragStall", "count": 20, "fraction": 0.2},
        )
        hints = result["investigation_hints"]
        self.assertEqual(
            [r["next_entry"] for r in hints["dispatch_routes"]],
            ["fetch_ftq_supply", "backend_resource_or_dependency"],
        )
        self.assertEqual(
            hints["suggested_start"]["next_entry"], "fetch_ftq_supply"
        )
        self.assertEqual(
            hints["fetch_anchor_candidate"],
            {"AnchorPC": 4096, "eligible_empty_slots": 2},
        )
        self.assertEqual(
            hints["commit_blocker_candidate"]["observed_ticks"], 10
        )
        transfer = result["fetch_partial_transfers"]
        self.assertEqual(
            transfer["coverage"],
            {
                "all_samples": 2,
                "all_empty_slots": 5,
                "eligible_samples": 1,
                "eligible_empty_slots": 2,
            },
        )
        self.assertEqual(
            [rank["AnchorPC"] for rank in transfer["rankings"]], [4096]
        )
        self.assertEqual(
            transfer["rankings"][0]["sample"],
            {"Tick": 100, "AnchorSeq": 10, "FTQID": 3, "EmptySlots": 2},
        )
        self.assertEqual(transfer["counter_check"]["status"], "exact")
        self.assertEqual(transfer["left_boundary_eligible_slots"], 2)
        self.assertEqual(transfer["right_boundary_eligible_slots"], 2)
        self.assertEqual(
            self.run_query()["topdown"]["status"], "unavailable_without_stats"
        )
        self.assertEqual(
            self.run_query()["dispatch_stall_reasons"]["status"],
            "unavailable_without_stats",
        )

    def test_control_recovery_route_is_a_hint(self):
        dispatch = {
            "status": "available",
            "bins_match_total": True,
            "no_stall": {"fraction": 0.65},
            "top_non_nostall": [
                {"reason": "ControlRecovery", "count": 27, "fraction": 0.27}
            ],
        }
        hints = investigation_hints(
            dispatch, [], {"status": "unavailable_old_db"}
        )
        self.assertEqual(
            hints["dispatch_routes"][0]["next_entry"], "control_recovery"
        )
        self.assertEqual(
            hints["evidence_ladder"]["intervention_support"],
            "not_measured_by_diagnose",
        )
        dispatch["no_stall"]["fraction"] = 0.95
        with_blocker = investigation_hints(
            dispatch,
            [{"PC": 100, "observed_ticks": 50}],
            {"status": "unavailable_old_db"},
        )
        self.assertEqual(
            with_blocker["suggested_start"]["next_entry"], "commit_blocker"
        )

    def test_fetch_counter_boundary_is_a_consistency_check(self):
        transfer = {
            "status": "available",
            "coverage": {"eligible_empty_slots": 10},
            "left_boundary_eligible_slots": 3,
            "right_boundary_eligible_slots": 4,
        }
        topdown = {
            "status": "available",
            "decode_width_from_histogram_bins": 8,
            "fetch_counters": {
                "fetch.fetchBubbles": 19,
                "fetch.fetchBubbles_max": 1,
            },
        }
        self.assertEqual(
            fetch_counter_check(transfer, topdown)["status"],
            "consistent_with_stats_boundary_shift",
        )

    def test_legacy_table_missing_still_ranks_commit_blockers(self):
        self.db.execute("DROP TABLE LifeTimeCommitTrace")
        result = self.run_query()
        self.assertEqual(result["symptom_rankings"], [])
        self.assertEqual(
            result["lifetime_coverage"]["status"], "lifetime_table_unavailable"
        )
        self.assertEqual(len(result["commit_blocker_rankings"]), 2)
        self.assertEqual(
            result["fetch_partial_transfers"]["status"], "unavailable_old_db"
        )


if __name__ == "__main__":
    unittest.main()

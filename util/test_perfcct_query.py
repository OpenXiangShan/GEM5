"""Synthetic evidence tests for the PerfCCT query CLI."""

import sqlite3
import tempfile
import unittest
from pathlib import Path

import perfcct_query as query


class QueryTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            """
            CREATE TABLE PerfCCTMeta(Key TEXT PRIMARY KEY, Value TEXT);
            CREATE TABLE PerfCCTInst(Cpu TEXT, TID INT, SeqNum INT, PC INT,
                Disasm TEXT, BornTick INT, CommitID INT, EndTick INT, EndKind TEXT,
                PRIMARY KEY(Cpu,TID,SeqNum));
            CREATE TABLE PerfCCTEvent(ID INTEGER PRIMARY KEY, Cpu TEXT,TID INT,
                SeqNum INT,Tick INT,Attempt INT,Event TEXT,Detail TEXT,
                RelatedSeq INT,ReasonMask INT);
            CREATE TABLE PerfCCTCommitSpan(ID INTEGER PRIMARY KEY,Cpu TEXT,TID INT,
                StartTick INT,EndTick INT,HeadSeq INT,BlockerSeq INT,Reason TEXT,
                Committed INT,SampleCycles INT,EndKind TEXT);
            INSERT INTO PerfCCTCommitSpan VALUES
                (1,'cpu',0,10,40,7,8,'group_not_ready',0,3,'closed'),
                (2,'cpu',0,40,60,8,8,'not_ready',2,2,'closed'),
                (3,'cpu',0,60,80,8,8,'future_reason',0,2,'trace_end'),
                (4,'cpu',1,70,NULL,8,8,'unknown',0,1,'unknown');
            INSERT INTO PerfCCTInst VALUES
                ('cpu',0,8,4096,'load',0,1,60,'commit'),
                ('cpu',1,8,8192,'load',0,NULL,NULL,'unknown');
            INSERT INTO PerfCCTEvent VALUES
                (1,'cpu',0,8,20,1,'replay','stlf',6,4),
                (2,'cpu',1,8,20,1,'replay','unknown',0,0),
                (3,'cpu',0,8,20,1,'wake','stlf',6,0),
                (4,'cpu',0,8,40,2,'attempt','',0,0);
        """
        )

    def tearDown(self):
        self.db.close()

    def run_query(self, *args):
        return query.query(
            self.db, query.parser().parse_args(["unused", *args])
        )

    def test_overlap_and_partial(self):
        result = self.run_query("stalls", "--start", "20", "--end", "50")
        self.assertEqual(
            [s["covered_ticks"] for s in result["spans"]], [20, 10]
        )
        self.assertEqual(result["spans"][0]["StartTick"], 10)
        self.assertEqual(result["spans"][0]["SampleCycles"], 3)
        zero = self.run_query(
            "stalls", "--kind", "zero", "--start", "40", "--end", "60"
        )
        self.assertEqual(zero["spans"], [])
        partial = self.run_query("stalls", "--kind", "partial")
        self.assertEqual([s["ID"] for s in partial["spans"]], [2])

    def test_reason_filter_accepts_multiple_and_unknown_reasons(self):
        result = self.run_query(
            "stalls",
            "--reason",
            "group_not_ready",
            "--reason",
            "future_reason",
            "--top",
            "1",
        )
        self.assertEqual([span["ID"] for span in result["spans"]], [1])
        result = self.run_query("stalls", "--reason", "missing_reason")
        self.assertEqual(result["spans"], [])

    def test_identity_and_event_order(self):
        result = self.run_query(
            "inst", "8", "--cpu", "cpu", "--tid", "0", "--end", "40"
        )
        self.assertEqual(len(result["instructions"]), 1)
        self.assertEqual([e["ID"] for e in result["events"]], [1, 3])
        self.assertTrue(all(s["TID"] == 0 for s in result["commit_spans"]))
        self.assertEqual(len(self.run_query("inst", "8")["instructions"]), 2)

    def test_legacy_lifetime_is_optional_and_linked_by_commit_id(self):
        self.assertEqual(self.run_query("inst", "8")["lifetimes"], [])
        self.db.executescript(
            """
            CREATE TABLE LifeTimeCommitTrace(ID INT, AtFetch INT, AtCommit INT);
            INSERT INTO LifeTimeCommitTrace VALUES(1, 0, 60), (8, 10, 100);
        """
        )
        result = self.run_query("inst", "8")
        self.assertEqual(len(result["lifetimes"]), 1)
        lifetime = result["lifetimes"][0]
        self.assertEqual(
            (lifetime["Cpu"], lifetime["TID"], lifetime["SeqNum"]),
            ("cpu", 0, 8),
        )
        self.assertEqual(lifetime["lifetime"]["AtCommit"], 60)
        self.assertEqual(lifetime["lifetime"]["ID"], 1)

    def test_unknown_and_truncated_are_preserved(self):
        result = self.run_query("stalls", "--start", "60", "--end", "100")
        self.assertEqual(result["spans"][0]["Reason"], "future_reason")
        self.assertTrue(result["spans"][0]["boundary_incomplete"])
        self.assertIsNone(result["spans"][1]["covered_ticks"])
        self.assertEqual(result["spans"][1]["EndKind"], "unknown")

    def test_database_is_read_only_and_missing_is_not_created(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.sqlite"
            with self.assertRaises(sqlite3.OperationalError):
                query.connect(path)
            self.assertFalse(path.exists())
            sqlite3.connect(path).close()
            db = query.connect(path)
            try:
                with self.assertRaises(sqlite3.OperationalError):
                    db.execute("CREATE TABLE bad(x)")
            finally:
                db.close()

    def test_chain_identity_cycles_unknown_and_limits(self):
        self.db.executescript(
            """
            INSERT INTO PerfCCTInst VALUES
                ('cpu',0,6,4100,'store',0,NULL,70,'squash');
            INSERT INTO PerfCCTEvent VALUES
                (5,'cpu',0,8,22,1,'wait_begin','stlf',6,0),
                (6,'cpu',0,6,23,1,'dependency','register',8,0),
                (7,'cpu',0,6,24,1,'dependency','unknown',0,0),
                (8,'cpu',0,6,25,1,'dependency','register',5,0);
        """
        )
        result = self.run_query("chain", "8", "--tid", "0")
        self.assertEqual(
            [n["identity"]["SeqNum"] for n in result["nodes"]], [8, 6, 5]
        )
        self.assertIsNone(result["nodes"][-1]["instruction"])
        self.assertEqual(
            [e["event"]["ID"] for e in result["edges"]], [5, 6, 7, 8]
        )
        self.assertEqual(result["edges"][1]["status"], "already_visited")
        self.assertIsNone(result["edges"][2]["to"])
        self.assertFalse(result["truncated"])
        shallow = self.run_query("chain", "8", "--tid", "0", "--depth", "0")
        self.assertEqual(len(shallow["nodes"]), 1)
        self.assertEqual(shallow["edges"][0]["status"], "depth_limit")
        self.assertTrue(shallow["truncated"])
        bounded = self.run_query("chain", "8", "--tid", "0", "--nodes", "1")
        self.assertEqual(bounded["edges"][0]["status"], "node_limit")
        self.assertTrue(bounded["truncated"])
        window = self.run_query("chain", "8", "--tid", "0", "--end", "22")
        self.assertEqual(window["edges"], [])
        dependencies = self.run_query(
            "chain", "6", "--tid", "0", "--start", "100"
        )
        self.assertEqual(
            [e["event"]["ID"] for e in dependencies["edges"]], [6, 7, 8]
        )

    def schedule_fixture(self):
        self.db.executescript(
            """
            INSERT INTO PerfCCTEvent VALUES
                (10,'cpu',0,8,25,0,'iq_ready_enqueue','candidate_queue_insert',0,0),
                (11,'cpu',0,8,25,0,'iq_cancel','load_cancel',6,0),
                (12,'cpu',0,8,26,0,'iq_operand_spec_wake','source_ready_transition',6,0),
                (13,'cpu',0,8,26,0,'iq_ready_enqueue','candidate_queue_insert',0,0),
                (14,'cpu',0,8,30,0,'iq_issue_to_fu','normal',0,0),
                (15,'cpu',1,8,25,0,'iq_ready_enqueue','candidate_queue_insert',0,0),
                (16,'other',0,8,25,0,'iq_future_event','unknown',0,0);
        """
        )

    def test_schedule_preserves_repeated_events_and_identity(self):
        self.schedule_fixture()
        result = self.run_query("inst", "8", "--cpu", "cpu", "--tid", "0")
        schedule = result["schedule_evidence"][0]
        self.assertEqual(
            schedule["identity"], {"Cpu": "cpu", "TID": 0, "SeqNum": 8}
        )
        self.assertEqual(
            [event["ID"] for event in schedule["events"]], [10, 11, 12, 13, 14]
        )
        self.assertEqual(schedule["event_counts"]["iq_ready_enqueue"], 2)
        self.assertEqual(schedule["events"][2]["RelatedSeq"], 6)
        self.assertFalse(schedule["events_truncated"])
        self.assertFalse(schedule["window_filtered"])
        self.assertEqual(schedule["readiness_coverage"], "unknown")
        unqualified = self.run_query("inst", "8")["schedule_evidence"]
        self.assertEqual(len(unqualified), 3)
        self.assertEqual(
            unqualified[-1]["events"][0]["Event"], "iq_future_event"
        )

    def test_schedule_missing_and_clipped_observations_stay_unknown(self):
        old = self.run_query("inst", "8", "--tid", "0")["schedule_evidence"][0]
        self.assertEqual(old["status"], "no_observed_events")
        self.assertEqual(old["readiness_coverage"], "unknown")
        self.schedule_fixture()
        clipped = self.run_query(
            "inst",
            "8",
            "--tid",
            "0",
            "--cpu",
            "cpu",
            "--start",
            "26",
            "--end",
            "30",
            "--limit",
            "1",
        )["schedule_evidence"][0]
        self.assertEqual([event["ID"] for event in clipped["events"]], [12])
        self.assertTrue(clipped["events_truncated"])
        self.assertTrue(clipped["window_filtered"])
        self.assertEqual(clipped["event_counts"], {"iq_operand_spec_wake": 1})
        self.assertEqual(clipped["readiness_coverage"], "unknown")
        empty = self.run_query(
            "inst", "8", "--tid", "0", "--start", "31", "--end", "32"
        )["schedule_evidence"][0]
        self.assertEqual(empty["events"], [])
        self.assertTrue(empty["window_filtered"])
        self.assertEqual(empty["status"], "no_observed_events")

    def cache_fixture(self):
        self.db.executescript(
            """
            INSERT INTO PerfCCTMeta VALUES
                ('cpu.cpu.data_requestor','11'), ('cpu.cpu.context.0','12'),
                ('cpu.cpu.context.1','13');
            CREATE TABLE PerfCCTCacheEvent(ID INTEGER PRIMARY KEY, Cache TEXT,
                Tick INT, Event TEXT, MSHR INT, BlockAddr INT, Secure INT,
                Requestor INT, Context INT, SeqNum INT, Targets INT,
                Allocated INT, HeldCredits INT, BlockedMask INT, Detail TEXT,
                ParentID INT);
            INSERT INTO PerfCCTCacheEvent VALUES
                (1,'l1d',10,'allocate',1,4096,0,11,12,8,1,1,0,0,'',0),
                (2,'l1d',20,'merge',1,4096,0,11,13,8,2,1,0,0,'',0),
                (3,'l1d',30,'reject',0,8192,0,11,12,9,0,1,0,1,'',0),
                (4,'l1d',30,'owner',1,4096,0,11,12,8,2,1,0,1,'',3),
                (5,'l1d',40,'release',1,4096,0,11,12,8,2,1,0,0,'',0),
                (6,'l1d',40,'credit_hold',1,4096,0,11,12,8,0,0,1,0,'',0),
                (7,'l1d',45,'owner_credit',1,4096,0,11,12,8,0,0,1,1,'',3),
                (8,'l1d',50,'credit_release',1,4096,0,11,12,8,0,0,0,0,'',0),
                (9,'other',20,'allocate',1,4096,0,99,-1,8,1,1,0,0,'',0);
        """
        )

    def test_resources_old_database_and_precise_member_mapping(self):
        self.assertFalse(self.run_query("resources")["available"])
        self.cache_fixture()
        result = self.run_query(
            "resources",
            "--cache",
            "l1d",
            "--mshr",
            "1",
            "--start",
            "30",
            "--end",
            "40",
        )
        self.assertEqual([e["ID"] for e in result["events"]], [4])
        life = result["lifecycles"][0]
        self.assertEqual(
            [e["ID"] for e in life["events"]], [1, 2, 4, 5, 6, 7, 8]
        )
        self.assertTrue(life["allocation_observed"])
        self.assertTrue(life["release_observed"])
        self.assertEqual(
            [m["mapping"]["identity"]["TID"] for m in life["members"]], [0, 1]
        )
        self.assertEqual(result["rejections"][0]["reject"]["ID"], 3)
        self.assertEqual(
            [e["ID"] for e in result["rejections"][0]["owners"]], [4, 7]
        )
        other = self.run_query("resources", "--cache", "other")
        self.assertEqual(
            other["events"][0]["request_identity"]["status"], "unknown"
        )
        self.assertEqual(len(other["lifecycles"]), 1)
        rejection = self.run_query("resources", "--reject", "3")
        self.assertEqual([e["ID"] for e in rejection["events"]], [3, 4, 7])

    def test_resources_unknown_ambiguity_and_truncation(self):
        self.cache_fixture()
        self.db.execute(
            "INSERT INTO PerfCCTMeta VALUES('cpu.alias.data_requestor','11')"
        )
        self.db.execute(
            "INSERT INTO PerfCCTMeta VALUES('cpu.alias.context.0','12')"
        )
        result = self.run_query("resources", "--cache", "l1d", "--limit", "1")
        self.assertTrue(result["events_truncated"])
        self.assertTrue(result["lifecycles"][0]["events_truncated"])
        mapping = result["events"][0]["request_identity"]
        self.assertEqual(mapping["status"], "unknown")
        self.assertEqual(len(mapping["candidates"]), 2)
        no_event = self.run_query(
            "resources",
            "--cache",
            "l1d",
            "--mshr",
            "1",
            "--start",
            "21",
            "--end",
            "22",
        )
        self.assertEqual(no_event["events"], [])
        self.assertEqual(len(no_event["lifecycles"]), 1)

    def test_inst_cache_events_mapping_and_limits(self):
        old = self.run_query("inst", "8", "--tid", "0")
        self.assertEqual(
            old["cache_events"][0]["status"], "cache_events_unavailable"
        )
        self.cache_fixture()
        result = self.run_query("inst", "8", "--tid", "1")
        cache = result["cache_events"][0]
        self.assertEqual(cache["status"], "resolved")
        self.assertEqual([e["ID"] for e in cache["events"]], [2])
        bounded = self.run_query("inst", "8", "--tid", "0", "--limit", "1")
        self.assertTrue(bounded["cache_events"][0]["events_truncated"])
        self.db.execute(
            "DELETE FROM PerfCCTMeta WHERE Key='cpu.cpu.context.1'"
        )
        missing = self.run_query("inst", "8", "--tid", "1")
        self.assertEqual(
            missing["cache_events"][0]["status"], "missing_request_mapping"
        )

    def test_resource_context_has_global_budget(self):
        self.cache_fixture()
        result = self.run_query("resources", "--limit", "4")
        context_rows = sum(
            len(life["events"]) for life in result["lifecycles"]
        )
        context_rows += sum(
            len(reject["owners"]) for reject in result["rejections"]
        )
        self.assertLessEqual(context_rows, 4)
        self.assertTrue(
            any(life["events_truncated"] for life in result["lifecycles"])
        )
        self.assertTrue(result["rejections"][0]["owners_truncated"])

    def target_fixture(self):
        self.cache_fixture()
        self.db.executescript(
            """
            ALTER TABLE PerfCCTCacheEvent ADD COLUMN RequestID INT DEFAULT 0;
            ALTER TABLE PerfCCTCacheEvent ADD COLUMN TargetID INT DEFAULT 0;
            ALTER TABLE PerfCCTCacheEvent ADD COLUMN RelatedRequestID INT DEFAULT 0;
        """
        )
        for (
            event_id,
            tick,
            event,
            request_id,
            target_id,
            related,
            context,
            parent,
        ) in [
            (10, 10, "target_add", 100, 20, 0, 12, 0),
            (11, 20, "target_add", 101, 21, 0, 13, 0),
            (12, 21, "target_service", 100, 20, 0, 12, 0),
            (13, 22, "target_remove", 100, 20, 0, 12, 0),
            (14, 23, "target_replace", 102, 21, 101, 13, 0),
            (15, 30, "target_owner", 102, 21, 0, 13, 3),
            (16, 51, "open_target", 102, 21, 0, 13, 0),
        ]:
            self.db.execute(
                """INSERT INTO PerfCCTCacheEvent
                (ID,Cache,Tick,Event,MSHR,BlockAddr,Secure,Requestor,Context,
                 SeqNum,Targets,Allocated,HeldCredits,BlockedMask,Detail,ParentID,
                 RequestID,TargetID,RelatedRequestID)
                 VALUES(?, 'l1d', ?, ?, 1, 4096, 0, 11, ?, 8, 1, 1, 0, 0,
                        '', ?, ?, ?, ?)""",
                (
                    event_id,
                    tick,
                    event,
                    context,
                    parent,
                    request_id,
                    target_id,
                    related,
                ),
            )

    def test_target_state_service_replace_remove_and_open(self):
        self.target_fixture()
        result = self.run_query(
            "resources", "--cache", "l1d", "--mshr", "1", "--at", "21"
        )
        state = result["lifecycles"][0]["target_state"]
        self.assertEqual(state["status"], "complete_observation")
        self.assertEqual(
            [t["TargetID"] for t in state["active_targets"]], [20, 21]
        )
        result = self.run_query(
            "resources", "--cache", "l1d", "--mshr", "1", "--at", "23"
        )
        state = result["lifecycles"][0]["target_state"]
        self.assertEqual(len(state["targets"]), 2)
        self.assertEqual(
            [(t["TargetID"], t["RequestID"]) for t in state["active_targets"]],
            [(21, 102)],
        )
        self.assertEqual(state["targets"][0]["end_kind"], "removed")
        final = self.run_query("resources", "--cache", "l1d", "--mshr", "1")
        target = final["lifecycles"][0]["target_state"]["active_targets"][0]
        self.assertEqual(target["end_kind"], "trace_end_open")
        self.assertEqual(target["request_identity"]["identity"]["TID"], 1)

    def test_target_owner_is_snapshot_not_historical_members(self):
        self.target_fixture()
        result = self.run_query("resources", "--reject", "3")
        rejection = result["rejections"][0]
        self.assertEqual(
            [t["TargetID"] for t in rejection["target_owners"]], [21]
        )
        self.assertEqual(rejection["target_owners"][0]["RequestID"], 102)
        self.assertEqual(len(result["lifecycles"][0]["historical_members"]), 2)

    def test_target_tracking_legacy_unknown_and_truncation(self):
        self.cache_fixture()
        legacy = self.run_query("resources", "--cache", "l1d")
        self.assertFalse(legacy["target_tracking_available"])
        self.assertEqual(
            legacy["lifecycles"][0]["target_state"]["status"], "unavailable"
        )
        self.db.close()
        self.setUp()
        self.target_fixture()
        limited = self.run_query(
            "resources", "--cache", "l1d", "--mshr", "1", "--limit", "2"
        )
        self.assertEqual(
            limited["lifecycles"][0]["target_state"]["status"],
            "incomplete_observation",
        )
        self.db.execute("UPDATE PerfCCTCacheEvent SET TargetID=0 WHERE ID=10")
        unknown = self.run_query("resources", "--cache", "l1d", "--mshr", "1")
        state = unknown["lifecycles"][0]["target_state"]
        self.assertEqual(state["status"], "incomplete_observation")
        self.assertTrue(
            any(i["reason"] == "unknown_target_id" for i in state["issues"])
        )

    def test_new_identity_filters_on_old_schema_are_unavailable(self):
        self.cache_fixture()
        for option in ("--request-id", "--target-id"):
            result = self.run_query("resources", option, "1")
            self.assertFalse(result["available"])
            self.assertEqual(result["events"], [])
            self.assertIn("unavailable", result["notes"][0])

    def test_request_target_filters_and_distinct_request_members(self):
        self.target_fixture()
        self.db.execute(
            "UPDATE PerfCCTCacheEvent SET RequestID=100 WHERE ID=1"
        )
        self.db.execute(
            "UPDATE PerfCCTCacheEvent SET RequestID=101, Context=12 WHERE ID=2"
        )
        result = self.run_query(
            "resources", "--cache", "l1d", "--request-id", "101"
        )
        self.assertTrue(all(e["RequestID"] == 101 for e in result["events"]))
        members = result["lifecycles"][0]["historical_members"]
        self.assertEqual([m["RequestID"] for m in members], [100, 101])
        self.assertEqual([m["SeqNum"] for m in members], [8, 8])
        self.assertEqual([m["Context"] for m in members], [12, 12])
        targets = self.run_query(
            "resources", "--cache", "l1d", "--target-id", "21"
        )
        self.assertTrue(targets["events"])
        self.assertTrue(all(e["TargetID"] == 21 for e in targets["events"]))

    def test_target_inconsistent_remove_and_replace_are_incomplete(self):
        self.target_fixture()
        self.db.execute(
            "UPDATE PerfCCTCacheEvent SET RequestID=999 WHERE ID=13"
        )
        self.db.execute(
            "UPDATE PerfCCTCacheEvent SET RelatedRequestID=999 WHERE ID=14"
        )
        self.db.execute(
            "UPDATE PerfCCTCacheEvent SET Event='target_remove', TargetID=20 WHERE ID=16"
        )
        result = self.run_query("resources", "--cache", "l1d", "--mshr", "1")
        state = result["lifecycles"][0]["target_state"]
        self.assertEqual(state["status"], "incomplete_observation")
        issues = {i["reason"] for i in state["issues"]}
        self.assertIn("target_request_mismatch", issues)
        self.assertIn("replacement_request_mismatch", issues)
        self.assertIn("event_after_target_remove", issues)

    def test_target_service_after_remove_preserves_closed_state(self):
        self.target_fixture()
        self.db.execute("UPDATE PerfCCTCacheEvent SET Tick=22 WHERE ID=12")
        self.db.execute("UPDATE PerfCCTCacheEvent SET Tick=21 WHERE ID=13")
        result = self.run_query(
            "resources", "--cache", "l1d", "--mshr", "1", "--at", "22"
        )
        state = result["lifecycles"][0]["target_state"]
        self.assertEqual(state["status"], "complete_observation")
        self.assertEqual(state["issues"], [])
        removed = next(t for t in state["targets"] if t["TargetID"] == 20)
        self.assertFalse(removed["active"])
        self.assertEqual(removed["remove_tick"], 21)
        self.assertEqual(removed["end_kind"], "removed")
        self.assertEqual(
            [t["TargetID"] for t in state["active_targets"]], [21]
        )


if __name__ == "__main__":
    unittest.main()

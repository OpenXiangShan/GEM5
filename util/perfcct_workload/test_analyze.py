"""Exercise ROI boundaries and conservative wait pairing with synthetic evidence."""

from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

from analyze import analyze


class AnalyzeTest(unittest.TestCase):
    def test_roi_clipping_unknown_wait_and_nonhead(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.db"
            with sqlite3.connect(path) as db:
                db.executescript(
                    """
CREATE TABLE PerfCCTMeta(Key TEXT,Value TEXT);
INSERT INTO PerfCCTMeta VALUES('cpu.cpu.clock_period','10');
CREATE TABLE PerfCCTInst(Cpu TEXT,TID INTEGER,SeqNum INTEGER,PC INTEGER,
    Disasm TEXT,EndTick INTEGER,EndKind TEXT);
INSERT INTO PerfCCTInst VALUES
    ('cpu',0,10,100,'begin',100,'commit'),
    ('cpu',0,11,300,'load',180,'commit'),
    ('cpu',0,14,200,'end',200,'commit'),
    ('cpu',0,12,300,'load2',190,'commit'),
    ('cpu',0,15,300,'younger',220,'commit');
CREATE TABLE PerfCCTEvent(ID INTEGER,Cpu TEXT,TID INTEGER,SeqNum INTEGER,
    Tick INTEGER,Attempt INTEGER,Event TEXT,Detail TEXT,RelatedSeq INTEGER);
INSERT INTO PerfCCTEvent VALUES
    (1,'cpu',0,11,110,1,'wait_begin','stlf',9),
    (2,'cpu',0,11,150,1,'wake','stlf',9),
    (3,'cpu',0,11,160,2,'wait_begin','translation',0),
    (4,'cpu',0,11,170,3,'wake','translation_observed_complete',0),
    (5,'cpu',0,15,180,1,'wait_begin','stlf',9),
    (6,'cpu',0,11,200,2,'wake','translation_observed_complete',0),
    (7,'cpu',0,11,120,1,'wake','stlf',8),
    (8,'cpu',0,11,175,4,'wait_begin','stlf',8),
    (9,'cpu',0,11,185,4,'wake','stlf',9),
    (10,'cpu',0,12,120,1,'wait_begin','stlf',9),
    (11,'cpu',0,12,160,1,'wake','stlf',9);
CREATE TABLE PerfCCTCommitSpan(Cpu TEXT,TID INTEGER,StartTick INTEGER,
    EndTick INTEGER,HeadSeq INTEGER,BlockerSeq INTEGER,Reason TEXT,
    Committed INTEGER,EndKind TEXT);
INSERT INTO PerfCCTCommitSpan VALUES
    ('cpu',0,90,130,10,11,'group_not_ready',0,'closed'),
    ('cpu',0,130,210,10,11,'group_not_ready',1,'closed');
"""
                )
            with patch(
                "analyze.symbols",
                return_value={
                    "roi_begin": 100,
                    "roi_end": 200,
                    "probe_load": 300,
                },
            ):
                result = analyze(path, "unused.elf")
            self.assertEqual(
                result["roi"]["marker_commit_to_commit_cycles"], 10
            )
            self.assertEqual(result["roi"]["committed_operations"], 3)
            self.assertEqual(result["group_nonhead"]["zero_commit_cycles"], 3)
            self.assertEqual(
                result["group_nonhead"]["positive_commit_cycles"], 7
            )
            self.assertEqual(
                result["wait_summary"]["stlf"]["observed_intervals"], 2
            )
            self.assertEqual(
                result["wait_summary"]["stlf"]["unknown_intervals"], 1
            )
            self.assertEqual(
                result["wait_summary"]["stlf"]["observed_cycles"], 8
            )
            self.assertEqual(
                result["wait_summary"]["stlf"]["observed_union_cycles"], 5
            )
            self.assertEqual(result["stlf_examples"][0]["end_tick"], 150)
            unknown = next(
                wait
                for wait in result["stlf_examples"]
                if wait["attempt"] == 4
            )
            self.assertIsNone(unknown["end_tick"])
            self.assertEqual(
                result["stlf_examples"][0]["own_blocker_overlap_cycles"], 4
            )
            self.assertEqual(
                result["wait_summary"]["translation"]["unknown_intervals"], 1
            )


if __name__ == "__main__":
    unittest.main()

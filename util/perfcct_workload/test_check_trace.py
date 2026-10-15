"""Resource generations and CPU dependencies must not retain stale identities."""

import sqlite3
import unittest

from check_trace import check_connection


class TraceIntegrityTest(unittest.TestCase):
    def test_generations_credits_and_dangling_reference(self):
        with sqlite3.connect(":memory:") as db:
            db.executescript(
                """
CREATE TABLE PerfCCTCacheEvent(ID INTEGER,Cache TEXT,Tick INTEGER,Event TEXT,
    MSHR INTEGER,Allocated INTEGER,HeldCredits INTEGER,ParentID INTEGER);
CREATE TABLE PerfCCTInst(Cpu TEXT,TID INTEGER,SeqNum INTEGER,EndKind TEXT);
CREATE TABLE PerfCCTEvent(ID INTEGER,Cpu TEXT,TID INTEGER,SeqNum INTEGER,
    Event TEXT,RelatedSeq INTEGER);
INSERT INTO PerfCCTInst VALUES('cpu',0,1,'commit'),('cpu',0,2,'commit');
INSERT INTO PerfCCTEvent VALUES(1,'cpu',0,2,'dependency',1);
INSERT INTO PerfCCTCacheEvent VALUES
    (1,'l1d',1,'allocate',1,1,0,0),
    (2,'l1d',2,'credit_hold',1,1,1,0),
    (3,'l1d',2,'release',1,1,1,0),
    (4,'l1d',3,'allocate',2,1,1,0),
    (5,'l1d',4,'reject',0,1,1,0),
    (6,'l1d',4,'owner',2,1,1,5),
    (7,'l1d',4,'owner_credit',1,1,1,5),
    (8,'l1d',5,'credit_release',1,1,0,0),
    (9,'l1d',6,'release',2,1,0,0),
    (10,'l1d',7,'trace_end',0,0,0,0);
"""
            )
            self.assertTrue(check_connection(db)["ok"])
            db.execute("UPDATE PerfCCTCacheEvent SET MSHR=1 WHERE ID=6")
            result = check_connection(db)
            self.assertFalse(result["ok"])
            self.assertTrue(
                any(
                    "active allocation" in item["message"]
                    for item in result["errors"]
                )
            )
            db.execute("UPDATE PerfCCTCacheEvent SET MSHR=2 WHERE ID=6")
            db.execute("UPDATE PerfCCTCacheEvent SET MSHR=1 WHERE ID=4")
            result = check_connection(db)
            self.assertFalse(result["ok"])
            self.assertTrue(
                any(
                    "generation reused" in item["message"]
                    for item in result["errors"]
                )
            )
            db.execute("UPDATE PerfCCTCacheEvent SET MSHR=2 WHERE ID=4")
            db.execute(
                "UPDATE PerfCCTInst SET EndKind='squash' WHERE SeqNum=1"
            )
            result = check_connection(db)
            self.assertFalse(result["ok"])
            self.assertTrue(
                any(
                    "squashed producer" in item["message"]
                    for item in result["errors"]
                )
            )

    def test_target_partial_service_locked_replacement_and_owner(self):
        with sqlite3.connect(":memory:") as db:
            db.executescript(
                """
CREATE TABLE PerfCCTCacheEvent(ID INTEGER,Cache TEXT,Tick INTEGER,Event TEXT,
    MSHR INTEGER,Allocated INTEGER,HeldCredits INTEGER,ParentID INTEGER,
    RequestID INTEGER,TargetID INTEGER,RelatedRequestID INTEGER,Targets INTEGER);
CREATE TABLE PerfCCTInst(Cpu TEXT,TID INTEGER,SeqNum INTEGER,EndKind TEXT);
CREATE TABLE PerfCCTEvent(ID INTEGER,Cpu TEXT,TID INTEGER,SeqNum INTEGER,
    Event TEXT,RelatedSeq INTEGER);
INSERT INTO PerfCCTCacheEvent VALUES
    (1,'l1d',1,'allocate',1,1,0,0,100,0,0,1),
    (2,'l1d',1,'target_add',1,1,0,0,100,10,0,1),
    (3,'l1d',2,'target_add',1,1,0,0,101,11,0,2),
    (4,'l1d',3,'target_remove',1,1,0,0,100,10,0,2),
    (5,'l1d',3,'target_service',1,1,0,0,100,10,0,1),
    (6,'l1d',4,'reject',0,1,0,0,200,0,0,0),
    (7,'l1d',4,'target_owner',1,1,0,6,101,11,0,1),
    (8,'l1d',5,'target_service',1,1,0,0,101,11,0,1),
    (9,'l1d',5,'target_replace',1,1,0,0,102,11,101,1),
    (10,'l1d',6,'target_remove',1,1,0,0,102,11,0,1),
    (11,'l1d',6,'release',1,1,0,0,0,0,0,0),
    (12,'l1d',7,'trace_end',0,0,0,0,0,0,0,0);
"""
            )
            self.assertTrue(check_connection(db)["ok"])
            db.execute(
                "UPDATE PerfCCTCacheEvent SET TargetID=10,RequestID=100 WHERE ID=7"
            )
            result = check_connection(db)
            self.assertFalse(result["ok"])
            self.assertTrue(
                any(
                    "at rejection" in item["message"]
                    for item in result["errors"]
                )
            )
            db.execute(
                "UPDATE PerfCCTCacheEvent SET TargetID=11,RequestID=101 WHERE ID=7"
            )
            db.execute(
                "UPDATE PerfCCTCacheEvent SET RelatedRequestID=99 WHERE ID=9"
            )
            result = check_connection(db)
            self.assertTrue(
                any(
                    "old request mismatch" in item["message"]
                    for item in result["errors"]
                )
            )
            db.execute(
                "UPDATE PerfCCTCacheEvent SET RelatedRequestID=101 WHERE ID=9"
            )
            db.execute("DELETE FROM PerfCCTCacheEvent WHERE ID>=10")
            db.executescript(
                """
INSERT INTO PerfCCTCacheEvent VALUES
    (10,'l1d',6,'trace_end',0,1,0,0,0,0,0,0),
    (11,'l1d',6,'open_mshr',1,1,0,0,0,0,0,1),
    (12,'l1d',6,'open_target',1,1,0,0,102,11,0,1);
"""
            )
            self.assertTrue(check_connection(db)["ok"])
            db.execute("DELETE FROM PerfCCTCacheEvent WHERE ID=12")
            self.assertFalse(check_connection(db)["ok"])


if __name__ == "__main__":
    unittest.main()

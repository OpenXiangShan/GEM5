#!/usr/bin/env python3
"""Hand-checked lifecycle regressions, including aliasing and ROI censoring."""

import csv
import tempfile
import unittest
from pathlib import Path

from correlate import summarize
from parse_trace import COLUMNS, load_trace


class Trace:
    def __init__(self):
        self.rows = []

    def add(self, tick, event, req=1, pf=0, pa="0x1000", va=None, **fields):
        row = {c: "" for c in COLUMNS}
        row.update(
            event=event,
            tick=tick,
            cycle=tick,
            actor="system.cpu.dcache",
            cache="system.cpu.dcache",
            level=1,
            request_id=req,
            prefetch_id=pf,
            related_id=0,
            secure=0,
            context_id=1,
            requestor_id=1,
            address_space="se:1000:0:0:0:0",
            pc="0x8000000012345678",
            paddr=pa or "",
            vaddr=va or "",
            op="load",
            size=8,
        )
        row.update(fields)
        self.rows.append(row)

    def candidate(self, tick, pf, pa, **fields):
        self.add(tick, "PF_CANDIDATE", req=pf + 1000, pf=pf, pa=pa, **fields)

    def demand(self, tick, req, pa, result="miss", **fields):
        self.add(tick, "DEMAND_ACCESS", req=req, pa=pa, result=result, **fields)
        self.add(tick + 100, "DEMAND_COMPLETE", req=req, pa=pa, **fields)

    def sent(self, pf, pa, tick=20):
        self.add(tick - 2, "PF_ENQUEUE", req=pf + 1000, pf=pf, pa=pa)
        self.add(tick - 1, "PF_DEQUEUE", req=pf + 1000, pf=pf, pa=pa)
        self.add(tick, "PF_SEND_ACCEPT", req=pf + 1000, pf=pf, pa=pa)

    def fill(self, pf, pa, tick=40):
        self.add(tick, "PF_FILL", req=pf + 1000, pf=pf, pa=pa, ready_tick=tick + 5, result="pure")

    def write(self, path, closed=True):
        with path.open("w", newline="") as stream:
            stream.write(
                "# prefetch_trace_version=3\n# block_size=64\n# start_tick=0\n# end_tick=10000\n# after_warmup=0\n"
            )
            writer = csv.DictWriter(stream, fieldnames=COLUMNS)
            writer.writeheader()
            for order, row in enumerate(sorted(self.rows, key=lambda r: r["tick"]), 1):
                writer.writerow({**row, "order": order})
            if closed:
                stream.write("# trace_end_tick=10000\n# trace_complete=1\n")


class AnalyzerTests(unittest.TestCase):
    def analyze(self, trace):
        folder = Path(self.temp.name)
        path = folder / "trace.csv"
        trace.write(path)
        db, metadata = load_trace(path, folder / "events.sqlite")
        self.addCleanup(db.close)
        return db, *summarize(db, metadata)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=Path.home() / ".tmp")
        self.addCleanup(self.temp.cleanup)

    def test_disjoint_failure_and_request_denominators(self):
        trace = Trace()
        trace.demand(100, 1, "0x1000")  # no prediction
        trace.candidate(10, 10, "0x2000")
        trace.sent(10, "0x2000")
        trace.fill(10, "0x2000")
        trace.demand(100, 2, "0x2000", "hit")
        trace.add(100, "PF_FIRST_USE", req=2, pf=10, pa="0x2000", result="hit")
        trace.candidate(10, 20, "0x3000")
        trace.sent(20, "0x3000")
        trace.add(100, "DEMAND_MERGE", req=3, related_id=20, pa="0x3000")
        trace.demand(100, 3, "0x3000")
        trace.fill(20, "0x3000", 140)
        trace.add(140, "PF_FIRST_USE", req=3, pf=20, pa="0x3000", result="late")
        trace.candidate(10, 30, "0x4000")
        trace.add(20, "PF_DROP", pf=30, pa="0x4000", reason="queue_full")
        trace.demand(100, 4, "0x4000")
        trace.candidate(10, 40, "0x5000")
        trace.sent(40, "0x5000")
        trace.fill(40, "0x5000")
        trace.add(60, "PF_EVICT", pf=40, pa="0x5000", result="unused", reason="replacement")
        trace.demand(100, 5, "0x5000")
        trace.candidate(10, 50, "0x6000")
        trace.sent(50, "0x6000")
        trace.fill(50, "0x6000")  # unobserved future use: right censored
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(
            summary["demand"]["failures"],
            {"no_prediction": 1, "late": 1, "dropped": 1, "evicted_early": 1},
        )
        self.assertEqual(summary["demand"]["misses"], 4)
        pf = summary["prefetch"]
        self.assertEqual(
            (
                pf["sent_requests"],
                pf["useful_requests"],
                pf["unused_evicted_requests"],
                pf["right_censored_sent_requests"],
            ),
            (4, 2, 1, 1),
        )
        self.assertEqual(pf["accuracy_observed"], 0.5)
        self.assertEqual(pf["timeliness"], 0.5)
        self.assertEqual(pcs[0]["pc"], "0x8000000012345678")

    def test_aliases_and_context_and_future_candidates(self):
        trace = Trace()
        trace.candidate(10, 10, "0x2000")
        trace.sent(10, "0x2000")
        trace.candidate(12, 11, "0x2000")
        trace.add(12, "PF_ALIAS", pf=11, pa="0x2000", related_id=10)
        trace.add(12, "PF_DROP", pf=11, pa="0x2000", reason="queue_duplicate")
        trace.demand(100, 1, "0x2000")
        trace.fill(10, "0x2000", 140)
        trace.add(140, "PF_FIRST_USE", req=1, pf=10, pa="0x2000", result="late")
        trace.candidate(110, 20, "0x3000")
        trace.demand(100, 2, "0x3000")  # future candidate cannot cover this demand
        trace.candidate(10, 30, None, va="0x4000", context_id=2)
        trace.demand(100, 3, "0x5000", va="0x4000", context_id=1)
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(summary["demand"]["failures"], {"late": 1, "no_prediction": 2})
        self.assertEqual(summary["prefetch"]["sent_requests"], 1)
        self.assertEqual(
            db.execute("SELECT matched_pf FROM demands WHERE request_id=1").fetchone()[0], 10
        )

    def test_fill_ready_and_lower_cache_and_retries(self):
        trace = Trace()
        trace.candidate(10, 10, "0x2000")
        trace.sent(10, "0x2000")
        trace.fill(10, "0x2000", 98)  # arrives before demand; ready afterwards
        trace.demand(100, 1, "0x2000")
        trace.candidate(10, 20, "0x3000", cache="system.l2", actor="system.l2.prefetcher", level=2)
        trace.sent(20, "0x3000")
        trace.add(40, "PF_FILL", pf=20, pa="0x3000", cache="system.l2", level=2, ready_tick=45)
        trace.demand(100, 2, "0x3000")
        trace.add(80, "DEMAND_ACCESS", req=3, pa="0x4000", result="miss_retry")
        trace.demand(100, 3, "0x4000")
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(
            summary["demand"]["failures"], {"late": 1, "lower_level_only": 1, "no_prediction": 1}
        )
        self.assertEqual(summary["demand"]["requests"], 3)
        self.assertEqual(summary["demand"]["lookup_attempts"], 4)
        self.assertEqual(summary["demand"]["replayed_lookups"], 1)
        self.assertEqual(
            db.execute(
                "SELECT complete_tick-first_tick FROM demands WHERE request_id=3"
            ).fetchone()[0],
            120,
        )

    def test_stale_dropped_candidate_does_not_explain_next_episode(self):
        trace = Trace()
        trace.candidate(10, 10, "0x2000")
        trace.add(20, "PF_DROP", pf=10, pa="0x2000", reason="queue_full")
        trace.demand(100, 1, "0x2000")
        trace.demand(300, 2, "0x2000")
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(summary["demand"]["failures"], {"dropped": 1, "no_prediction": 1})

    def test_counter_audit_rejects_an_entirely_missing_event_family(self):
        from analyze import reconcile_counters

        trace = Trace()
        trace.demand(100, 1, "0x1000")
        db, summary, pcs = self.analyze(trace)
        summary.update(
            trace_matches_stats_window=True,
            config={
                "entries": {
                    "system.cpu.dcache.cache_level": "1",
                    "system.cpu.dcache.is_read_only": "false",
                }
            },
            stats={"native_counters": {"system.cpu.dcache.prefetcher.pfDequeued": 3}},
        )
        audit = reconcile_counters(db, summary)
        self.assertEqual(audit["status"], "mismatch")
        self.assertEqual(audit["checks"][0]["trace"], 0)

    def test_direct_dtlb_counters_require_scope_metadata(self):
        from analyze import reconcile_counters

        trace = Trace()
        owner = "system.cpu.mmu.dtb"
        trace.add(10, "PF_TLB_LOOKUP", pf=10, actor=owner, result="hit")
        trace.add(11, "PF_TLB_LOOKUP", pf=20, actor=owner, result="miss")
        trace.add(12, "DEMAND_TLB_MISS", actor=owner, result="miss")
        db, summary, _ = self.analyze(trace)
        summary.update(
            trace_matches_stats_window=True,
            trace={"translation_trace_scope": "direct_l1_data"},
            config={"entries": {owner + ".is_dtlb": "true"}},
            stats={"native_counters": {
                owner + ".l1InitialLookupMisses": 1,
                owner + ".readprefetchHits": 1,
                owner + ".readprefetchMisses": 1,
            }},
        )
        audit = reconcile_counters(db, summary)
        self.assertEqual(audit["status"], "equal")
        self.assertEqual(len(audit["checks"]), 3)
        summary["stats"]["native_counters"][owner + ".readprefetchMisses"] = 2
        self.assertEqual(reconcile_counters(db, summary)["status"], "mismatch")
        summary["trace"] = {}
        self.assertEqual(reconcile_counters(db, summary)["checks"], [])

    def test_same_hardware_context_different_address_spaces_and_pa_mismatch(self):
        trace = Trace()
        trace.candidate(10, 10, None, va="0x4000", address_space="se:1000:0:0:0:0")
        trace.demand(100, 1, "0x5000", va="0x4000", address_space="se:2000:0:0:0:0")
        trace.candidate(10, 20, "0x6000", va="0x8000")
        trace.demand(100, 2, "0x7000", va="0x8000")
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(summary["demand"]["failures"], {"no_prediction": 2})
        self.assertEqual(len(pcs), 2)

    def test_coverage_requires_an_off_baseline_not_an_on_control(self):
        import copy
        from analyze import compare

        trace = Trace()
        trace.demand(100, 1, "0x1000")
        trace.demand(200, 2, "0x2000", "hit")
        db, on, pcs = self.analyze(trace)
        on.update(
            stats={"ipc": 2.0, "retired_instructions": 100},
            config={"entries": {}},
            trace_matches_stats_window=True,
        )
        off = copy.deepcopy(on)
        off["stats"]["ipc"] = 1.0
        off["demand"]["misses"] = 2
        self.assertEqual(compare(off, on, prefetch_off=True)["net_miss_coverage"], 0.5)
        self.assertIsNone(compare(off, on)["net_miss_coverage"])
        self.assertEqual(compare(off, on)["ipc_speedup"], 1.0)

    def test_port_backpressure_does_not_make_a_late_prefetch_timely(self):
        trace = Trace()
        trace.candidate(10, 10, "0x2000")
        trace.sent(10, "0x2000")
        trace.add(30, "PF_FILL", pf=10, pa="0x2000", cache="system.l2", level=2, ready_tick=35)
        trace.fill(10, "0x2000", 70)  # L1 ready at 75, after the L2 fill
        trace.add(50, "DEMAND_ARRIVE", req=1, pa="0x2000", arrival_tick=50)
        trace.demand(100, 1, "0x2000", "hit", arrival_tick=50)
        trace.add(
            100,
            "PF_FIRST_USE",
            req=1,
            pf=10,
            pa="0x2000",
            result="hit",
            ready_tick=75,
            arrival_tick=50,
        )
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(summary["prefetch"]["timeliness"], 0.0)
        row = db.execute("SELECT classification,complete_tick-first_tick FROM demands").fetchone()
        self.assertEqual(tuple(row), ("late_prefetch_hit", 150))
        self.assertEqual(summary["demand"]["misses"], 0)

    def test_prediction_created_after_port_arrival_cannot_cover_original_miss(self):
        trace = Trace()
        trace.add(10, "DEMAND_ARRIVE", req=1, pa="0x2000", arrival_tick=10)
        trace.candidate(20, 10, "0x2000")
        trace.sent(10, "0x2000", tick=30)
        trace.add(100, "DEMAND_MERGE", req=1, related_id=10, pa="0x2000", arrival_tick=10)
        trace.demand(100, 1, "0x2000", arrival_tick=10)
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(summary["demand"]["failures"], {"no_prediction": 1})
        self.assertEqual(summary["demand"]["failure_stages"], {"prediction_after_arrival": 1})

    def test_timeline_follows_prefetch_generated_by_a_different_pc(self):
        from analyze import export_timeline

        trace = Trace()
        trace.candidate(10, 10, "0x2000", pc="0xaaa")
        trace.sent(10, "0x2000")
        trace.fill(10, "0x2000")
        trace.demand(100, 1, "0x2000", "hit", pc="0xbbb")
        trace.add(
            100, "PF_FIRST_USE", req=1, pf=10, pa="0x2000", pc="0xbbb", result="hit", ready_tick=45
        )
        db, summary, pcs = self.analyze(trace)
        timeline = export_timeline(db, Path(self.temp.name), pc="0xbbb")
        events = {e["event"] for e in timeline["events"]}
        self.assertTrue(
            {"PF_CANDIDATE", "PF_SEND_ACCEPT", "PF_FILL", "PF_FIRST_USE", "DEMAND_COMPLETE"}
            <= events
        )

    def test_split_store_training_does_not_credit_another_line(self):
        trace = Trace()
        trace.demand(100, 1, "0x2000", inst_seq=99)
        trace.demand(100, 2, "0x2040", inst_seq=99)
        trace.add(90, "PF_TRAIN", req=100, pa="0x2000", inst_seq=99)
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(
            [r[0] for r in db.execute("SELECT trained FROM demands ORDER BY request_id")], [1, 0]
        )

    def test_source_attribution_aliases_and_observed_phase_samples(self):
        from stages import source_analysis

        trace = Trace()
        # One request generated at L2, then accepted and used at L1.
        trace.candidate(10, 10, "0x2000", cache="system.l2", source=12)
        trace.sent(10, "0x2000", tick=30)
        trace.fill(10, "0x2000", tick=50)
        trace.add(100, "PF_FIRST_USE", pf=10, pa="0x2000", ready_tick=55)
        # Another source aliases it: retain its drop, but never credit a send.
        trace.candidate(12, 11, "0x2000", source=3)
        trace.add(12, "PF_ALIAS", pf=11, pa="0x2000", related_id=10)
        trace.add(12, "PF_DROP", pf=11, pa="0x2000", reason="queue_duplicate")
        trace.candidate(20, 20, "0x3000", cache="system.l2", source=12)
        trace.sent(20, "0x3000", tick=50)  # no observed fill/use
        trace.candidate(30, 30, "0x4000", cache="system.l2", source=12)
        trace.add(35, "PF_DROP", pf=30, pa="0x4000", reason="prediction_buffer_full")
        db, summary, pcs = self.analyze(trace)
        analysis = source_analysis(db)
        sources = {(r["cache"], r["source"]): r for r in analysis["sources"]}
        l2 = sources[("system.l2", 12)]
        self.assertEqual(
            (l2["candidates"], l2["sent"], l2["useful"], l2["censored"]), (3, 2, 1, 1)
        )
        alias = sources[("system.cpu.dcache", 3)]
        self.assertEqual((alias["candidates"], alias["aliases"], alias["sent"]), (1, 1, 0))
        self.assertIsNone(alias["accuracy_observed"])
        drops = {
            (r["cache"], r["source"], r["reason"]): r["events"] for r in analysis["drop_events"]
        }
        self.assertEqual(drops[("system.cpu.dcache", 3, "queue_duplicate")], 1)
        self.assertEqual(drops[("system.l2", 12, "prediction_buffer_full")], 1)
        phases = {r["stage"]: r for r in analysis["phases"]}
        prediction = phases["prediction_to_enqueue"]
        self.assertEqual(
            (
                prediction["samples"],
                prediction["mean_ticks"],
                prediction["p50_ticks"],
                prediction["p90_ticks"],
            ),
            (2, 23, 18, 28),
        )
        ready = phases["accept_to_first_ready"]
        self.assertEqual(
            (ready["cache"], ready["source"], ready["samples"], ready["p99_ticks"]),
            ("system.l2", 12, 1, 25),
        )

    def test_multilevel_request_outcomes_partition_sent_requests(self):
        trace = Trace()
        # Evicting L3 does not make a still-resident L1 copy unused.
        for pf, pa in [(10, "0x2000"), (20, "0x3000"), (30, "0x4000")]:
            trace.candidate(10, pf, pa)
            trace.sent(pf, pa)
            trace.add(30, "PF_FILL", pf=pf, pa=pa, cache="system.l3", level=3, ready_tick=35)
            trace.add(
                31,
                "PF_EVICT",
                pf=pf,
                pa=pa,
                cache="system.l3",
                level=3,
                result="unused",
                reason="invalidation",
            )
            trace.fill(pf, pa)
        trace.add(100, "PF_FIRST_USE", pf=10, pa="0x2000", ready_tick=45)
        trace.add(60, "PF_EVICT", pf=30, pa="0x4000", result="unused", reason="replacement")
        # This eviction belongs to a request sent before the observation window.
        trace.add(60, "PF_EVICT", pf=40, pa="0x5000", result="unused", reason="replacement")
        # A pending accepted hop cannot be closed by eviction of another copy.
        trace.candidate(10, 50, "0x6000")
        trace.sent(50, "0x6000")
        trace.add(30, "PF_FILL", pf=50, pa="0x6000", cache="system.l3", level=3, ready_tick=35)
        trace.add(
            31,
            "PF_EVICT",
            pf=50,
            pa="0x6000",
            cache="system.l3",
            level=3,
            result="unused",
            reason="invalidation",
        )
        db, summary, pcs = self.analyze(trace)
        pf = summary["prefetch"]
        self.assertEqual(
            (
                pf["sent_requests"],
                pf["useful_requests"],
                pf["unused_evicted_requests"],
                pf["right_censored_sent_requests"],
            ),
            (4, 1, 1, 2),
        )

    def test_other_cache_eviction_does_not_invalidate_a_live_copy(self):
        trace = Trace()
        trace.candidate(10, 10, "0x2000", cache="system.l2", level=2)
        trace.sent(10, "0x2000")
        trace.add(30, "PF_FILL", pf=10, pa="0x2000", cache="system.l3", level=3, ready_tick=35)
        trace.add(
            31,
            "PF_EVICT",
            pf=10,
            pa="0x2000",
            cache="system.l3",
            level=3,
            result="unused",
            reason="invalidation",
        )
        trace.add(40, "PF_FILL", pf=10, pa="0x2000", cache="system.l2", level=2, ready_tick=45)
        trace.demand(100, 1, "0x2000")
        trace.candidate(10, 20, "0x3000")
        trace.sent(20, "0x3000")
        trace.add(30, "PF_FILL", pf=20, pa="0x3000", cache="system.l3", level=3, ready_tick=35)
        trace.fill(20, "0x3000", tick=98)  # L1 ready only at 103
        trace.demand(100, 2, "0x3000")
        db, summary, pcs = self.analyze(trace)
        self.assertEqual(summary["demand"]["failures"], {"lower_level_only": 1, "late": 1})

    def test_open_trace_is_rejected(self):
        path = Path(self.temp.name) / "open.csv"
        Trace().write(path, closed=False)
        with self.assertRaisesRegex(ValueError, "still open or truncated"):
            load_trace(path, Path(self.temp.name) / "open.sqlite")

    def test_queue_intervals_match_actor_and_exclude_translation_wait(self):
        from queue_analysis import queue_analysis

        trace = Trace()
        trace.candidate(10, 10, "0x1000", source=4)
        state = '{"priority":32,"prediction_buffer_entries":1}'
        trace.add(
            10,
            "PF_BUFFER_ENQUEUE",
            pf=10,
            source=4,
            result=state,
            actor="generator",
        )
        trace.add(
            15,
            "PF_BUFFER_DEQUEUE",
            pf=10,
            source=4,
            result=state,
            actor="generator",
        )
        state = '{"priority":32,"ready_queue_entries":2,"translation_queue_entries":1}'
        trace.add(
            15,
            "PF_ENQUEUE",
            pf=10,
            source=4,
            result=state,
            reason="translation_queue",
        )
        trace.add(
            25,
            "PF_ENQUEUE",
            pf=10,
            source=4,
            result=state,
            reason="ready_queue",
        )
        trace.add(40, "PF_DEQUEUE", pf=10, source=4, result=state)
        # An observed dequeue with an absent enqueue remains incomplete.
        trace.add(
            42, "PF_DEQUEUE", pf=10, source=4, result=state, actor="forwarder"
        )
        trace.add(45, "PF_SEND_ACCEPT", pf=10, source=4)
        trace.add(50, "PF_FIRST_USE", req=2, pf=10, source=4, result="hit")
        db, _, _ = self.analyze(trace)
        result = queue_analysis(db)
        self.assertEqual(len(result["intervals"]), 2)
        elapsed = {
            row["stage"]: row["elapsed_ticks"] for row in result["intervals"]
        }
        self.assertEqual(elapsed, {"prediction_buffer": 5, "ready_queue": 15})
        self.assertTrue(all(row["priority"] == 32 for row in result["stages"]))
        self.assertEqual(result["samples"][-1]["actor"], "forwarder")
        self.assertEqual(len(result["outcomes"]), 2)
        self.assertTrue(all(row["useful_requests"] == 1
                            and row["accuracy_observed"] == 1.0
                            for row in result["outcomes"]))

    def test_prefetch_id_timeline_follows_aliases_and_demand_consumers(self):
        from analyze import export_timeline

        trace = Trace()
        trace.candidate(10, 10, "0x1000")
        trace.candidate(11, 20, "0x1000")
        trace.add(12, "PF_ALIAS", pf=20, related_id=10)
        trace.candidate(13, 30, "0x2000")
        trace.add(14, "PF_TLB_LOOKUP", req=100, pf=10, pa=None,
                  actor="system.cpu.mmu.dtb", result="miss")
        trace.add(15, "PF_TRANSLATE", req=100, pf=10)
        trace.add(90, "DEMAND_TLB_MISS", req=7, pa=None,
                  actor="system.cpu.mmu.dtb", result="miss")
        trace.add(
            30,
            "CACHE_ARB",
            req=99,
            related_id=10,
            pa="0x3000",
            reason="ready_demand_before_prefetch",
        )
        trace.demand(33, 99, "0x3000")
        trace.sent(10, "0x1000")
        trace.fill(10, "0x1000")
        trace.demand(100, 7, "0x1000", "hit", pc="0x777")
        trace.add(100, "PF_FIRST_USE", req=7, pf=10, pa="0x1000", result="hit")
        db, _, _ = self.analyze(trace)
        timeline = export_timeline(db, Path(self.temp.name), prefetch_id=10)
        ids = {row["prefetch_id"] for row in timeline["events"]}
        self.assertEqual(ids, {0, 10, 20})
        self.assertTrue(
            any(
                row["event"] == "DEMAND_COMPLETE" and row["request_id"] == 7
                for row in timeline["events"]
            )
        )
        self.assertTrue(
            any(
                row["event"] == "CACHE_ARB" and row["request_id"] == 99
                for row in timeline["events"]
            )
        )
        self.assertTrue(
            any(
                row["event"] == "DEMAND_COMPLETE" and row["request_id"] == 99
                for row in timeline["events"]
            )
        )
        self.assertEqual(
            {row["event"] for row in timeline["events"]
             if row["actor"] == "system.cpu.mmu.dtb"},
            {"PF_TLB_LOOKUP", "DEMAND_TLB_MISS"},
        )
        self.assertEqual(timeline["scope"]["prefetch_id"], 10)


if __name__ == "__main__":
    unittest.main()

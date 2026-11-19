#!/usr/bin/env python3
"""Hand-integrated residency intervals and ROI boundary regressions."""

import json
import sqlite3
import unittest

from analyze import reconcile_counters
from occupancy import cache_occupancy


class OccupancyTests(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute(
            'CREATE TABLE events(event TEXT,cache TEXT,tick INTEGER,"order" INTEGER,'
            "result TEXT,level INTEGER)"
        )
        self.addCleanup(self.db.close)

    def state(self, tick, order, valid, prefetched, unused):
        self.db.execute(
            "INSERT INTO events VALUES ('CACHE_OCCUPANCY','l1',?,?,?,1)",
            (
                tick,
                order,
                json.dumps(
                    {
                        "valid_lines": valid,
                        "prefetched_lines": prefetched,
                        "unused_prefetched_lines": unused,
                        "capacity_lines": 16,
                    }
                ),
            ),
        )

    def test_time_integral_survives_same_tick_changes_and_final_residency(
        self,
    ):
        self.state(10, 1, 8, 4, 3)
        self.state(15, 2, 8, 4, 2)
        self.state(15, 3, 8, 4, 1)
        self.state(16, 4, 8, 3, 1)
        result = cache_occupancy(
            self.db, {"cache_occupancy_version": "1"}, 10, 20, bins=3
        )
        row = result["caches"][0]
        self.assertEqual(row["valid_lines_ticks"], 80)
        self.assertEqual(row["prefetched_lines_ticks"], 36)
        self.assertEqual(row["unused_prefetched_lines_ticks"], 20)
        self.assertEqual(row["prefetched_lines_capacity_fraction"], 0.225)
        for key in (
            "valid_lines",
            "prefetched_lines",
            "unused_prefetched_lines",
        ):
            self.assertAlmostEqual(
                sum(
                    interval[key]
                    * (interval["end_tick"] - interval["begin_tick"])
                    for interval in result["timeline"]
                ),
                row[key + "_ticks"],
            )

    def test_exclusive_end_state_is_outside_measurement(self):
        self.state(10, 1, 8, 4, 3)
        self.state(20, 2, 16, 8, 6)
        result = cache_occupancy(
            self.db, {"cache_occupancy_version": "1"}, 10, 20
        )
        row = result["caches"][0]
        self.assertEqual(row["valid_lines_ticks"], 80)
        self.assertEqual(row["peak_valid_lines"], 8)

    def test_missing_initial_snapshot_is_rejected(self):
        self.state(11, 1, 8, 4, 3)
        with self.assertRaisesRegex(ValueError, "missing ROI snapshot"):
            cache_occupancy(self.db, {"cache_occupancy_version": "1"}, 10, 20)

    def test_inconsistent_state_is_rejected(self):
        self.state(10, 1, 8, 4, 5)
        with self.assertRaisesRegex(ValueError, "invalid occupancy state"):
            cache_occupancy(self.db, {"cache_occupancy_version": "1"}, 10, 20)

    def test_native_average_uses_final_tick_and_print_precision(self):
        self.state(10, 1, 8, 4, 3)
        self.state(15, 2, 7, 3, 2)
        occupancy = cache_occupancy(
            self.db, {"cache_occupancy_version": "1"}, 10, 20
        )
        summary = {
            "trace_matches_stats_window": True,
            "config": {"entries": {}},
            "cache_occupancy": occupancy,
            "observation_ticks": [10, 20],
            "stats": {"native_counters": {"l1.tags.tagsInUse": 7.454545}},
        }
        self.assertEqual(
            reconcile_counters(self.db, summary)["status"], "equal"
        )
        summary["stats"]["native_counters"]["l1.tags.tagsInUse"] = 7.464545
        self.assertEqual(
            reconcile_counters(self.db, summary)["status"], "mismatch"
        )

    def test_older_trace_reports_absent_measurement(self):
        self.assertEqual(
            cache_occupancy(self.db, {}, 10, 20)["status"], "not_recorded"
        )


if __name__ == "__main__":
    unittest.main()

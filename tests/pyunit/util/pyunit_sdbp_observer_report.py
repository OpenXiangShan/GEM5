# Copyright (c) 2026 Institute of Computing Technology, CAS
# SPDX-License-Identifier: BSD-3-Clause

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().parents[3] / "util/sdbp_observer_report.py"
SPEC = importlib.util.spec_from_file_location("sdbp_report", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SDBPObserverReportTest(unittest.TestCase):
    def matrix(self, source, tp, fp, tn, fn):
        counts = {
            "Predictions": tp + fp + tn + fn,
            "Resolved": tp + fp + tn + fn,
            "DeadTruePositive": tp,
            "DeadFalsePositive": fp,
            "LiveTrueNegative": tn,
            "LiveFalseNegative": fn,
        }
        return {
            f"system.l2.replacement_policy.fullCache{name}BySource::{source}": value
            for name, value in counts.items()
        }

    def test_coarse_layout_unchanged(self):
        stats = self.matrix("l1Prefetch", 4, 1, 3, 2)
        report = MODULE.report(stats)
        self.assertEqual(report["source_layout"], "level")
        self.assertEqual(report["aggregate"]["prefetch"]["accuracy"], 0.7)
        self.assertEqual(report["aggregate"]["l2Prefetch"]["resolved"], 0)

    def test_detailed_groups_sum_counts_before_ratios(self):
        stats = {
            **self.matrix("l1Prefetch_Berti", 4, 1, 3, 2),
            **self.matrix("l1Prefetch_LLDP", 0, 0, 1, 0),
            **self.matrix("l2Prefetch_LLDP", 1, 2, 0, 0),
            **self.matrix("l2Prefetch_PF_NONE", 0, 0, 0, 1),
            **self.matrix("l1Demand", 1, 0, 1, 0),
        }
        report = MODULE.report(stats)
        self.assertEqual(report["source_layout"], "level_and_algorithm")
        groups = report["aggregate"]
        self.assertEqual(groups["l1Prefetch"]["accuracy"], 8 / 11)
        self.assertEqual(groups["l2Prefetch"]["accuracy"], 1 / 4)
        self.assertEqual(groups["prefetch"]["accuracy"], 9 / 15)
        self.assertEqual(groups["all"]["resolved"], 17)
        self.assertEqual(groups["prefetch_LLDP"]["resolved"], 4)
        self.assertEqual(groups["prefetch_LLDP"]["accuracy"], 2 / 4)
        self.assertEqual(groups["prefetch"]["dead_precision"], 5 / 8)
        self.assertEqual(groups["prefetch"]["dead_recall"], 5 / 8)

    def test_source_lifecycle_loss_fails(self):
        stats = self.matrix("l2Prefetch_LLDP", 1, 0, 0, 0)
        key = next(key for key in stats if "Predictions" in key)
        stats[key] = 2
        with self.assertRaises(ValueError):
            MODULE.report(stats)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Regression tests for the A/B plus raw-value decision join."""

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("ideal_constant_lvp_value_decision.py")


class IdealConstantLVPValueDecisionTest(unittest.TestCase):
    def write_csv(self, path, fields, rows):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="") as output:
            writer = csv.DictWriter(output, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def write_ab(self, root):
        ab_dir = root / "ab"
        fields = (
            "slice",
            "benchmark",
            "cycle_speedup_pct",
            "enabled_cycles",
            "disabled_cycles",
        )
        self.write_csv(
            ab_dir / "per_slice_comparison.csv",
            fields,
            [
                {
                    "slice": "mcf_1",
                    "benchmark": "mcf",
                    "cycle_speedup_pct": "4.0",
                    "enabled_cycles": "96",
                    "disabled_cycles": "100",
                },
                {
                    "slice": "gcc_2",
                    "benchmark": "gcc",
                    "cycle_speedup_pct": "-1.0",
                    "enabled_cycles": "101",
                    "disabled_cycles": "100",
                },
            ],
        )
        self.write_csv(
            ab_dir / "sensitive_slices.csv",
            ("slice", "sensitivity_class"),
            [{"slice": "mcf_1", "sensitivity_class": "gain"}],
        )
        return ab_dir

    def value_slice_rows(self):
        base = {
            "scope": "roi",
            "profile_version": "ideal_constant_lvp_saturated_values_v1",
            "pc_entries": "2",
            "cumulative_distinct_values": "3",
            "global_distinct_saturated_values": "2",
            "value_sharing_saved_slots": "1",
            "prediction_use_columns_present": "1",
            "prediction_uses": "9",
            "correct_prediction_uses": "8",
            "stats_vp_supported": "20",
            "stats_vp_predicted": "9",
            "stats_vp_corrected": "8",
            "coverage_contribution_pct": "40.0",
            "concurrent_distinct_value_peak": "2",
            "concurrent_distinct_value_peak_source": "stats",
            "interval_concurrent_distinct_value_peak": "2",
            "interval_concurrent_saturated_pc_peak": "2",
        }
        return [
            {**base, "slice": "mcf_1", "benchmark": "mcf"},
            {
                **base,
                "slice": "gcc_2",
                "benchmark": "gcc",
                "cumulative_distinct_values": "2",
                "global_distinct_saturated_values": "2",
                "value_sharing_saved_slots": "0",
                "concurrent_distinct_value_peak": "1",
                "interval_concurrent_distinct_value_peak": "1",
                "prediction_uses": "6",
                "correct_prediction_uses": "6",
                "stats_vp_predicted": "6",
                "stats_vp_corrected": "6",
                "coverage_contribution_pct": "30.0",
            },
        ]

    def write_values(self, root, mismatch=False):
        value_dir = root / "values"
        slice_rows = self.value_slice_rows()
        self.write_csv(
            value_dir / "per_slice_values.csv",
            tuple(slice_rows[0]),
            slice_rows,
        )
        pc_rows = [
            {
                "slice": "mcf_1",
                "benchmark": "mcf",
                "scope": "roi",
                "tid": "0",
                "pc": "0x10",
                "distinct_saturated_values": "2",
                "saturated_value_segments": "2",
                "prediction_uses": "5",
                "correct_prediction_uses": "4",
                "wrong_prediction_uses": "1",
                "coverage_contribution_pct": "20.0",
            },
            {
                "slice": "mcf_1",
                "benchmark": "mcf",
                "scope": "roi",
                "tid": "0",
                "pc": "0x20",
                "distinct_saturated_values": "1",
                "saturated_value_segments": "1",
                "prediction_uses": "4",
                "correct_prediction_uses": "4",
                "wrong_prediction_uses": "0",
                "coverage_contribution_pct": "20.0",
            },
            {
                "slice": "gcc_2",
                "benchmark": "gcc",
                "scope": "roi",
                "tid": "0",
                "pc": "0x30",
                "distinct_saturated_values": "1",
                "saturated_value_segments": "1",
                "prediction_uses": "3",
                "correct_prediction_uses": "3",
                "wrong_prediction_uses": "0",
                "coverage_contribution_pct": "15.0",
            },
            {
                "slice": "gcc_2",
                "benchmark": "gcc",
                "scope": "roi",
                "tid": "0",
                "pc": "0x40",
                "distinct_saturated_values": "1",
                "saturated_value_segments": "1",
                "prediction_uses": "3",
                "correct_prediction_uses": "3",
                "wrong_prediction_uses": "0",
                "coverage_contribution_pct": "15.0",
            },
        ]
        if mismatch:
            pc_rows[-1]["distinct_saturated_values"] = "2"
        self.write_csv(value_dir / "per_pc_values.csv", tuple(pc_rows[0]), pc_rows)
        self.write_csv(
            value_dir / "per_value_sharing.csv",
            ("sharing_fanout",),
            [{"sharing_fanout": "1"}, {"sharing_fanout": "2"}],
        )
        return value_dir

    def run_tool(self, ab_dir, value_dir, output, check=True, charts=False):
        command = [
            sys.executable,
            str(SCRIPT),
            "--ab-dir",
            str(ab_dir),
            "--value-dir",
            str(value_dir),
            "--out-dir",
            str(output),
            "--expected-slices",
            "2",
        ]
        if not charts:
            command.append("--skip-charts")
        return subprocess.run(command, check=check, text=True, capture_output=True)

    def test_joins_only_ab_speedup_with_v3_value_fields(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            output = root / "output"
            completed = self.run_tool(self.write_ab(root), self.write_values(root), output)
            summary = json.loads(completed.stdout)
            self.assertEqual(summary["joined_slices"], 2)
            self.assertEqual(summary["sensitive_slices"], 1)
            self.assertEqual(summary["profile_version"], "ideal_constant_lvp_saturated_values_v1")
            self.assertTrue((output / "value_decision_report_section.md").is_file())

            with (output / "per_slice_value_decision.csv").open(newline="") as report:
                rows = {row["slice"]: row for row in csv.DictReader(report)}
            self.assertEqual(rows["mcf_1"]["cycle_speedup_pct"], "4.0")
            self.assertEqual(rows["mcf_1"]["sensitivity_class"], "gain")
            self.assertEqual(rows["mcf_1"]["online_peak_distinct_values"], "2")
            self.assertEqual(rows["mcf_1"]["pc_distinct_values_max"], "2.0")
            self.assertAlmostEqual(
                float(rows["mcf_1"]["value_sharing_ratio_pct"]), 100 / 3
            )
            self.assertEqual(rows["gcc_2"]["sensitivity_class"], "insensitive")

            with (output / "per_pc_value_decision.csv").open(newline="") as report:
                pc_rows = list(csv.DictReader(report))
            self.assertEqual(len(pc_rows), 4)
            self.assertEqual(pc_rows[0]["sensitivity_class"], "gain")

    def test_rejects_per_pc_cumulative_value_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            completed = self.run_tool(
                self.write_ab(root),
                self.write_values(root, mismatch=True),
                root / "output",
                check=False,
            )
            self.assertEqual(completed.returncode, 2)
            self.assertIn("per-PC values do not sum", completed.stderr)


if __name__ == "__main__":
    unittest.main()

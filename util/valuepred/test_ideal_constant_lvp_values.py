#!/usr/bin/env python3
"""Regression tests for IdealConstantLVP saturated-value aggregation."""

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("ideal_constant_lvp_values.py")
FULL_HEADER = (
    "scope,tid,pc,saturation_epoch,value_segment,saturated_value,"
    "saturation_start_seq_no,saturation_end_seq_no,prediction_uses,"
    "correct_prediction_uses,open_at_scope_start,open_at_end\n"
)
BASE_HEADER = (
    "scope,tid,pc,saturation_epoch,value_segment,saturated_value\n"
)


def value_row(
    scope,
    pc,
    epoch,
    value_segment,
    value,
    start=0,
    end=0,
    prediction_uses=1,
    correct_prediction_uses=1,
    open_at_scope_start=0,
    open_at_end=0,
):
    return (
        f"{scope},0,{pc},{epoch},{value_segment},{value},{start},{end},"
        f"{prediction_uses},{correct_prediction_uses},{open_at_scope_start},"
        f"{open_at_end}\n"
    )


class IdealConstantLVPValuesTest(unittest.TestCase):
    def write_values(self, archive, slice_name, rows, header=FULL_HEADER):
        m5out = archive / "spec_all" / slice_name / "m5out"
        m5out.mkdir(parents=True)
        value_csv = m5out / "ideal_constant_lvp_saturated_values.csv"
        with value_csv.open("w") as output:
            output.write("# ideal_constant_lvp_saturated_values_v1\n")
            output.write("# value_definition=raw_regval_bit_pattern\n")
            output.write(header)
            output.writelines(rows)
        return m5out

    def write_stats(
        self,
        m5out,
        roi_peak=None,
        lifetime_peak=None,
        vp_supported=None,
        vp_predicted=None,
        vp_corrected=None,
    ):
        with (m5out / "stats.txt").open("w") as output:
            output.write("---------- Begin Simulation Statistics ----------\n")
            if roi_peak is not None:
                output.write(
                    "system.cpu.valuePred.predictors."
                    "profileRoiPeakDistinctSaturatedValues "
                    f"{roi_peak}\n"
                )
            if lifetime_peak is not None:
                output.write(
                    "system.cpu.valuePred.predictors."
                    "profileLifetimePeakDistinctSaturatedValues "
                    f"{lifetime_peak}\n"
                )
            for stat, value in (
                ("VPsupported", vp_supported),
                ("VPpredicted", vp_predicted),
                ("VPcorrected", vp_corrected),
            ):
                if value is not None:
                    output.write(f"system.cpu.valuePred.{stat} {value}\n")
            output.write("---------- End Simulation Statistics ----------\n")

    def run_tool(self, archive, output, scope="roi", check=True):
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                str(archive),
                "--scope",
                scope,
                "--out-dir",
                str(output),
            ],
            check=check,
            text=True,
            capture_output=True,
        )

    def test_online_stats_are_primary_and_interval_sweep_is_auxiliary(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            m5out = self.write_values(
                archive,
                "mcf_1",
                [
                    value_row("roi", "0x10", 1, 1, "0xaa", 10, 20),
                    value_row("roi", "0x20", 1, 1, "0xaa", 15, 25),
                    value_row("roi", "0x30", 1, 1, "0xbb", 20, 30),
                    value_row("roi", "0x10", 2, 1, "0xcc", 35, 40),
                    value_row("lifetime", "0x10", 1, 1, "0xaa", 1, 0,
                              open_at_end=1),
                ],
            )
            # Only the final complete stats block is authoritative.  It
            # deliberately differs from the offline interval sweep below.
            self.write_stats(
                m5out,
                roi_peak=2,
                vp_supported=10,
                vp_predicted=4,
                vp_corrected=4,
            )
            with (m5out / "stats.txt").open("a") as stats:
                stats.write("---------- Begin Simulation Statistics ----------\n")
                stats.write(
                    "system.cpu.valuePred.predictors."
                    "profileRoiPeakDistinctSaturatedValues 5\n"
                )
                stats.write("system.cpu.valuePred.VPsupported 10\n")
                stats.write("system.cpu.valuePred.VPpredicted 4\n")
                stats.write("system.cpu.valuePred.VPcorrected 4\n")
                stats.write("---------- End Simulation Statistics ----------\n")

            completed = self.run_tool(archive, output)
            self.assertEqual(json.loads(completed.stdout)["slices"], 1)
            self.assertTrue((output / "summary.json").is_file())
            self.assertTrue((output / "per_slice_values.csv").is_file())
            self.assertTrue((output / "per_pc_values.csv").is_file())
            self.assertTrue((output / "per_value_sharing.csv").is_file())

            with (output / "per_slice_values.csv").open(newline="") as report:
                slice_row = next(csv.DictReader(report))
            self.assertEqual(slice_row["cumulative_distinct_values"], "4")
            self.assertEqual(slice_row["global_distinct_saturated_values"], "3")
            self.assertEqual(slice_row["value_sharing_saved_slots"], "1")
            self.assertEqual(slice_row["prediction_uses"], "4")
            self.assertEqual(slice_row["correct_prediction_uses"], "4")
            self.assertEqual(slice_row["coverage_contribution_pct"], "40.0")
            self.assertEqual(slice_row["concurrent_distinct_value_peak"], "5")
            self.assertEqual(slice_row["concurrent_distinct_value_peak_source"], "stats")
            self.assertEqual(
                slice_row["interval_concurrent_distinct_value_peak"], "2"
            )
            self.assertEqual(
                slice_row["interval_concurrent_saturated_pc_peak"], "3"
            )
            self.assertEqual(
                slice_row["interval_peak_differs_from_online_stats"], "1"
            )

            with (output / "per_pc_values.csv").open(newline="") as report:
                pc_rows = {row["pc"]: row for row in csv.DictReader(report)}
            self.assertEqual(pc_rows["0x10"]["distinct_saturated_values"], "2")
            self.assertEqual(pc_rows["0x10"]["saturation_epochs"], "2")
            self.assertEqual(pc_rows["0x10"]["prediction_uses"], "2")
            self.assertEqual(pc_rows["0x10"]["coverage_contribution_pct"], "20.0")
            self.assertEqual(pc_rows["0x20"]["value_sharing_fanout_max"], "2")

            with (output / "per_value_sharing.csv").open(newline="") as report:
                value_rows = {
                    row["saturated_value"]: row for row in csv.DictReader(report)
                }
            self.assertEqual(value_rows["0xaa"]["sharing_fanout"], "2")
            self.assertEqual(value_rows["0xaa"]["prediction_uses"], "2")

            with (output / "summary.json").open() as report:
                summary = json.load(report)
            self.assertEqual(summary["peak_source_counts"], {"stats": 1})
            self.assertEqual(
                summary["slices_with_interval_peak_different_from_online_stats"],
                1,
            )
            self.assertEqual(
                summary["distributions"]["concurrent_distinct_value_peak"]["max"],
                5.0,
            )
            self.assertEqual(
                summary["distributions"][
                    "interval_concurrent_distinct_value_peak"
                ]["max"],
                2.0,
            )

    def test_rejects_value_profile_stats_use_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            m5out = self.write_values(
                archive,
                "gcc_uses_1",
                [value_row("roi", "0x10", 1, 1, "0xaa", 10, 20)],
            )
            self.write_stats(m5out, roi_peak=1, vp_predicted=2, vp_corrected=1)

            completed = self.run_tool(archive, output, check=False)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("prediction uses 1 != VPpredicted 2", completed.stderr)

    def test_stats_peak_remains_available_without_interval_columns(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            m5out = self.write_values(
                archive,
                "gcc_2",
                ["roi,0,0x10,1,1,0xaa\n"],
                header=BASE_HEADER,
            )
            self.write_stats(m5out, roi_peak=4)

            self.run_tool(archive, output)
            with (output / "per_slice_values.csv").open(newline="") as report:
                row = next(csv.DictReader(report))
            self.assertEqual(row["concurrent_distinct_value_peak"], "4")
            self.assertEqual(row["concurrent_distinct_value_peak_source"], "stats")
            self.assertEqual(row["interval_concurrent_distinct_value_peak"], "")
            self.assertEqual(row["interval_concurrent_saturated_pc_peak"], "")

    def test_interval_sweep_does_not_become_hardware_capacity_without_stats(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            self.write_values(
                archive,
                "bzip2_3",
                [value_row("roi", "0x10", 1, 1, "0xaa", 10, 20)],
            )

            self.run_tool(archive, output)
            with (output / "per_slice_values.csv").open(newline="") as report:
                row = next(csv.DictReader(report))
            self.assertEqual(row["concurrent_distinct_value_peak"], "")
            self.assertEqual(row["concurrent_distinct_value_peak_source"], "unavailable")
            self.assertEqual(row["interval_concurrent_distinct_value_peak"], "1")

    def test_rejects_duplicate_keys_and_malformed_intervals(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            duplicate = value_row("roi", "0x10", 1, 1, "0xaa", 10, 20)
            self.write_values(archive, "perlbench_4", [duplicate, duplicate])
            completed = self.run_tool(archive, output, check=False)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("duplicates", completed.stderr)

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            self.write_values(
                archive,
                "perlbench_5",
                [value_row("roi", "0x10", 1, 1, "0xaa", 10, 20,
                           open_at_scope_start=1)],
            )
            completed = self.run_tool(archive, output, check=False)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("inconsistent scope-start interval", completed.stderr)

        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            self.write_values(
                archive,
                "perlbench_6",
                [value_row("roi", "0x10", 1, 1, "-1", 10, 20)],
            )
            completed = self.run_tool(archive, output, check=False)
            self.assertEqual(completed.returncode, 2)
            self.assertIn("negative saturated_value", completed.stderr)


if __name__ == "__main__":
    unittest.main()

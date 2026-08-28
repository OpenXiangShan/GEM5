#!/usr/bin/env python3
"""Regression test for the IdealConstantLVP profile aggregation tool."""

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("ideal_constant_lvp_profile.py")
HEADER = (
    "scope,tid,pc,updates,first_update,last_update,value_changes,"
    "saturation_transitions,saturated_updates,first_saturation_update,"
    "ever_saturated,saturated_at_end,confidence,value\n"
)


def profile_row(scope, pc, ever_saturated, saturated_at_end):
    return (
        f"{scope},0,{pc},10,1,10,0,1,2,5,{ever_saturated},"
        f"{saturated_at_end},511,0x1\n"
    )


class IdealConstantLVPProfileTest(unittest.TestCase):
    def write_profile(self, archive, slice_name, rows):
        profile = archive / "spec_all" / slice_name / "m5out"
        profile.mkdir(parents=True)
        with (profile / "ideal_constant_lvp_profile.csv").open("w") as output:
            output.write("# ideal_constant_lvp_profile_v1\n")
            output.write("# sat_counter_bits=9\n")
            output.write(HEADER)
            output.writelines(rows)

    def test_merges_static_pcs_within_a_benchmark(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            archive = root / "archive"
            output = root / "output"
            self.write_profile(
                archive,
                "gcc_input_a_1",
                [
                    profile_row("lifetime", "0x10", 1, 1),
                    profile_row("lifetime", "0x20", 0, 0),
                ],
            )
            self.write_profile(
                archive,
                "gcc_input_b_2",
                [
                    profile_row("lifetime", "0x10", 1, 0),
                    profile_row("lifetime", "0x30", 1, 1),
                ],
            )

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(archive),
                    "--out-dir",
                    str(output),
                ],
                check=True,
                text=True,
                capture_output=True,
            )

            with (output / "summary.json").open() as summary_file:
                summary = json.load(summary_file)
            self.assertEqual(summary["slices"], 2)
            self.assertEqual(summary["slice_distinct_pcs"]["sum"], 4)
            self.assertEqual(summary["corpus_unique_pc_items"], 3)
            self.assertEqual(summary["corpus_pc_items_ever_saturated"], 2)

            with (output / "per_benchmark.csv").open(newline="") as report:
                rows = list(csv.DictReader(report))
            self.assertEqual(rows, [
                {
                    "benchmark": "gcc",
                    "slices": "2",
                    "sum_slice_distinct_pcs": "4",
                    "unique_pcs_across_slices": "3",
                    "pcs_ever_saturated": "2",
                    "pcs_saturated_at_end_in_any_slice": "2",
                    "max_slice_distinct_pcs": "2",
                    "p95_slice_distinct_pcs": "2",
                    "max_slice_ever_saturated_pcs": "2",
                    "p95_slice_ever_saturated_pcs": "2",
                }
            ])


if __name__ == "__main__":
    unittest.main()

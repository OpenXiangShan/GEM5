#!/usr/bin/env python3
"""Regression test for IdealConstantLVP static set-pressure analysis."""

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("ideal_constant_lvp_capacity.py")
HEADER = (
    "scope,tid,pc,updates,first_update,last_update,value_changes,"
    "saturation_transitions,saturated_updates,first_saturation_update,"
    "ever_saturated,saturated_at_end,confidence,value\n"
)


class IdealConstantLVPCapacityTest(unittest.TestCase):
    def test_reports_static_overflow_for_selected_entries(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            profile = root / "archive" / "spec_all" / "mcf_1" / "m5out"
            profile.mkdir(parents=True)
            with (profile / "ideal_constant_lvp_profile.csv").open("w") as output:
                output.write("# ideal_constant_lvp_profile_v1\n")
                output.write(HEADER)
                output.write("lifetime,0,0x0,1,1,1,0,0,0,0,1,1,511,0x1\n")
                output.write("lifetime,0,0x8,1,1,1,0,0,0,0,1,1,511,0x1\n")
                output.write("lifetime,0,0x10,1,1,1,0,0,0,0,0,0,0,0x1\n")

            out_dir = root / "output"
            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    str(root / "archive"),
                    "--table",
                    "pct:2:1:ever-saturated",
                    "--out-dir",
                    str(out_dir),
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            with (out_dir / "per_slice_set_pressure.csv").open(newline="") as report:
                rows = list(csv.DictReader(report))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["selected_pcs"], "2")
            self.assertEqual(rows[0]["max_set_occupancy"], "2")
            self.assertEqual(rows[0]["static_overflow_entries"], "1")


if __name__ == "__main__":
    unittest.main()

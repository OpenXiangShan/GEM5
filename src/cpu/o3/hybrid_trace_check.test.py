#!/usr/bin/env python3
"""Behavioral tests for Hybrid entry trace validation."""

import tempfile
import unittest
from pathlib import Path

from hybrid_trace_check import check_trace


class HybridTraceTest(unittest.TestCase):
    def check(self, lines, groups=1, insts=3, retired=1, downgraded=0,
              types=None, lengths=None, width=1):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "config.ini").write_text(
                "[system.cpu]\nRobCompressPolicy=hybrid\n"
                f"commitWidth={width}\nnumROBEntries=4\n"
                "CROB_instPerGroup=8\n")
            values = {
                "rob.hybridAllocatedGroups": groups,
                "rob.hybridAllocatedInsts": insts,
                "rob.hybridGroupLength::samples": groups,
                "rob.hybridDowngrades": downgraded,
                "commit.hybridCommittedEntries": retired,
                "commit.hybridDrainedEntries": 0,
            }
            for name in ("NORMAL-S", "NORMAL-C", "NORMAL-N", "CC", "CS", "SC"):
                values["rob.hybridGroupType::" + name] = (types or {"CS": 1}).get(name, 0)
            for length, count in (lengths or {3: 1}).items():
                values[f"rob.hybridGroupLength::{length}"] = count
            (root / "stats.txt").write_text("".join(
                f"system.cpu.{key} {value}\n" for key, value in values.items()))
            (root / "rob.log").write_text("\n".join(lines) + "\n")
            return check_trace(root / "rob.log", root / "config.ini", root / "stats.txt")

    def allocation(self):
        return [
            "0: system.cpu.rob: Hybrid allocate id=1 type=4 former=1 latter=2",
            "0: system.cpu.rob: Hybrid member id=1 sn=10 former=1",
            "0: system.cpu.rob: Hybrid member id=1 sn=20 former=0",
            "0: system.cpu.rob: Hybrid member id=1 sn=30 former=0",
        ]

    def test_entry_width_allows_multiple_instructions(self):
        lines = self.allocation() + [
            "10: system.cpu.rob: Hybrid remove id=1 sn=10 former=1 reason=commit remaining=2",
            "10: system.cpu.rob: Hybrid remove id=1 sn=20 former=0 reason=commit remaining=1",
            "10: system.cpu.rob: Hybrid remove id=1 sn=30 former=0 reason=commit remaining=0",
            "10: system.cpu.commit: 3",
        ]
        result = self.check(lines)
        self.assertEqual(result["max_successful_retire"], 3)
        self.assertEqual(result["max_entries_accessed"], 1)

    def test_latter_squash_keeps_former_with_sequence_gaps(self):
        lines = self.allocation() + [
            "5: system.cpu.rob: Hybrid squash id=1 former=1 itself=0 boundary=10",
            "5: system.cpu.rob: Hybrid remove id=1 sn=30 former=0 reason=squash remaining=2",
            "6: system.cpu.rob: Hybrid remove id=1 sn=20 former=0 reason=squash remaining=1",
            "6: system.cpu.rob: Hybrid downgrade id=1 former=1",
            "10: system.cpu.rob: Hybrid remove id=1 sn=10 former=1 reason=commit remaining=0",
            "10: system.cpu.commit: 1",
        ]
        result = self.check(lines, downgraded=1)
        self.assertEqual(result["downgrades"], 1)
        self.assertEqual(result["squashed_removed"], 2)

    def test_wrong_squash_boundary_is_rejected(self):
        lines = self.allocation() + [
            "5: system.cpu.rob: Hybrid squash id=1 former=1 itself=0 boundary=20",
            "10: system.cpu.commit: 0",
        ]
        with self.assertRaises(AssertionError):
            self.check(lines, retired=0)

    def test_missing_required_downgrade_is_rejected(self):
        lines = self.allocation() + [
            "5: system.cpu.rob: Hybrid squash id=1 former=1 itself=0 boundary=10",
            "5: system.cpu.rob: Hybrid remove id=1 sn=30 former=0 reason=squash remaining=2",
            "6: system.cpu.rob: Hybrid remove id=1 sn=20 former=0 reason=squash remaining=1",
            "10: system.cpu.rob: Hybrid remove id=1 sn=10 former=1 reason=commit remaining=0",
            "10: system.cpu.commit: 1",
        ]
        with self.assertRaises(AssertionError):
            self.check(lines)

    def test_walker_cannot_remove_retained_former(self):
        lines = self.allocation() + [
            "5: system.cpu.rob: Hybrid squash id=1 former=1 itself=0 boundary=10",
            "5: system.cpu.rob: Hybrid remove id=1 sn=30 former=0 reason=squash remaining=2",
            "6: system.cpu.rob: Hybrid remove id=1 sn=20 former=0 reason=squash remaining=1",
            "6: system.cpu.rob: Hybrid remove id=1 sn=10 former=1 reason=squash remaining=0",
            "10: system.cpu.commit: 0",
        ]
        with self.assertRaises(AssertionError):
            self.check(lines, retired=0)

    def test_full_flush_replaces_previous_slot_target(self):
        lines = self.allocation() + [
            "5: system.cpu.rob: Hybrid squash id=1 former=1 itself=0 boundary=10",
            "5: system.cpu.rob: Hybrid remove id=1 sn=30 former=0 reason=squash remaining=2",
            "6: system.cpu.rob: Hybrid full squash boundary=0",
            "6: system.cpu.rob: Hybrid remove id=1 sn=20 former=0 reason=squash remaining=1",
            "6: system.cpu.rob: Hybrid remove id=1 sn=10 former=1 reason=squash remaining=0",
            "10: system.cpu.commit: 0",
        ]
        result = self.check(lines, retired=0)
        self.assertEqual(result["squashed_removed"], 3)

    def test_retiring_two_entries_with_width_one_is_rejected(self):
        lines = self.allocation() + [
            "0: system.cpu.rob: Hybrid allocate id=2 type=0 former=1 latter=0",
            "0: system.cpu.rob: Hybrid member id=2 sn=40 former=1",
            "10: system.cpu.rob: Hybrid remove id=1 sn=10 former=1 reason=commit remaining=2",
            "10: system.cpu.rob: Hybrid remove id=1 sn=20 former=0 reason=commit remaining=1",
            "10: system.cpu.rob: Hybrid remove id=1 sn=30 former=0 reason=commit remaining=0",
            "10: system.cpu.rob: Hybrid remove id=2 sn=40 former=1 reason=commit remaining=0",
            "10: system.cpu.commit: 4",
        ]
        with self.assertRaises(AssertionError):
            self.check(lines, groups=2, insts=4, retired=2,
                       types={"CS": 1, "NORMAL-S": 1}, lengths={3: 1, 1: 1})


if __name__ == "__main__":
    unittest.main()

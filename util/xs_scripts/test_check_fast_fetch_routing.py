"""Regression tests for per-instruction normal-path delivery checks."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from check_fast_fetch_routing import check_trace


PREFIX = """\
0: cpu.fetch: packet size=2 ready=3
2: cpu.decode: route epoch=1 size=2 offered=0 valid=3 fast=0 normal=3 ready=3 cycle=2 normal_only=1
2: cpu.decode: routed seq=1 epoch=1 ftq=1 fast=0 normal=1 valid=1 ready=3
2: cpu.decode: routed seq=2 epoch=1 ftq=1 fast=0 normal=1 valid=1 ready=3
"""
ENQUEUE_FIRST = "3: cpu.decode: normal-enqueue seq=1 ready=3 cycle=3\n"
ENQUEUE_SECOND = "3: cpu.decode: normal-enqueue seq=2 ready=3 cycle=3\n"
NEXT_CYCLE = "4: cpu.commit: commit seq=1 pc=0x80000000\n"
REARM = "3: cpu.fetch: rearm-empty epoch=2 block_valid=0\n"


class NormalEnqueueTest(unittest.TestCase):
    """Reject missing, repeated and mistimed normal lanes in routed packets."""

    def check(self, trace):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "frontend.trace"
            path.write_text(trace)
            return check_trace(path)[0]

    def test_all_normal_lanes_enqueue(self):
        totals = self.check(
            PREFIX + ENQUEUE_FIRST + ENQUEUE_SECOND + NEXT_CYCLE
        )
        self.assertEqual(totals["normal_enqueued"], 2)
        self.assertEqual(totals["normal_inflight"], 0)

    def test_missing_second_lane_at_eof(self):
        with self.assertRaisesRegex(AssertionError, "missing normal enqueue"):
            self.check(PREFIX + ENQUEUE_FIRST)

    def test_missing_all_lanes_without_later_routing(self):
        with self.assertRaisesRegex(AssertionError, "missing normal enqueue"):
            self.check(PREFIX + NEXT_CYCLE)

    def test_duplicate_enqueue(self):
        with self.assertRaisesRegex(
            AssertionError, "duplicate normal enqueue"
        ):
            self.check(PREFIX + ENQUEUE_FIRST + ENQUEUE_FIRST + ENQUEUE_SECOND)

    def test_early_or_late_enqueue(self):
        for cycle in (2, 4):
            with self.subTest(cycle=cycle), self.assertRaises(AssertionError):
                self.check(
                    PREFIX
                    + f"{cycle}: cpu.decode: normal-enqueue seq=1 ready=3 cycle={cycle}\n"
                    + ENQUEUE_SECOND
                )

    def test_rearm_requires_all_normal_lanes(self):
        with self.assertRaisesRegex(AssertionError, "rearm overtakes"):
            self.check(PREFIX + ENQUEUE_FIRST + REARM + ENQUEUE_SECOND)

    def test_rearm_after_all_enqueues(self):
        totals = self.check(PREFIX + ENQUEUE_FIRST + ENQUEUE_SECOND + REARM)
        self.assertEqual(totals["empty_rearms"], 1)

    def test_final_packet_still_in_flight(self):
        totals = self.check(PREFIX)
        self.assertEqual(totals["normal_inflight"], 2)

    def test_normal_ready_requires_enqueue(self):
        with self.assertRaisesRegex(
            AssertionError, "normal ready before enqueue"
        ):
            self.check(PREFIX + "4: cpu.decode: normal-ready seq=1 cycle=4\n")


if __name__ == "__main__":
    unittest.main()

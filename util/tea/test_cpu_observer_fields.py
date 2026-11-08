"""Regression checks for declared observers in sparse CPU statistics."""
import unittest

from cpu_observer_fields import audit_added_cpu_observers


OBSERVER = "system.cpu.dcache.prefetcher.bop_large.incumbentTieSelected"
OLD_BIN = "system.cpu.dcache.ReadReq.missLatencyDist::0-9"
NEW_BIN = "system.cpu.dcache.ReadReq.missLatencyDist::10-19"


class ObserverFieldTests(unittest.TestCase):
    def test_sparse_bins_change_with_behavior(self):
        before = [{"system.cpu.numCycles": 100, OLD_BIN: 2}]
        after = [{"system.cpu.numCycles": 95, NEW_BIN: 2, OBSERVER: 0}]
        result = audit_added_cpu_observers(before, after, {OBSERVER})
        self.assertTrue(result["passed"])
        self.assertEqual(result["phase_output_changes"], [{
            "phase": 0,
            "other_added_output_keys": [NEW_BIN],
            "other_removed_output_keys": [OLD_BIN],
        }])

    def test_missing_observer_is_not_a_sparse_bin(self):
        with self.assertRaises(AssertionError):
            audit_added_cpu_observers([{}], [{NEW_BIN: 1}], {OBSERVER})

    def test_preexisting_field_is_not_an_added_observer(self):
        with self.assertRaises(AssertionError):
            audit_added_cpu_observers([{OBSERVER: 0}], [{OBSERVER: 1}],
                                      {OBSERVER})

    def test_phase_count_mismatch_is_rejected(self):
        with self.assertRaises(ValueError):
            audit_added_cpu_observers([{}], [], {OBSERVER})


if __name__ == "__main__":
    unittest.main()

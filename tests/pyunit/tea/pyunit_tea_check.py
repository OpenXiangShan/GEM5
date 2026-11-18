"""Exercise sampled-state attribution in the real TEA tracker."""
from collections import Counter
import csv
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).resolve().parent / "fixtures"


def csv_rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


class TrackerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.output = Path(cls.temp.name)
        binary = cls.output / "tea-test"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-O0",
                "-Wall",
                "-Wextra",
                "-I" + str(FIXTURES),
                "-I" + str(ROOT / "src"),
                str(ROOT / "src/cpu/o3/tea.cc"),
                str(FIXTURES / "main.cc"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], cwd=cls.output, check=True)

    def rows(self, case):
        return csv_rows(self.output / (case + ".csv"))

    def test_all_thirteen_state_and_quality_budgets(self):
        cases = sorted(self.output.glob("*.states.csv"))
        self.assertEqual(len(cases), 13)
        for path in cases:
            case = path.name.removesuffix(".states.csv")
            with self.subTest(case=case):
                states, quality = Counter(), Counter()
                for row in self.rows(case):
                    weight = float(row["sample_weight"])
                    states[row["state"]] += weight
                    psv = int(row["psv"])
                    quality["attributed" if psv >= 0 else psv] += weight
                expected = {
                    row["state"]: float(row["weight"])
                    for row in csv_rows(path)
                }
                for state, weight in expected.items():
                    self.assertAlmostEqual(states[state], weight)
                counts = {
                    row["counter"]: float(row["value"])
                    for row in csv_rows(self.output / (case + ".counts.csv"))
                }
                self.assertAlmostEqual(sum(states.values()), counts["samples"])
                for key, field in [
                    ("attributed", "attributed"),
                    (-1, "cancelled"),
                    (-2, "pending"),
                    (-3, "unresolved"),
                ]:
                    self.assertAlmostEqual(quality[key], counts[field])

    def test_different_waiting_pcs_have_identical_old_marginals(self):
        marginal = []
        stalled = []
        for case in ["ambiguity-a", "ambiguity-b"]:
            totals, waits = Counter(), Counter()
            for row in self.rows(case):
                weight = float(row["sample_weight"])
                totals[row["thread"], row["pc"], row["psv"]] += weight
                if row["state"] == "Stalled":
                    waits[row["pc"]] += weight
            marginal.append(totals)
            stalled.append(waits)
        self.assertEqual(marginal[0], marginal[1])
        self.assertEqual(stalled, [{"0x1000": 1}, {"0x2000": 1}])

    def test_retirement_does_not_reclassify_prior_waiting(self):
        rows = self.rows("collision")
        self.assertEqual(
            {(r["state"], float(r["sample_weight"])) for r in rows},
            {("Computing", 1), ("Stalled", 1)},
        )
        waiting = [
            r for r in self.rows("late-events") if r["state"] == "Stalled"
        ]
        self.assertEqual(len(waiting), 1)
        self.assertNotEqual(int(waiting[0]["psv"]), 0)

    def test_cancelled_and_pending_keep_sampling_state(self):
        self.assertTrue(
            all(r["state"] == "Stalled" for r in self.rows("cancelled"))
        )
        self.assertTrue(all(r["psv"] == "-1" for r in self.rows("cancelled")))
        pending = self.rows("pending")
        self.assertEqual({r["state"] for r in pending}, {"Stalled", "Drained"})
        self.assertTrue(all(r["psv"] == "-2" for r in pending))


if __name__ == "__main__":
    unittest.main()

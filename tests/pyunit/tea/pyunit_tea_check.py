"""Exercise the real TEA tracker and the raw-window analysis contract."""
from collections import Counter
import csv
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = Path(__file__).resolve().parent / "fixtures"
SPEC = importlib.util.spec_from_file_location(
    "tea_analysis", ROOT / "util/tea/analyze_spec17_tea_dip.py"
)
ANALYSIS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ANALYSIS)


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


class WindowTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        (self.folder / "config.ini").write_text(
            "[system.cpu]\n"
            "enableDip=true\nenableTea=true\nenable_difftest=true\n"
            "enable_mem_dedup=true\nnumThreads=1\nclk_domain=clock\n"
            "dipSamplePeriod=1\nteaSamplePeriod=1\nrenameWidth=8\n"
            "warmupInstCount=2\nmax_insts_any_thread=4\n"
            "[clock]\nclock=333\n"
        )
        counters = {
            "committedInsts": 2,
            "numCycles": 4,
            "commit.branchMispredicts": 0,
            "dip.sampledSlots": 32,
            "dip.attributedSlots": 24,
            "dip.unresolvedSlots": 0,
            "dip.pendingSlots": 8,
            "tea.samples": 4,
            "tea.attributedSamples": 2,
            "tea.cancelledTargets": 0,
            "tea.pendingSamples": 1,
            "tea.unresolvedTargets": 1,
        }
        for state in ANALYSIS.DIP_STATES:
            counters["dip.slotsByState::" + state] = (
                0 if state == "FrontEnd" else 8
            )
        for state in ANALYSIS.TEA_STATES:
            counters["tea.samplesByState::" + state] = 1
        block = "---------- Begin Simulation Statistics ----------\n"
        block += "system.l2.unused 123\n"
        block += "".join(
            f"system.cpu.{key} {value}\n" for key, value in counters.items()
        )
        block += "---------- End Simulation Statistics ----------\n"
        (self.folder / "stats.txt").write_text(block + block)
        (self.folder / "system.cpu.dip.csv").write_text(
            "pc,state,slot_weight,cycle_weight,resolution,cause,iq,iq_flags,frontend_reason\n"
            "0x1000,Base,8,1,resolved,0,0,0,0\n"
            "0x1000,Stall,8,1,resolved,3,0,0,0\n"
            "0x1000,Misspeculation,8,1,resolved,0,0,0,0\n"
            "0x1000,,8,1,pending,0,0,0,0\n"
        )
        (self.folder / "system.cpu.tea.csv").write_text(
            "thread,pc,psv,sample_weight,state\n"
            "0,0x1000,0,1,Computing\n"
            "0,,-3,1,Stalled\n"
            "0,0x1010,0,1,Flushed\n"
            "0,,-2,1,Drained\n"
        )

    def test_width_pending_and_no_pc_are_preserved(self):
        data, _, _, _ = ANALYSIS.window(self.folder, True)
        self.assertEqual(data["dispatch_width"], 8)
        self.assertEqual(data["CPI"], 2)
        self.assertEqual(data["DIP_pending_CPI"], 0.5)
        self.assertEqual(data["TEA_no_PC_CPI"], 1)
        self.assertEqual(
            sum(data["DIP_CPI"].values()) + data["DIP_pending_CPI"], 2
        )
        self.assertEqual(data["DIP_overlapping_causes_CPI"]["S_ROB"], 0.5)
        self.assertEqual(data["DIP_overlapping_causes_CPI"]["S_ISB"], 0.5)
        self.assertTrue(data["TEA_state_PC_available"])
        self.assertNotIn(
            "system.l2.unused",
            ANALYSIS.stats_blocks(self.folder / "stats.txt")[0],
        )

    def test_wrong_state_assignment_is_rejected_even_when_total_matches(self):
        path = self.folder / "system.cpu.tea.csv"
        path.write_text(path.read_text().replace("1,Stalled", "1,Computing"))
        with self.assertRaises(AssertionError):
            ANALYSIS.window(self.folder)

    def test_sparse_samples_are_not_interpreted_as_all_cycle_cpi(self):
        path = self.folder / "config.ini"
        path.write_text(
            path.read_text().replace("teaSamplePeriod=1", "teaSamplePeriod=7")
        )
        with self.assertRaises(AssertionError):
            ANALYSIS.window(self.folder)

    def test_official_in_progress_is_not_suite_success(self):
        run = self.folder / "run.json"
        run.write_text('{"status":"in_progress","conclusion":""}')
        with self.assertRaises(AssertionError):
            ANALYSIS.suite(
                self.folder,
                run,
                self.folder / "ref.json",
                self.folder / "dispatch.json",
            )


if __name__ == "__main__":
    unittest.main()

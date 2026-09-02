import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "aggregate_kmhv2_break_stats.py"


def load_module():
    spec = importlib.util.spec_from_file_location("aggregate_crob", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_stats(root, slice_name, *, simple, load, branch, breaking_load,
                breaking_branch, break_blocks, physical, no_break, lost,
                run_1, run_2):
    path = root / slice_name / "m5out" / "stats.txt"
    path.parent.mkdir(parents=True)
    path.write_text(
        "\n".join(
            [
                "system.cpu.commit.crobKmhv2AnalyzedBundles 10",
                f"system.cpu.commit.crobKmhv2InstClass::simpleIntegerAlu {simple}",
                "system.cpu.commit.crobKmhv2InstClass::simpleFloatingAlu 0",
                "system.cpu.commit.crobKmhv2InstClass::simpleOther 0",
                f"system.cpu.commit.crobKmhv2InstClass::load {load}",
                "system.cpu.commit.crobKmhv2InstClass::store 0",
                f"system.cpu.commit.crobKmhv2InstClass::branch {branch}",
                "system.cpu.commit.crobKmhv2InstClass::jump 0",
                "system.cpu.commit.crobKmhv2InstClass::otherComplex 0",
                f"system.cpu.commit.crobKmhv2BreakingInstClass::load {breaking_load}",
                f"system.cpu.commit.crobKmhv2BreakingInstClass::branch {breaking_branch}",
                f"system.cpu.commit.crobKmhv2BreakBlocks {break_blocks}",
                f"system.cpu.commit.crobKmhv2SimpleRunLength::1 {run_1}",
                f"system.cpu.commit.crobKmhv2SimpleRunLength::2 {run_2}",
                f"system.cpu.commit.crobKmhv2PhysicalEntries {physical}",
                f"system.cpu.commit.crobKmhv2NoBreakPhysicalEntries {no_break}",
                f"system.cpu.commit.crobKmhv2BreakLostEntries {lost}",
            ]
        )
        + "\n",
        encoding="utf-8",
    )


class AggregateKmhv2BreakStatsTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def write_weights(self, data):
        path = self.root / "weights.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def test_collects_slices_and_computes_normalized_simpoint_weighting(self):
        module = load_module()
        stats_root = self.root / "spec_all"
        write_stats(
            stats_root, "mcf_11", simple=80, load=10, branch=10,
            breaking_load=4, breaking_branch=1, break_blocks=5,
            physical=40, no_break=35, lost=5, run_1=10, run_2=35,
        )
        write_stats(
            stats_root, "mcf_22", simple=60, load=20, branch=20,
            breaking_load=8, breaking_branch=2, break_blocks=10,
            physical=50, no_break=40, lost=10, run_1=20, run_2=20,
        )
        weights = self.write_weights({
            "mcf": {"insts": "1", "points": {"11": "0.25", "22": "0.75"}}
        })

        slices, workloads, break_dist, run_dist = module.collect_results(
            stats_root, weights
        )

        self.assertEqual([row["slice"] for row in slices], ["mcf_11", "mcf_22"])
        self.assertEqual([row["normalized_weight"] for row in slices], [0.25, 0.75])
        self.assertEqual(slices[0]["simple_fraction"], 0.8)
        self.assertEqual(slices[0]["break_lost_fraction"], 0.125)
        self.assertEqual(workloads[0]["workload"], "mcf")
        self.assertEqual(workloads[0]["simple_fraction"], 0.25 * 0.8 + 0.75 * 0.6)
        self.assertEqual(workloads[0]["break_blocks_per_1k_inst"], 0.25 * 50 + 0.75 * 100)
        self.assertEqual(workloads[0]["actual_rob_density"], 0.25 * 2.5 + 0.75 * 2.0)
        self.assertEqual(workloads[-1]["workload"], "__suite_mean__")
        self.assertEqual(workloads[-1]["simple_fraction"], workloads[0]["simple_fraction"])
        mcf_break_dist = [row for row in break_dist if row["workload"] == "mcf"]
        self.assertAlmostEqual(sum(row["fraction"] for row in mcf_break_dist), 1.0)
        self.assertTrue(any(row["workload"] == "__suite_mean__" for row in break_dist))
        mcf_run_dist = [row for row in run_dist if row["workload"] == "mcf"]
        self.assertAlmostEqual(sum(row["fraction"] for row in mcf_run_dist), 1.0)
        self.assertTrue(any(row["workload"] == "__suite_mean__" for row in run_dist))

    def test_cli_writes_four_csv_files(self):
        module = load_module()
        stats_root = self.root / "spec_all"
        write_stats(
            stats_root, "gcc_7", simple=8, load=1, branch=1,
            breaking_load=1, breaking_branch=0, break_blocks=1,
            physical=4, no_break=3, lost=1, run_1=2, run_2=3,
        )
        weights = self.write_weights({
            "gcc": {"insts": "1", "points": {"7": "0.4"}}
        })
        output = self.root / "output"

        self.assertEqual(module.main([
            str(stats_root), "--weights", str(weights), "--output-dir", str(output)
        ]), 0)

        expected = {
            "crob_kmhv2_slices.csv",
            "crob_kmhv2_workloads.csv",
            "crob_kmhv2_breaking_instruction_distribution.csv",
            "crob_kmhv2_simple_run_distribution.csv",
        }
        self.assertEqual({path.name for path in output.iterdir()}, expected)
        with (output / "crob_kmhv2_workloads.csv").open(newline="") as handle:
            self.assertEqual(list(csv.DictReader(handle))[0]["workload"], "gcc")

    def test_rejects_stats_without_crob_counters(self):
        module = load_module()
        path = self.root / "mcf_1" / "m5out" / "stats.txt"
        path.parent.mkdir(parents=True)
        path.write_text("simTicks 123\n", encoding="utf-8")
        weights = self.write_weights({
            "mcf": {"insts": "1", "points": {"1": "1.0"}}
        })

        with self.assertRaisesRegex(ValueError, "CROB kmhv2 counters"):
            module.collect_results(self.root, weights)

    def test_rejects_slice_without_simpoint_weight(self):
        module = load_module()
        write_stats(
            self.root, "mcf_99", simple=8, load=1, branch=1,
            breaking_load=1, breaking_branch=0, break_blocks=1,
            physical=4, no_break=3, lost=1, run_1=2, run_2=3,
        )
        weights = self.write_weights({
            "mcf": {"insts": "1", "points": {"1": "1.0"}}
        })

        with self.assertRaisesRegex(ValueError, "missing simpoint weight.*mcf_99"):
            module.collect_results(self.root, weights)


if __name__ == "__main__":
    unittest.main()

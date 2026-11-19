import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from compare_windows import compare_manifest, render, pc_comparison
from test_analyzer import Trace


class WindowTests(unittest.TestCase):
    def test_signed_pc_overlap_preserves_context(self):
        a = ("L1", "1", "as", "0", "0x10")
        b = ("L1", "2", "as", "0", "0x10")
        result = pc_comparison({a: 10, b: 1}, {a: -5, b: 2}, 1)
        self.assertEqual((result["shared"], result["same_sign"]), (1, 0))
        self.assertEqual(pc_comparison({a: 1}, {b: 1}, 1)["shared"], 0)

    def test_end_to_end(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def make_pair(name, count, candidate_cycles):
                for role, cycles in (("control", count), ("candidate", candidate_cycles)):
                    d = root / name / role
                    d.mkdir(parents=True)
                    t = Trace()
                    t.demand(100, 1, "0x1000", "miss" if role == "control" else "hit")
                    t.write(d / "prefetch-trace.csv")
                    (d / "config.ini").write_text("[system]\ntype=System\n")
                    (d / "stats.txt").write_text(
                        f"Begin Simulation Statistics\nsystem.cpu.numCycles {cycles}\n"
                        f"system.cpu.committedInsts {count}\nfinalTick 10000\n"
                        "simTicks 10000\nEnd Simulation Statistics\n")
                base = root / name
                subprocess.run([sys.executable, Path(__file__).with_name("analyze.py"),
                                base / "candidate", "--control", base / "control",
                                "--output", base / "report"], check=True, capture_output=True)
                return dict(start=0, end=count, warmup=0, report=f"{name}/report")

            full = make_pair("full", 100, 80)
            short = make_pair("short", 20, 25)
            second = make_pair("second", 100, 120)
            points = [dict(checkpoint="a", weight=.5, full=full, short=[short])]
            data = dict(benchmark="demo", experiment="a/b", reference="control", slices=points)
            path = root / "manifest.json"
            def run():
                path.write_text(json.dumps(data))
                return compare_manifest(path)
            result = run()
            self.assertIsNone(result["benchmark_speedup_pct"])
            s = result["slices"][0]["short"][0]
            self.assertFalse(s["direction_agrees"])
            self.assertAlmostEqual(s["speedup_gap_pp"], -45)
            self.assertEqual(s["miss_mpki"], [dict(reason="no_prediction", short=-50, full=-10)])
            self.assertIn("方向不一致", render(result))
            points.append(dict(checkpoint="b", weight=.5, full=second, short=[]))
            self.assertAlmostEqual(run()["benchmark_speedup_pct"], 0)
            del second["report"]
            second["stats_pair"] = dict(control="second/control", candidate="second/candidate")
            result = run()
            self.assertAlmostEqual(result["benchmark_speedup_pct"], 0)
            self.assertEqual(result["slices"][1]["full"]["audit"], ["stats_only"] * 2)
            short.update(start=95, end=115)
            with self.assertRaisesRegex(ValueError, "strict subwindow"):
                run()
            short.update(start=0, end=19)
            with self.assertRaisesRegex(ValueError, "retired count"):
                run()

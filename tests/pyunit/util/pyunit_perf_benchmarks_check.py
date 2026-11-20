from dataclasses import replace
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


MODULE_PATH = (
    Path(__file__).resolve().parents[3] / "util/xs_scripts/perf_benchmarks.py"
)
SPEC = importlib.util.spec_from_file_location("perf_benchmarks", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class CustomBenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.profile = self.base / "spec17_example"
        self.root = self.profile / "checkpoint"
        image_dir = self.root / "gcc_input" / "42"
        image_dir.mkdir(parents=True)
        self.image = image_dir / "test.zstd"
        self.image.touch()
        self.cluster = self.profile / "json" / "checkpoints_all.json"
        self.cluster.parent.mkdir()
        self.cluster.write_text(
            json.dumps(
                {"gcc_input": {"insts": "123456", "points": {"42": "1.0"}}}
            )
        )

    def resolve(self, kind="custom", root=None, cluster=""):
        return MODULE.resolve_custom_benchmark(
            kind, str(root or self.profile), str(cluster), self.base / "output"
        )

    def test_profile_and_checkpoint_root(self):
        config = self.resolve()
        self.assertEqual(
            config.github_outputs()["archive_subdir"], "custom/spec17_example"
        )
        self.assertEqual(config, self.resolve(root=self.root))
        self.assertEqual(config.score_script, "gem5-score-ci-17.sh")
        self.assertEqual(
            Path(config.checkpoint_list).read_text(),
            "gcc_input_42 gcc_input/42 0 0 20 20\n",
        )

    def test_explicit_suite_and_json(self):
        for suite, script in [
            ("06", "gem5-score-ci.sh"),
            ("17", "gem5-score-ci-17.sh"),
            ("26", "gem5-score-ci-26.sh"),
        ]:
            config = self.resolve(f"custom-spec{suite}", cluster=self.cluster)
            self.assertEqual(config.score_script, script)

    def test_legacy_layout(self):
        legacy = self.profile / "checkpoint-0-0-0"
        self.root.rename(legacy)
        self.cluster.rename(legacy / "cluster-0-0.json")
        self.assertEqual(self.resolve().checkpoint_root, str(legacy))

    def test_missing_or_ambiguous_image(self):
        self.image.unlink()
        with self.assertRaisesRegex(
            ValueError, "expected one checkpoint image"
        ):
            self.resolve()
        self.image.touch()
        self.image.with_suffix(".gz").touch()
        with self.assertRaisesRegex(
            ValueError, "expected one checkpoint image"
        ):
            self.resolve()

    def test_no_implicit_weights_fallback(self):
        self.cluster.unlink()
        with self.assertRaisesRegex(ValueError, "full-coverage JSON"):
            self.resolve()

    def test_ambiguous_json_requires_override(self):
        (self.root / "cluster-0-0.json").write_text(self.cluster.read_text())
        with self.assertRaisesRegex(ValueError, "full-coverage JSON"):
            self.resolve()
        self.resolve(cluster=self.cluster)

    def test_unknown_suite_requires_override(self):
        renamed = self.base / "new_profile"
        self.profile.rename(renamed)
        with self.assertRaisesRegex(ValueError, "cannot infer SPEC suite"):
            self.resolve(root=renamed)
        self.resolve("custom-spec17", root=renamed)

    def test_invalid_metadata(self):
        for profile in [
            {},
            {"gcc_input": {"insts": 1, "points": {}}},
            {"gcc_input": {"insts": 0, "points": {"42": 1}}},
            {"gcc_input": {"insts": 1, "points": {"42": "nan"}}},
        ]:
            self.cluster.write_text(json.dumps(profile))
            with self.assertRaises(ValueError):
                self.resolve()

    def test_registered_profiles_unchanged(self):
        for name in MODULE.benchmark_types():
            if name == "h-spec06-0.3c":
                continue  # Covered separately with a checkpoint profile.
            config = MODULE.resolve_benchmark(name)
            self.assertEqual(config.benchmark_type, name)
            self.assertEqual(config.github_outputs()["archive_subdir"], name)
            self.assertEqual(config.github_outputs()["benchmark_type"], name)


class HSpec06BenchmarkTest(unittest.TestCase):
    NAME = "h-spec06-0.3c"

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        base = Path(temp.name)
        self.output = base / "output"
        self.full_list = base / "checkpoint.lst"
        self.full_list.write_text(
            "# checkpoint list\n"
            "gcc_10 gcc/10 0 0 20 20\n"
            "gcc_11 gcc/11 1 2 3 4\n"
            "mcf_20 mcf/20 5 6 7 8\n"
        )
        self.weights = base / "checkpoints_cov0.3.json"
        self.weights.write_text(
            json.dumps(
                {
                    "mcf": {"points": {"20": "0.7"}},
                    "gcc": {"points": {"11": "0.5"}},
                }
            )
        )
        subset = replace(
            MODULE._BENCHMARKS[self.NAME],
            checkpoint_list=str(self.full_list),
            checkpoint_root=str(base),
            cluster_config=str(self.weights),
        )
        patcher = patch.dict(MODULE._BENCHMARKS, {self.NAME: subset})
        patcher.start()
        self.addCleanup(patcher.stop)

    def resolve(self):
        return MODULE.resolve_benchmark(self.NAME, output_dir=self.output)

    def test_subset_matches_weights_and_preserves_list_order_and_fields(self):
        outputs = self.resolve().github_outputs()
        self.assertEqual(outputs["benchmark_type"], self.NAME)
        self.assertEqual(outputs["cluster_config"], str(self.weights))
        self.assertEqual(
            Path(outputs["checkpoint_list"]).read_text(),
            "gcc_11 gcc/11 1 2 3 4\nmcf_20 mcf/20 5 6 7 8\n",
        )

    def test_missing_weighted_point_is_rejected(self):
        self.full_list.write_text("gcc_11 gcc/11 0 0 20 20\n")
        with self.assertRaisesRegex(ValueError, "missing.*mcf/20"):
            self.resolve()
        self.assertFalse(self.output.exists())

    def test_duplicate_weighted_point_is_rejected(self):
        with self.full_list.open("a") as listing:
            listing.write("gcc_11 gcc/11 0 0 20 20\n")
        with self.assertRaisesRegex(ValueError, "duplicate checkpoint gcc/11"):
            self.resolve()
        self.assertFalse(self.output.exists())

    def test_full_coverage_uses_archive_list_and_weights_directly(self):
        config = MODULE.resolve_benchmark("h-spec06-1.0c")
        root = Path(config.checkpoint_root).parent
        self.assertEqual(root.name, "spec06_gcc16_rv64gcb_h_260822")
        self.assertEqual(
            Path(config.checkpoint_list), root / "checkpoint/checkpoint.lst"
        )
        self.assertEqual(
            Path(config.cluster_config), root / "json/checkpoints_all.json"
        )


if __name__ == "__main__":
    unittest.main()

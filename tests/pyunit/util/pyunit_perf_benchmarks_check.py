import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import time


sys.path.insert(
    0, str(Path(__file__).resolve().parents[3] / "util/xs_scripts")
)
from ci import benchmarks as MODULE
from ci.run_perf_ci import archive, complete_stats, prune
from ci.runner import Job, RunnerConfig, run_jobs, start


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
            kind, str(root or self.profile), str(cluster)
        )

    def test_profile_and_checkpoint_root(self):
        config = self.resolve()
        self.assertEqual(config.archive_subdir, "custom/spec17_example")
        self.assertEqual(config, self.resolve(root=self.root))
        self.assertEqual(config.score_script, "gem5-score-ci-17.sh")
        self.assertEqual(
            list(MODULE.workloads(config))[0].row + "\n",
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
        self.assertEqual(self.resolve().root, legacy)

    def test_missing_or_ambiguous_image(self):
        self.image.unlink()
        with self.assertRaisesRegex(
            ValueError, "expected one checkpoint image"
        ):
            list(MODULE.workloads(self.resolve()))
        self.image.touch()
        self.image.with_suffix(".gz").touch()
        with self.assertRaisesRegex(
            ValueError, "expected one checkpoint image"
        ):
            list(MODULE.workloads(self.resolve()))

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
                list(MODULE.workloads(self.resolve()))

    def test_gcc12_catalog(self):
        self.assertNotIn("gcc12-spec06-0.3c", MODULE.CATALOG)
        self.assertIn("gcc12-spec06-smt-0.3c", MODULE.CATALOG)
        self.assertEqual(
            MODULE.resolve_benchmark("gcc12-spec06-1.0c").weights,
            MODULE.resolve_benchmark("gcc12-spec06-smt-1.0c").weights,
        )

    def test_filter_and_trace_windows(self):
        (self.base / "a.trace.zstd").touch()
        rows = ["slice1 a.trace 0 1 2 4", "slice2 missing 0 1 2 4"]
        (w,) = MODULE.select_workloads(rows, self.base, "SLICE1", "tracertl")
        self.assertIn("--maxinsts=7", w.arguments("tracertl"))
        self.assertIn("--warmup-insts-no-switch=3", w.arguments("tracertl"))
        with self.assertRaisesRegex(ValueError, "no workloads"):
            list(MODULE.select_workloads(rows, self.base, "not-found"))


class RunnerTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def job(self, name, code):
        return Job(
            "", name, self.root, self.root / name, ["python3", "-c", code], {}
        )

    def test_initial_fallback_and_results(self):
        jobs = [self.job("a", "pass"), self.job("b", "raise SystemExit(23)")]
        with patch("ci.hosts.probe_idle_cpus", return_value=(0, "busy")):
            results = run_jobs(
                jobs,
                RunnerConfig(servers="node037"),
                1,
                fallback_to_local=2,
                log=lambda _: None,
            )
        self.assertEqual(
            {r.job.workload_name: r.return_code for r in results},
            {"a": 0, "b": 23},
        )
        self.assertTrue((self.root / "a/completed").is_file())
        self.assertTrue((self.root / "b/abort").is_file())
        self.assertFalse(list(self.root.glob("*/running")))

    def test_deadline_stops_running_and_pending(self):
        jobs = [
            self.job(str(i), "import time;time.sleep(60)") for i in range(2)
        ]
        results = run_jobs(
            jobs,
            RunnerConfig(),
            1,
            deadline=time.monotonic() + 0.5,
            log=lambda _: None,
        )
        self.assertEqual([r.status for r in results], ["timeout", "timeout"])
        self.assertEqual(len(list(self.root.glob("*/abort"))), 2)

    def test_missing_statistics_fail_successful_command(self):
        (result,) = run_jobs(
            [self.job("missing", "pass")],
            RunnerConfig(),
            1,
            validate_output=lambda job: complete_stats(
                job.work_dir / "stats.txt"
            ),
            log=lambda _: None,
        )
        self.assertEqual(result.status, "failed")
        self.assertTrue((result.job.work_dir / "abort").is_file())

    def test_busy_host_is_probed_again(self):
        config = RunnerConfig(
            servers="node037", jobs_per_server=1, probe_interval=0.2
        )
        with patch(
            "ci.hosts.probe_idle_cpus", side_effect=[(0, ""), (1, "")]
        ), patch(
            "ci.runner.start",
            side_effect=lambda job, _, cfg, attempt: start(
                job, "local", cfg, attempt
            ),
        ):
            (result,) = run_jobs(
                [self.job("queued", "pass")],
                config,
                1,
                deadline=time.monotonic() + 5,
                log=lambda _: None,
            )
        self.assertEqual(result.status, "completed")
        self.assertEqual(result.server_name, "node037")

    def test_cancel_before_launch(self):
        (result,) = run_jobs(
            [self.job("a", "pass")],
            RunnerConfig(),
            1,
            cancelled=lambda: True,
            log=lambda _: None,
        )
        self.assertEqual(result.status, "cancelled")
        self.assertFalse((self.root / "a/log.txt").exists())


class ArchiveTest(unittest.TestCase):
    def test_statistics_must_have_finished_dump(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "stats.txt"
            self.assertFalse(complete_stats(path))
            path.write_text("Begin Simulation Statistics")
            self.assertFalse(complete_stats(path))
            path.write_text(
                "---------- End Simulation Statistics   ----------\n"
            )
            self.assertTrue(complete_stats(path))

    def test_retention_and_concurrent_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for i in range(201):
                run = root / f"20261101_000000_abc_x_run{i:03d}"
                run.mkdir()
                (run / "metadata.txt").touch()
                (run / "finished").touch()
            active = root / "20261101_000000_abc_w_run0"
            active.mkdir()
            (active / "metadata.txt").touch()
            (active / "launcher.active").touch()
            (active / "score.txt").touch()
            prune(root)
            self.assertEqual(len(list(root.glob("*/finished"))), 200)
            self.assertTrue(active.exists())
            with archive(root, "20261102_000000_abc_x", "0") as first:
                with archive(root, "20261102_000000_abc_x", "0") as second:
                    self.assertNotEqual(first, second)
                    self.assertTrue((first / "launcher.active").exists())
                self.assertTrue((second / "finished").exists())


if __name__ == "__main__":
    unittest.main()

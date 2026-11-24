import json
from argparse import Namespace
from dataclasses import replace
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import time


sys.path.insert(
    0, str(Path(__file__).resolve().parents[3] / "util/xs_scripts")
)
from ci import benchmarks as MODULE
from ci.run_perf_ci import (
    archive,
    check_archive,
    finish_perf,
    prepare_command,
    prune,
    worker_stats,
)
from ci.stats_probe import stats_state
from ci.runner import (
    CHECKPOINT_ENV,
    Job,
    Result,
    RunnerConfig,
    run_jobs,
    start,
    stop,
)


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

    def test_archive_timeout_withholds_strict_score_and_preserves_exit(self):
        cfg = RunnerConfig(launch_interval=0)
        jobs = [self.job("visible", "pass"), self.job("missing", "pass")]
        results = run_jobs(jobs, cfg, 1)
        stats = jobs[0].work_dir / "m5out/stats.txt"
        stats.parent.mkdir()
        stats.write_text("---------- End Simulation Statistics ----------\n")
        profile = MODULE.resolve_benchmark("gcc12-spec06-smt-0.3c")
        with patch("ci.run_perf_ci.time.monotonic", side_effect=[0, 61]):
            rc = finish_perf(
                profile,
                self.root,
                self.root,
                results,
                cfg,
                True,
                self.root / "summary",
            )
        self.assertEqual(rc, 1)
        self.assertFalse((self.root / "score.txt").exists())
        self.assertIn("Scorable 1/2", (self.root / "summary").read_text())
        self.assertIn(
            "missing: exit=0 on local, archive stats missing",
            (self.root / "metadata.txt").read_text(),
        )
        self.assertTrue((jobs[1].work_dir / "completed").exists())
        self.assertFalse((jobs[1].work_dir / "abort").exists())

    def test_trace_uses_verified_set_even_with_old_archive_markers(self):
        name = "example"
        job = self.job(name, "pass")
        job = replace(job, work_dir=self.root / "spec_all" / name)
        job.work_dir.mkdir(parents=True)
        (job.work_dir / "abort").touch()
        (job.work_dir / "stats.txt").write_text(
            "---------- Begin Simulation Statistics ----------\n"
            "simInsts 20\nsystem.cpu.numCycles 10\n"
            "system.cpu.commit.branchMispredicts 1\n"
            "---------- End Simulation Statistics ----------\n"
        )
        profile = MODULE.BenchmarkProfile(
            "trace", self.root, None, trace_format="tracertl"
        )
        result = Result(job, "completed", 0, "local", 0, 1)
        self.assertEqual(
            finish_perf(
                profile,
                self.root,
                self.root,
                [result],
                RunnerConfig(),
                True,
                None,
            ),
            0,
        )
        self.assertIn(
            "Overall IPC (equal trace weights): 2.000000",
            (self.root / "score.txt").read_text(),
        )

    def test_non_strict_spec_labels_partial_score(self):
        profile = MODULE.resolve_benchmark("gcc12-spec06-smt-0.3c")
        results = [
            Result(self.job(n, "pass"), status, rc, "local", 0, 1)
            for n, status, rc in [
                ("ok", "completed", 0),
                ("failed", "failed", 23),
            ]
        ]
        directory = self.root / "ok/m5out"
        directory.mkdir(parents=True)
        (directory / "stats.txt").write_text(
            "---------- End Simulation Statistics ----------\n"
        )
        scorer = self.root / "tools/example-scripts"
        scorer.mkdir(parents=True)
        (scorer / profile.score_script).write_text(
            'test -L "$1/ok" && test ! -e "$1/failed" && echo score'
        )
        with patch("ci.run_perf_ci.SCORER", scorer.parent):
            rc = finish_perf(
                profile,
                self.root,
                self.root,
                results,
                RunnerConfig(),
                False,
                self.root / "summary",
            )
        self.assertEqual(rc, 0)
        self.assertIn(
            "Partial score. Scorable 1/2",
            (self.root / "score.txt").read_text(),
        )
        self.assertIn("exit=23", (self.root / "summary").read_text())

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

    def test_fixed_pool_probes_once(self):
        cfg = RunnerConfig(
            servers="node037",
            jobs_per_server=1,
            probe_interval=0,
            launch_interval=0,
        )
        with patch(
            "ci.hosts.probe_idle_cpus", return_value=(1, "")
        ) as probe, patch(
            "ci.runner.start", return_value=Mock(stdin=None, poll=lambda: 0)
        ):
            results = run_jobs(
                [self.job(n, "pass") for n in ("a", "b")], cfg, 2
            )
        self.assertEqual([r.status for r in results], ["completed"] * 2)
        probe.assert_called_once()

    def test_refresh_counts_occupied_slots(self):
        cfg = RunnerConfig(
            servers="node037",
            jobs_per_server=2,
            probe_interval=0.01,
            launch_interval=0,
        )
        jobs = [self.job(n, "pass") for n in ("a", "b", "c")]
        started = []

        def launch(job, *args):
            started.append(job.workload_name)

            def poll():
                if job is jobs[0] and probe.call_count < 2:
                    return None
                if job is jobs[1] and len(started) < 3:
                    return None
                return 0

            return Mock(stdin=None, poll=poll)

        # The first two jobs fill the host. A reprobe sees zero idle cores.
        with patch("ci.hosts.probe_idle_cpus") as probe, patch(
            "ci.runner.start", side_effect=launch
        ), patch("ci.runner.stop"):
            probe.side_effect = lambda *args: (2 if not started else 0, "")
            results = run_jobs(jobs, cfg, 3, deadline=time.monotonic() + 3)
        self.assertEqual([r.status for r in results], ["completed"] * 3)

    def test_ssh_launch_failure_moves_pending_jobs_to_healthy_host(self):
        cfg = RunnerConfig(
            servers="node036,node037",
            require_idle_cpus=0,
            jobs_per_server=1,
            probe_interval=0,
            retry_delay=0.1,
            launch_interval=0,
        )

        def launch(job, node, *args):
            return Mock(
                stdin=None, poll=lambda: 255 if node == "node036" else 0
            )

        with patch("ci.runner.start", side_effect=launch) as started:
            results = run_jobs(
                [self.job(n, "pass") for n in ("a", "b")], cfg, 1
            )
        self.assertEqual(
            [c.args[1] for c in started.call_args_list],
            ["node036", "node037", "node037"],
        )
        self.assertTrue(all(r.status == "completed" for r in results))

    def test_last_failed_host_can_recover(self):
        cfg = RunnerConfig(
            servers="node037",
            jobs_per_server=1,
            probe_interval=0,
            retry_delay=0.1,
            launch_interval=0,
        )
        with patch(
            "ci.hosts.probe_idle_cpus", return_value=(1, "")
        ) as probe, patch(
            "ci.runner.start",
            side_effect=[
                Mock(stdin=None, poll=lambda: 255),
                Mock(stdin=None, poll=lambda: 0),
            ],
        ):
            (result,) = run_jobs(
                [self.job("a", "pass")], cfg, 1, deadline=time.monotonic() + 3
            )
        self.assertEqual(probe.call_count, 2)
        self.assertEqual(result.status, "completed")

    def test_late_stats_release_slot_and_rejoin_scoring(self):
        cfg = RunnerConfig(
            servers="node037",
            jobs_per_server=1,
            require_idle_cpus=0,
            launch_interval=0,
        )
        with patch(
            "ci.runner.start",
            side_effect=lambda job, node, cfg, attempt: start(
                job, "local", cfg, attempt
            ),
        ) as launch:
            results = run_jobs(
                [self.job(n, "pass") for n in ("late", "ready")], cfg, 1
            )
        self.assertEqual(launch.call_count, 2)
        now = [0]

        def advance(seconds):
            now[0] += seconds

        def state(path):
            return (
                "complete"
                if path.parent.name == "ready" or now[0] >= 11
                else "missing"
            )

        with patch(
            "ci.run_perf_ci.time.monotonic", side_effect=lambda: now[0]
        ), patch("ci.run_perf_ci.time.sleep", side_effect=advance), patch(
            "ci.run_perf_ci.stats_probe.stats_state", side_effect=state
        ) as read, patch(
            "ci.run_perf_ci.worker_stats"
        ) as ssh:
            self.assertEqual(
                check_archive(results, "stats.txt", cfg),
                ({"ready", "late"}, {}),
            )
        self.assertEqual(now[0], 11)
        self.assertEqual(
            sum(
                call.args[0].parent.name == "ready"
                for call in read.call_args_list
            ),
            1,
        )
        ssh.assert_not_called()

    def test_diagnose_only_unavailable_stats_once_per_worker(self):
        results = [
            Result(self.job(n, "pass"), "completed", 0, "node037", 0, 1)
            for n in ("a", "b", "c")
        ]
        with patch(
            "ci.run_perf_ci.worker_stats",
            return_value=["complete", "incomplete", "missing"],
        ) as ssh:
            ready, issues = check_archive(
                results, "m5out/stats.txt", RunnerConfig(), timeout=0
            )
        self.assertFalse(ready)
        ssh.assert_called_once()
        self.assertEqual(
            [v.split(", ")[-1] for v in issues.values()],
            [
                "worker stats complete",
                "worker stats incomplete",
                "worker stats missing",
            ],
        )

    def test_worker_probe_uses_same_footer_check_and_handles_disconnect(self):
        good = self.root / "path with spaces"
        good.write_text("---------- End Simulation Statistics ----------\n")
        paths = [good, self.root / "missing"]
        with patch(
            "ci.hosts.build_ssh_command",
            side_effect=lambda target, cmd, *args: cmd,
        ):
            self.assertEqual(
                worker_stats("node037", paths, RunnerConfig()),
                ["complete", "missing"],
            )
        with patch(
            "ci.run_perf_ci.subprocess.run",
            side_effect=subprocess.TimeoutExpired("ssh", 10),
        ):
            self.assertEqual(
                worker_stats("node037", paths, RunnerConfig()),
                ["unconfirmed"] * 2,
            )

    def test_stop_if_process_exits_during_escalation(self):
        proc = Mock(stdin=None, pid=123, poll=lambda: None)
        proc.wait.side_effect = [subprocess.TimeoutExpired("gem5", 3), 0]
        with patch(
            "ci.runner.os.killpg", side_effect=[None, ProcessLookupError]
        ):
            stop(proc)
        self.assertEqual(proc.wait.call_count, 2)

    def test_perf_builds_only_active_checkpoint_environment(self):
        config = self.root / "kmhv3.py"
        config.touch()
        ref = self.root / "ref.so"
        ref.touch()
        args = Namespace(
            config_path=config.name,
            build_type="fast",
            gcbv_ref_so="",
            extra_args="",
        )
        with patch.dict(
            os.environ, dict.fromkeys(CHECKPOINT_ENV, "stale")
        ), patch(
            "ci.run_perf_ci.subprocess.check_output", return_value=str(ref)
        ):
            for profile, extra, active in [
                ("gcc12-spec06-1.0c", "", {"GCBV_REF_SO": str(ref)}),
                ("h-spec06-0.5c", "", {"GCBH_REF_SO": str(ref)}),
                (
                    "gcc12-spec06-1.0c",
                    "--smt",
                    {
                        "GCBV_MULTI_CORE_REF_SO": str(ref),
                        "GCB_MULTI_CORE_RESTORER": "",
                    },
                ),
                (
                    "gcc12-spec06-1.0c",
                    "--num-cpus=2",
                    {
                        "GCBV_MULTI_CORE_REF_SO": str(ref),
                        "GCB_MULTI_CORE_RESTORER": "",
                    },
                ),
            ]:
                args.extra_args = extra
                _, _, env, _ = prepare_command(
                    self.root, MODULE.resolve_benchmark(profile), args
                )
                self.assertEqual(
                    {k: env[k] for k in CHECKPOINT_ENV if k in env}, active
                )

    def test_local_and_ssh_commands_clear_inactive_environment(self):
        with patch.dict(
            os.environ, dict.fromkeys(CHECKPOINT_ENV, "stale")
        ), patch(
            "ci.hosts.build_ssh_command",
            side_effect=lambda target, cmd, *args: cmd,
        ):
            for node in ("local", "node037"):
                job = replace(
                    self.job(
                        node,
                        "import os; assert 'GCBV_REF_SO' not in os.environ; "
                        "assert os.environ['GCBH_REF_SO'] == 'selected'",
                    ),
                    env={"GCBH_REF_SO": "selected"},
                )
                proc = start(job, node, RunnerConfig(), 0)
                try:
                    self.assertEqual(proc.wait(timeout=10), 0)
                finally:
                    stop(proc)


class ArchiveTest(unittest.TestCase):
    def test_statistics_must_have_finished_dump(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "stats.txt"
            self.assertEqual(stats_state(path), "missing")
            path.write_text("Begin Simulation Statistics")
            self.assertEqual(stats_state(path), "incomplete")
            path.write_text(
                "---------- End Simulation Statistics   ----------\n"
            )
            self.assertEqual(stats_state(path), "complete")
            with path.open("a") as out:
                out.write(
                    "---------- Begin Simulation Statistics ----------\n"
                )
            self.assertEqual(stats_state(path), "incomplete")

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

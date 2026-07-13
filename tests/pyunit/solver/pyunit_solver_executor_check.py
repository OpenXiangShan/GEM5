import argparse
from pathlib import Path
import subprocess
from types import SimpleNamespace
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import util.xs_scripts.distributed_sim as dist
from util.solver.executor.ci_local import CiLocalParallelExecutor
from util.solver.executor.distributed import (
    DistributedExecutionConfig,
    DistributedWorkloadJob,
    DistributedWorkloadResult,
    DistributedWorkloadScheduler,
    resolve_jobs_per_server,
    resolve_require_idle_cpus,
    resolve_server_names,
)
from util.solver.run_solver import apply_runtime_overrides, runtime_messages
from util.solver.types import ObjectiveSpec, ParsedProblem, StopSpec, TrialRequest


def make_problem() -> ParsedProblem:
    return ParsedProblem(
        name="Example",
        problem_ref="configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch",
        config_path="configs/example/idealkmhv3.py",
        benchmark_type="gcc15-spec06-0.3c",
        specific_benchmarks="",
        custom_bin="",
        extra_args="",
        parameters=[],
        objective=ObjectiveSpec(source_kind="stats", metric="system.cpu.ipc"),
        stop=StopSpec(max_trials=2),
    )


class SolverRuntimeOverrideTestCase(unittest.TestCase):
    def test_apply_runtime_overrides_enables_custom_bin_mode(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=8,
            config_path="",
            benchmark_type="custom_bin",
            specific_benchmarks="",
            custom_bin="/tmp/demo.bin",
            extra_args="--maxinsts=1000",
        )

        updated = apply_runtime_overrides(problem, args)

        self.assertEqual(updated.stop.max_trials, 8)
        self.assertEqual(updated.benchmark_type, "custom_bin")
        self.assertEqual(updated.custom_bin, "/tmp/demo.bin")
        self.assertEqual(updated.specific_benchmarks, "")
        self.assertIn("--maxinsts=1000", updated.extra_args)
        self.assertTrue(runtime_messages(updated))

    def test_apply_runtime_overrides_rejects_filters_in_custom_bin_mode(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            config_path="",
            benchmark_type="custom_bin",
            specific_benchmarks="mcf",
            custom_bin="/tmp/demo.bin",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "specific-benchmarks"):
            apply_runtime_overrides(problem, args)

    def test_apply_runtime_overrides_rejects_missing_custom_bin_in_custom_mode(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            config_path="",
            benchmark_type="custom_bin",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "requires a non-empty custom_bin"):
            apply_runtime_overrides(problem, args)

    def test_apply_runtime_overrides_ignores_custom_bin_outside_custom_mode(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            config_path="",
            benchmark_type="spec17-1.0c",
            specific_benchmarks="mcf",
            custom_bin="/tmp/demo.bin",
            extra_args="",
        )

        updated = apply_runtime_overrides(problem, args)

        self.assertEqual(updated.benchmark_type, "spec17-1.0c")
        self.assertEqual(updated.specific_benchmarks, "mcf")
        self.assertEqual(updated.custom_bin, "")
        self.assertTrue(runtime_messages(updated))
        self.assertIn("ignores custom_bin", runtime_messages(updated)[0])

    def test_apply_runtime_overrides_rejects_score_txt_with_custom_bin_mode(self):
        problem = make_problem()
        problem.objective = ObjectiveSpec(
            source_kind="score_txt",
            metric="Estimated Int score per GHz",
        )
        args = argparse.Namespace(
            max_trials=None,
            config_path="",
            benchmark_type="custom_bin",
            specific_benchmarks="",
            custom_bin="/tmp/demo.bin",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "benchmark_type=custom_bin"):
            apply_runtime_overrides(problem, args)

    def test_apply_runtime_overrides_normalizes_supported_config_basename(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            config_path="kmhv3.py",
            benchmark_type="spec17-1.0c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
        )

        updated = apply_runtime_overrides(problem, args)

        self.assertEqual(updated.config_path, "configs/example/kmhv3.py")
        self.assertIn(
            "config_path overridden at runtime: configs/example/kmhv3.py",
            runtime_messages(updated),
        )

    def test_apply_runtime_overrides_rejects_unsupported_config_path(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            config_path="configs/example/smt_idealkmhv3.py",
            benchmark_type="gcc15-spec06-0.3c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "unsupported config_path"):
            apply_runtime_overrides(problem, args)

    def test_apply_runtime_overrides_rejects_smt_benchmark_type(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            config_path="",
            benchmark_type="gcc12-spec06-smt-0.3c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "SMT benchmark_type"):
            apply_runtime_overrides(problem, args)


class CiLocalExecutorCommandTestCase(unittest.TestCase):
    def test_custom_bin_command_adds_raw_cpt(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=1,
            )
            problem = make_problem()
            problem.benchmark_type = "custom_bin"
            cmd = executor._build_gem5_command(
                problem,
                "/tmp/demo.bin",
                overlay_path="/tmp/overlay.json",
            )

        self.assertIn("--raw-cpt", cmd)
        self.assertIn("--solver-overlay=/tmp/overlay.json", cmd)

    def test_checkpoint_slice_command_keeps_checkpoint_mode(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=1,
            )
            problem = make_problem()
            cmd = executor._build_gem5_command(
                problem,
                "/tmp/demo.zstd",
                overlay_path="/tmp/overlay.json",
            )

        self.assertNotIn("--raw-cpt", cmd)

    def test_score_failure_is_attached_to_trial_instead_of_raising(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            checkpoint = Path(tmpdir) / "demo.zstd"
            checkpoint.write_text("", encoding="utf-8")
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=1,
            )
            problem = make_problem()
            problem.objective = ObjectiveSpec(
                source_kind="score_txt",
                metric="Estimated Int score per GHz",
            )
            problem.specific_benchmarks = "demo"
            trial = TrialRequest("trial_0001", 0, {"x": 1})
            with patch(
                "util.solver.executor.ci_local.iter_workload_entries",
                return_value=[["demo", "frag"]],
            ):
                with patch(
                    "util.solver.executor.ci_local.locate_checkpoint",
                    return_value=str(checkpoint),
                ):
                    with patch.object(
                        CiLocalParallelExecutor,
                        "_run_command",
                        return_value=SimpleNamespace(returncode=0),
                    ):
                        with patch.object(
                            CiLocalParallelExecutor,
                            "_maybe_generate_score",
                            return_value=({}, "score evaluator failed with return_code=1"),
                        ):
                            result = executor.run_trials(problem, [trial])[0]

        self.assertEqual(result.status, "completed")
        self.assertEqual(
            result.error,
            "score evaluator failed with return_code=1",
        )


class SolverDistributedConfigTestCase(unittest.TestCase):
    def test_default_servers_and_auto_capacity(self):
        config = DistributedExecutionConfig(servers="default")

        names = resolve_server_names(config)

        self.assertEqual(names[0], "node020")
        self.assertEqual(names[-1], "node039")
        self.assertEqual(len(names), 19)
        jobs_per_server = resolve_jobs_per_server(
            config,
            total_parallelism=8,
            server_count=3,
        )
        self.assertEqual(jobs_per_server, 3)
        self.assertEqual(
            resolve_require_idle_cpus(
                config,
                jobs_per_server=jobs_per_server,
            ),
            3,
        )

    def test_explicit_capacity_and_disabled_idle_probe(self):
        config = DistributedExecutionConfig(
            servers="node001-node002",
            jobs_per_server=4,
            require_idle_cpus=0,
        )

        self.assertEqual(resolve_server_names(config), ["node001", "node002"])
        self.assertEqual(
            resolve_jobs_per_server(
                config,
                total_parallelism=16,
                server_count=2,
            ),
            4,
        )
        self.assertEqual(
            resolve_require_idle_cpus(config, jobs_per_server=4),
            0,
        )

    def test_busy_server_capacity_is_zero(self):
        config = DistributedExecutionConfig(
            servers="node001",
            jobs_per_server=2,
            require_idle_cpus=2,
            load_probe_interval=0.01,
        )
        scheduler = DistributedWorkloadScheduler(
            config,
            total_parallelism=2,
            log=lambda message: None,
        )

        with patch(
            "util.solver.executor.distributed.dist.probe_idle_cpus",
            return_value=(1, "idle_physical_cores=1/64"),
        ):
            scheduler._refresh_server_capacity(scheduler.servers[0], force=True)

        self.assertEqual(scheduler.servers[0].capacity, 0)

    def test_idle_probe_timeout_message_is_compact(self):
        long_command = [
            "ssh",
            "ci-runner@172.28.9.101",
            "python3 -c " + "x" * 1000,
        ]
        with patch(
            "util.xs_scripts.distributed_sim.run_host_command",
            side_effect=subprocess.TimeoutExpired(cmd=long_command, timeout=10),
        ):
            idle_cpus, detail = dist.probe_idle_cpus(
                server_name="node025",
                idle_probe_mode="physical",
                idle_cpu_threshold=30.0,
                ssh_config="",
                ssh_options=[],
                ssh_user="",
                dispatch_host="ci-runner@172.28.9.101",
                timeout=10,
            )

        self.assertIsNone(idle_cpus)
        self.assertEqual(
            detail,
            (
                "idle probe to node025 (172.19.20.25) via dispatch host "
                "ci-runner@172.28.9.101 timed out after 10s"
            ),
        )
        self.assertNotIn("python3 -c", detail)

    def test_scheduler_launches_after_first_available_probe(self):
        config = DistributedExecutionConfig(
            servers="node001-node003",
            jobs_per_server=1,
            require_idle_cpus=1,
            poll_interval=0.01,
            launch_interval=0.0,
        )
        scheduler = DistributedWorkloadScheduler(
            config,
            total_parallelism=1,
            log=lambda message: None,
        )
        probe_calls = []
        launched = []

        def fake_probe(server_name, **kwargs):
            probe_calls.append(server_name)
            return 1, "idle_physical_cores=1/64"

        def fake_launch(scheduled, server):
            launched.append((scheduled.job.workload_name, server.name))

        with tempfile.TemporaryDirectory() as tmpdir:
            job = DistributedWorkloadJob(
                trial_id="trial_0001",
                workload_name="demo",
                checkpoint=Path("demo.zstd"),
                work_dir=Path(tmpdir) / "demo",
                command=["true"],
                env={},
            )
            with patch(
                "util.solver.executor.distributed.dist.probe_idle_cpus",
                side_effect=fake_probe,
            ):
                with patch.object(scheduler, "_launch", fake_launch):
                    scheduler.run([job])

        self.assertEqual(probe_calls, ["node001"])
        self.assertEqual(launched, [("demo", "node001")])


class CiLocalExecutorParallelismTestCase(unittest.TestCase):
    def test_standard_mode_runs_trials_concurrently(self):
        state = {
            "active": 0,
            "max_active": 0,
            "commands": [],
        }
        lock = threading.Lock()

        def fake_run(*args, **kwargs):
            with lock:
                state["active"] += 1
                state["max_active"] = max(state["max_active"], state["active"])
                state["commands"].append(args[0])
            time.sleep(0.05)
            with lock:
                state["active"] -= 1
            return SimpleNamespace(returncode=0)

        with tempfile.TemporaryDirectory() as tmpdir:
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=2,
                max_parallel_workloads=1,
            )
            problem = make_problem()
            problem.specific_benchmarks = "mcf"
            trials = [
                TrialRequest("trial_0001", 0, {"x": 1}),
                TrialRequest("trial_0002", 0, {"x": 2}),
            ]
            with patch(
                "util.solver.executor.ci_local.iter_workload_entries",
                return_value=[["demo", "frag"]],
            ):
                with patch(
                    "util.solver.executor.ci_local.locate_checkpoint",
                    return_value=str(Path(tmpdir) / "demo.zstd"),
                ):
                    with patch.object(
                        CiLocalParallelExecutor,
                        "_run_command",
                        side_effect=fake_run,
                    ):
                        with patch.object(
                            CiLocalParallelExecutor,
                            "_maybe_generate_score",
                            return_value=({}, None),
                        ):
                            results = executor.run_trials(problem, trials)

        self.assertEqual([result.trial_id for result in results], ["trial_0001", "trial_0002"])
        self.assertGreaterEqual(state["max_active"], 2)
        self.assertTrue(all(command[0].endswith("gem5.fast") for command in state["commands"]))

    def test_standard_mode_runs_multiple_workloads_concurrently(self):
        state = {
            "active": 0,
            "max_active": 0,
        }
        lock = threading.Lock()

        def fake_run(*args, **kwargs):
            with lock:
                state["active"] += 1
                state["max_active"] = max(state["max_active"], state["active"])
            time.sleep(0.05)
            with lock:
                state["active"] -= 1
            return SimpleNamespace(returncode=0)

        with tempfile.TemporaryDirectory() as tmpdir:
            checkpoint_a = Path(tmpdir) / "a.zstd"
            checkpoint_b = Path(tmpdir) / "b.zstd"
            checkpoint_a.write_text("", encoding="utf-8")
            checkpoint_b.write_text("", encoding="utf-8")
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=1,
                max_parallel_workloads=2,
            )
            problem = make_problem()
            trials = [TrialRequest("trial_0001", 0, {"x": 1})]
            with patch(
                "util.solver.executor.ci_local.iter_workload_entries",
                return_value=[["a", "frag_a"], ["b", "frag_b"]],
            ):
                with patch(
                    "util.solver.executor.ci_local.locate_checkpoint",
                    side_effect=[str(checkpoint_a), str(checkpoint_b)],
                ):
                    with patch.object(
                        CiLocalParallelExecutor,
                        "_run_command",
                        side_effect=fake_run,
                    ):
                        with patch.object(
                            CiLocalParallelExecutor,
                            "_maybe_generate_score",
                            return_value=({}, None),
                        ):
                            results = executor.run_trials(problem, trials)

        self.assertEqual(results[0].status, "completed")
        self.assertGreaterEqual(state["max_active"], 2)

    def test_custom_mode_runs_multiple_workloads_concurrently(self):
        state = {
            "active": 0,
            "max_active": 0,
        }
        lock = threading.Lock()

        def fake_run(*args, **kwargs):
            with lock:
                state["active"] += 1
                state["max_active"] = max(state["max_active"], state["active"])
            time.sleep(0.05)
            with lock:
                state["active"] -= 1
            return SimpleNamespace(returncode=0)

        with tempfile.TemporaryDirectory() as tmpdir:
            bin_a = Path(tmpdir) / "a.bin"
            bin_b = Path(tmpdir) / "b.bin"
            bin_a.write_text("", encoding="utf-8")
            bin_b.write_text("", encoding="utf-8")
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=1,
                max_parallel_workloads=2,
            )
            problem = make_problem()
            problem.benchmark_type = "custom_bin"
            problem.custom_bin = f"{bin_a},{bin_b}"
            trials = [TrialRequest("trial_0001", 0, {"x": 1})]
            with patch.object(
                CiLocalParallelExecutor,
                "_run_command",
                side_effect=fake_run,
            ):
                with patch.object(
                    CiLocalParallelExecutor,
                    "_maybe_generate_score",
                    return_value=({}, None),
                ):
                    results = executor.run_trials(problem, trials)

        self.assertEqual(results[0].status, "completed")
        self.assertGreaterEqual(state["max_active"], 2)

    def test_distributed_mode_builds_workload_jobs_with_overlay(self):
        captured = {}

        class FakeScheduler:
            def __init__(self, config, **kwargs):
                captured["config"] = config
                captured["total_parallelism"] = kwargs["total_parallelism"]

            def describe(self):
                return {"mode": "distributed"}

            def run(self, jobs, *, deadline=None):
                captured["jobs"] = jobs
                captured["deadline"] = deadline
                results = []
                for job in jobs:
                    job.work_dir.mkdir(parents=True, exist_ok=True)
                    (job.work_dir / "completed").touch()
                    results.append(
                        DistributedWorkloadResult(
                            trial_id=job.trial_id,
                            workload_name=job.workload_name,
                            checkpoint=job.checkpoint,
                            status="completed",
                            return_code=0,
                            server_name="node020",
                            detail="mocked",
                            started_at=1.0,
                            finished_at=3.0,
                        )
                    )
                return results

        with tempfile.TemporaryDirectory() as tmpdir:
            checkpoint = Path(tmpdir) / "demo.zstd"
            checkpoint.write_text("", encoding="utf-8")
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=2,
                max_parallel_workloads=1,
                distributed_config=DistributedExecutionConfig(
                    servers="node020",
                    jobs_per_server=1,
                    require_idle_cpus=0,
                ),
            )
            problem = make_problem()
            trials = [TrialRequest("trial_0001", 0, {"x": 1})]
            with patch(
                "util.solver.executor.ci_local.iter_workload_entries",
                return_value=[["demo", "frag"]],
            ):
                with patch(
                    "util.solver.executor.ci_local.locate_checkpoint",
                    return_value=str(checkpoint),
                ):
                    with patch(
                        "util.solver.executor.ci_local.DistributedWorkloadScheduler",
                        FakeScheduler,
                    ):
                        with patch.object(
                            CiLocalParallelExecutor,
                            "_maybe_generate_score",
                            return_value=({}, None),
                        ):
                            results = executor.run_trials(problem, trials)

        self.assertEqual(results[0].status, "completed")
        self.assertEqual(captured["total_parallelism"], 2)
        self.assertEqual(len(captured["jobs"]), 1)
        job = captured["jobs"][0]
        self.assertEqual(job.trial_id, "trial_0001")
        self.assertEqual(job.workload_name, "demo")
        self.assertTrue(any(part.startswith("--solver-overlay=") for part in job.command))
        self.assertTrue(str(job.work_dir).endswith("raw/spec_all/demo"))

    def test_distributed_mode_uses_per_trial_workload_duration(self):
        class FakeScheduler:
            def __init__(self, config, **kwargs):
                pass

            def describe(self):
                return {"mode": "distributed"}

            def run(self, jobs, *, deadline=None):
                results = []
                windows = {
                    "trial_0001": (10.0, 25.0),
                    "trial_0002": (100.0, 104.0),
                }
                for job in jobs:
                    job.work_dir.mkdir(parents=True, exist_ok=True)
                    (job.work_dir / "completed").touch()
                    started_at, finished_at = windows[job.trial_id]
                    results.append(
                        DistributedWorkloadResult(
                            trial_id=job.trial_id,
                            workload_name=job.workload_name,
                            checkpoint=job.checkpoint,
                            status="completed",
                            return_code=0,
                            server_name="node020",
                            detail="mocked",
                            started_at=started_at,
                            finished_at=finished_at,
                        )
                    )
                return results

        with tempfile.TemporaryDirectory() as tmpdir:
            checkpoint = Path(tmpdir) / "demo.zstd"
            checkpoint.write_text("", encoding="utf-8")
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=2,
                max_parallel_workloads=1,
                distributed_config=DistributedExecutionConfig(
                    servers="node020",
                    jobs_per_server=1,
                    require_idle_cpus=0,
                ),
            )
            problem = make_problem()
            trials = [
                TrialRequest("trial_0001", 0, {"x": 1}),
                TrialRequest("trial_0002", 0, {"x": 2}),
            ]
            with patch(
                "util.solver.executor.ci_local.iter_workload_entries",
                return_value=[["demo", "frag"]],
            ):
                with patch(
                    "util.solver.executor.ci_local.locate_checkpoint",
                    return_value=str(checkpoint),
                ):
                    with patch(
                        "util.solver.executor.ci_local.DistributedWorkloadScheduler",
                        FakeScheduler,
                    ):
                        with patch.object(
                            CiLocalParallelExecutor,
                            "_maybe_generate_score",
                            return_value=({}, None),
                        ):
                            results = executor.run_trials(problem, trials)

        self.assertEqual(
            {result.trial_id: result.duration_sec for result in results},
            {"trial_0001": 15.0, "trial_0002": 4.0},
        )


if __name__ == "__main__":
    unittest.main()

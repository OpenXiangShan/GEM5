import argparse
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from util.solver.executor.ci_local import CiLocalParallelExecutor
from util.solver.run_solver import apply_runtime_overrides
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
    def test_apply_runtime_overrides_sets_benchmark_and_custom_bin(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=8,
            benchmark_type="spec17-1.0c",
            specific_benchmarks="",
            custom_bin="/tmp/demo.bin",
            extra_args="--maxinsts=1000",
        )

        updated = apply_runtime_overrides(problem, args)

        self.assertEqual(updated.stop.max_trials, 8)
        self.assertEqual(updated.benchmark_type, "spec17-1.0c")
        self.assertEqual(updated.custom_bin, "/tmp/demo.bin")
        self.assertEqual(updated.specific_benchmarks, "")
        self.assertIn("--maxinsts=1000", updated.extra_args)

    def test_apply_runtime_overrides_rejects_filter_and_custom_bin(self):
        problem = make_problem()
        args = argparse.Namespace(
            max_trials=None,
            benchmark_type="",
            specific_benchmarks="mcf",
            custom_bin="/tmp/demo.bin",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "custom-bin"):
            apply_runtime_overrides(problem, args)

    def test_apply_runtime_overrides_rejects_score_txt_with_custom_bin(self):
        problem = make_problem()
        problem.objective = ObjectiveSpec(
            source_kind="score_txt",
            metric="Estimated Int score per GHz",
        )
        args = argparse.Namespace(
            max_trials=None,
            benchmark_type="",
            specific_benchmarks="",
            custom_bin="/tmp/demo.bin",
            extra_args="",
        )

        with self.assertRaisesRegex(ValueError, "score_txt objective does not support custom_bin"):
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


if __name__ == "__main__":
    unittest.main()

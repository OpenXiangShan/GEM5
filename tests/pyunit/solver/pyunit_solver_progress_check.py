import io
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from util.solver.executor.ci_local import CiLocalParallelExecutor
from util.solver.run_solver import ProgressReporter, format_best_trial
from util.solver.types import (
    EvaluatedTrial,
    ObjectiveSpec,
    ParsedProblem,
    StopSpec,
    TrialRequest,
)


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
        stop=StopSpec(max_trials=4),
    )


class ProgressReporterTestCase(unittest.TestCase):
    def test_reporter_prints_phase_and_batch_progress(self):
        reporter = ProgressReporter(label="solver")
        trials = [
            TrialRequest("trial_0001", 0, {"x": 1}),
            TrialRequest("trial_0002", 0, {"x": 2}),
        ]
        valid = EvaluatedTrial(
            trial_id="trial_0001",
            generation=0,
            assignments={"x": 1},
            status="valid",
            objective_value=1.25,
            metrics={},
            invalid_reason=None,
            outdir="/tmp/trial_0001",
            duration_sec=12.0,
        )
        invalid = EvaluatedTrial(
            trial_id="trial_0002",
            generation=0,
            assignments={"x": 2},
            status="invalid",
            objective_value=None,
            metrics={},
            invalid_reason="timeout",
            outdir="/tmp/trial_0002",
            duration_sec=18.0,
        )

        with io.StringIO() as buffer, redirect_stdout(buffer):
            reporter.phase("setup", "problem=Example")
            reporter.batch_started(1, trials, completed_trials=0, max_trials=4)
            reporter.batch_completed(
                1,
                [valid, invalid],
                [valid, invalid],
                valid,
                batch_duration_sec=30.0,
            )
            output = buffer.getvalue()

        self.assertEqual(format_best_trial(valid), "trial_0001=1.250000")
        self.assertEqual(format_best_trial(None), "none")
        self.assertIn("setup: problem=Example", output)
        self.assertIn("batch 1 start", output)
        self.assertIn("completed=0/4", output)
        self.assertIn("best=trial_0001=1.250000", output)
        self.assertIn("trial result: trial_0002, status=invalid", output)
        self.assertIn("reason=timeout", output)


class ExecutorProgressLoggingTestCase(unittest.TestCase):
    def test_executor_logs_trial_and_workload_progress(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            checkpoint = Path(tmpdir) / "demo.zstd"
            checkpoint.write_text("", encoding="utf-8")
            executor = CiLocalParallelExecutor(
                workdir=tmpdir,
                build_type="fast",
                max_parallel_trials=1,
                max_parallel_workloads=1,
            )
            problem = make_problem()
            problem.specific_benchmarks = "mcf"
            trials = [TrialRequest("trial_0001", 0, {"x": 1})]

            with io.StringIO() as buffer, redirect_stdout(buffer):
                with patch(
                    "util.solver.executor.ci_local.iter_workload_entries",
                    return_value=[["demo", "frag"]],
                ):
                    with patch(
                        "util.solver.executor.ci_local.locate_checkpoint",
                        return_value=str(checkpoint),
                    ):
                        with patch(
                            "util.solver.executor.ci_local.subprocess.run",
                            return_value=SimpleNamespace(returncode=0),
                        ):
                            with patch.object(
                                CiLocalParallelExecutor,
                                "_maybe_generate_score",
                                return_value=None,
                            ):
                                executor.run_trials(problem, trials)
                output = buffer.getvalue()

        self.assertIn("executing batch of 1 trial(s)", output)
        self.assertIn("starting trial_0001", output)
        self.assertIn("trial_0001: launching 1 workload(s)", output)
        self.assertIn("trial_0001: start workload demo", output)
        self.assertIn("trial_0001: workload demo completed", output)
        self.assertIn("completed trial_0001: status=completed", output)


if __name__ == "__main__":
    unittest.main()

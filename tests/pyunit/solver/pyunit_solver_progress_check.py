import io
from contextlib import redirect_stdout
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from util.solver.executor.ci_local import CiLocalParallelExecutor
from util.solver.run_solver import (
    ProgressReporter,
    finalize_run,
    format_best_trial,
)
from util.solver import run_solver as run_solver_module
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
                        with patch.object(
                            CiLocalParallelExecutor,
                            "_run_command",
                            return_value=SimpleNamespace(returncode=0),
                        ):
                            with patch.object(
                                CiLocalParallelExecutor,
                                "_maybe_generate_score",
                                return_value=({}, None),
                            ):
                                executor.run_trials(problem, trials)
                output = buffer.getvalue()

        self.assertIn("executing batch of 1 trial(s)", output)
        self.assertIn("starting trial_0001", output)
        self.assertIn("trial_0001: launching 1 workload(s)", output)
        self.assertIn("trial_0001: start workload demo", output)
        self.assertIn("trial_0001: workload demo completed", output)
        self.assertIn("completed trial_0001: status=completed", output)


class FinalizeRunTestCase(unittest.TestCase):
    def test_finalize_run_writes_partial_summary_artifacts(self):
        problem = make_problem()
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=1.25,
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=12.0,
            ),
        ]
        metadata = {
            "problem_ref": problem.problem_ref,
            "resolved_problem_ref": problem.problem_ref,
            "solver_kind": "ga",
            "solver_backend": "GaSolver",
            "solver_report": {},
            "gem5_build_type": "fast",
            "benchmark_type": problem.benchmark_type,
            "specific_benchmarks": "",
            "custom_bin": "",
            "extra_args": "",
            "dry_run": False,
            "stop_reason": "cancelled by SIGINT",
            "partial_summary": True,
            "cancel_signal": "SIGINT",
        }

        class DummySolver:
            def report_metadata(self):
                return {"algorithm": "dummy"}

        with tempfile.TemporaryDirectory() as tmpdir:
            workdir = Path(tmpdir)
            (workdir / "binding.json").write_text("{}", encoding="utf-8")
            (workdir / "parsed_problem.json").write_text("{}", encoding="utf-8")
            reporter = ProgressReporter(label="solver")
            with patch("util.solver.run_solver.publish_step_summary") as publish_mock:
                best = finalize_run(
                    workdir=workdir,
                    problem=problem,
                    history=history,
                    solver=DummySolver(),
                    metadata=metadata,
                    progress=reporter,
                )

            self.assertEqual(best.trial_id, "trial_0001")
            summary = (workdir / "summary.md").read_text(encoding="utf-8")
            self.assertIn("cancelled by SIGINT", summary)
            self.assertTrue((workdir / "artifact_manifest.json").is_file())
            self.assertTrue((workdir / "history.jsonl").is_file())
            self.assertTrue((workdir / "charts" / "best_objective.svg").is_file())
            publish_mock.assert_called_once()

    def test_main_finalizes_partial_results_after_cancel(self):
        problem = make_problem()
        problem.stop.max_trials = 4

        class FakeSolver:
            def __init__(self):
                self.calls = 0

            def propose(self, history, batch_size):
                if self.calls == 0:
                    self.calls += 1
                    return [TrialRequest("trial_0001", 0, {"x": 1})]
                if self.calls == 1:
                    self.calls += 1
                    return [TrialRequest("trial_0002", 1, {"x": 2})]
                return []

            def report_metadata(self):
                return {"algorithm": "fake", "calls": self.calls}

        class FakeExecutor:
            instances = []

            def __init__(self, **_kwargs):
                self.cancelled = False
                self.run_calls = 0
                FakeExecutor.instances.append(self)

            def cancel(self):
                self.cancelled = True

            def cleanup(self):
                return None

            def run_trials(self, _problem, trials):
                self.run_calls += 1
                if self.run_calls == 1:
                    return [
                        SimpleNamespace(
                            trial_id="trial_0001",
                            generation=0,
                            assignments={"x": 1},
                            status="completed",
                            return_code=0,
                            duration_sec=1.0,
                            outdir=str(self.workdir / "trials" / "trial_0001"),
                            raw_files={},
                            error=None,
                        )
                    ]
                run_solver_module.signal.raise_signal(run_solver_module.signal.SIGINT)
                return []

            @property
            def workdir(self):
                return self._workdir

            @workdir.setter
            def workdir(self, value):
                self._workdir = Path(value)

        def fake_executor_factory(**kwargs):
            executor = FakeExecutor()
            executor.workdir = kwargs["workdir"]
            return executor

        def fake_evaluate(_problem, result):
            return EvaluatedTrial(
                trial_id=result.trial_id,
                generation=result.generation,
                assignments=result.assignments,
                status="valid",
                objective_value=2.5,
                metrics={},
                invalid_reason=None,
                outdir=result.outdir,
                duration_sec=result.duration_sec,
            )

        with tempfile.TemporaryDirectory() as tmpdir:
            argv = [
                "run_solver.py",
                "--problem-ref",
                problem.problem_ref,
                "--workdir",
                tmpdir,
                "--max-parallel-trials",
                "1",
            ]
            fake_solver = FakeSolver()
            preview_solver = FakeSolver()
            search_solver = FakeSolver()
            with patch.object(run_solver_module, "parse_problem", return_value=problem):
                bind_side_effect = (
                    lambda executor, parsed_problem, bind_output: (
                        Path(bind_output).write_text("{}", encoding="utf-8"),
                        parsed_problem,
                    )[1]
                )
                with patch.object(
                    run_solver_module,
                    "bind_problem_targets",
                    side_effect=bind_side_effect,
                ):
                    with patch.object(
                        run_solver_module,
                        "CiLocalParallelExecutor",
                        side_effect=fake_executor_factory,
                    ):
                        with patch.object(
                            run_solver_module,
                            "choose_solver",
                            side_effect=[preview_solver, search_solver],
                        ):
                            with patch.object(run_solver_module, "evaluate_trial", side_effect=fake_evaluate):
                                with patch.object(run_solver_module, "publish_step_summary"):
                                    with patch("sys.argv", argv):
                                        exit_code = run_solver_module.main()

            self.assertEqual(exit_code, 130)
            metadata = json.loads((Path(tmpdir) / "metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(metadata["stop_reason"], "cancelled by SIGINT")
            self.assertTrue(metadata["partial_summary"])
            summary = (Path(tmpdir) / "summary.md").read_text(encoding="utf-8")
            self.assertIn("trial_0001", summary)
            manifest = json.loads((Path(tmpdir) / "artifact_manifest.json").read_text(encoding="utf-8"))
            self.assertTrue(manifest["partial_summary"])
            self.assertTrue(FakeExecutor.instances[0].cancelled)


if __name__ == "__main__":
    unittest.main()

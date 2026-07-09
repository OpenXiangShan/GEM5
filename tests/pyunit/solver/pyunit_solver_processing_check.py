import tempfile
import unittest
from pathlib import Path

from util.solver.processing.aggregate import (
    best_trial,
    evaluate_trial,
    pareto_frontier,
)
from util.solver.solver.grid import GridSolver
from util.solver.spec import Choice, Maximize, Minimize, SolveSpec, Stop, TunableParam
from util.solver.types import (
    EvaluatedTrial,
    ObjectiveSpec,
    ParsedProblem,
    StopSpec,
    TrialExecutionResult,
)
from util.solver.parser.load_spec import parse_problem


class TinyGridSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob = TunableParam.Unsigned(domain=Choice([1, 2]))
    mode = TunableParam.Unsigned(domain=Choice([10, 20]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=4)


class TinyMultiObjectiveSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob = TunableParam.Unsigned(domain=Choice([1, 2]))
    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=2)


class GridAndProcessingTestCase(unittest.TestCase):
    def test_grid_solver_enumerates_cartesian_product(self):
        problem = parse_problem(f"{__file__}:TinyGridSearch")
        solver = GridSolver(problem)
        trials = solver.propose([], 10)
        assignments = [trial.assignments for trial in trials]
        self.assertEqual(
            assignments,
            [
                {"knob": 1, "mode": 10},
                {"knob": 1, "mode": 20},
                {"knob": 2, "mode": 10},
                {"knob": 2, "mode": 20},
            ],
        )

    def test_score_txt_objective_is_extracted(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trial_dir = Path(tmpdir) / "trial_0001"
            raw_dir = trial_dir / "raw" / "spec_all" / "demo"
            raw_dir.mkdir(parents=True)
            (trial_dir / "score.txt").write_text(
                "Estimated Int score per GHz: 22.75\n",
                encoding="utf-8",
            )
            problem = ParsedProblem(
                name="ScoreProblem",
                problem_ref="dummy.py:ScoreProblem",
                config_path="configs/example/idealkmhv3.py",
                benchmark_type="gcc15-spec06-0.3c",
                specific_benchmarks="",
                custom_bin="",
                extra_args="",
                parameters=[],
                objective=ObjectiveSpec(
                    source_kind="score_txt",
                    metric="Estimated Int score per GHz",
                ),
                stop=StopSpec(max_trials=1),
            )
            execution = TrialExecutionResult(
                trial_id="trial_0001",
                generation=0,
                assignments={},
                status="completed",
                return_code=0,
                duration_sec=1.0,
                outdir=str(trial_dir),
                raw_files={"score_txt": str(trial_dir / "score.txt")},
            )
            evaluated = evaluate_trial(problem, execution)
            self.assertEqual(evaluated.status, "valid")
            self.assertEqual(evaluated.objective_value, 22.75)

    def test_execution_error_marks_trial_invalid(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trial_dir = Path(tmpdir) / "trial_0001"
            (trial_dir / "raw" / "spec_all" / "demo").mkdir(parents=True)
            problem = ParsedProblem(
                name="ScoreProblem",
                problem_ref="dummy.py:ScoreProblem",
                config_path="configs/example/idealkmhv3.py",
                benchmark_type="gcc15-spec06-0.3c",
                specific_benchmarks="",
                custom_bin="",
                extra_args="",
                parameters=[],
                objective=ObjectiveSpec(
                    source_kind="score_txt",
                    metric="Estimated Int score per GHz",
                ),
                stop=StopSpec(max_trials=1),
            )
            execution = TrialExecutionResult(
                trial_id="trial_0001",
                generation=0,
                assignments={},
                status="completed",
                return_code=0,
                duration_sec=1.0,
                outdir=str(trial_dir),
                error="score evaluator failed with return_code=1",
            )
            evaluated = evaluate_trial(problem, execution)
            self.assertEqual(evaluated.status, "invalid")
            self.assertEqual(
                evaluated.invalid_reason,
                "score evaluator failed with return_code=1",
            )

    def test_stats_objective_min_direction_prefers_smaller_value(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trial_dir = Path(tmpdir) / "trial_0001"
            raw_dir = trial_dir / "raw" / "spec_all" / "demo"
            raw_dir.mkdir(parents=True)
            (raw_dir / "stats.txt").write_text(
                "system.cpu.branchMispredicts 12\n",
                encoding="utf-8",
            )
            problem = ParsedProblem(
                name="MinStatsProblem",
                problem_ref="dummy.py:MinStatsProblem",
                config_path="configs/example/idealkmhv3.py",
                benchmark_type="gcc15-spec06-0.3c",
                specific_benchmarks="",
                custom_bin="",
                extra_args="",
                parameters=[],
                objective=Minimize.stats("system.cpu.branchMispredicts"),
                stop=StopSpec(max_trials=1),
            )
            execution = TrialExecutionResult(
                trial_id="trial_0001",
                generation=0,
                assignments={},
                status="completed",
                return_code=0,
                duration_sec=1.0,
                outdir=str(trial_dir),
            )
            evaluated = evaluate_trial(problem, execution)
            self.assertEqual(evaluated.status, "valid")
            self.assertEqual(evaluated.objective_value, 12.0)

    def test_best_trial_prefers_smaller_value_for_min_direction(self):
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=12.0,
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0002",
                generation=1,
                assignments={"x": 2},
                status="valid",
                objective_value=4.0,
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0002",
                duration_sec=1.0,
            ),
        ]
        best = best_trial(history, direction="min")
        self.assertIsNotNone(best)
        self.assertEqual(best.trial_id, "trial_0002")

    def test_multi_objective_stats_are_extracted(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            trial_dir = Path(tmpdir) / "trial_0001"
            raw_dir = trial_dir / "raw" / "spec_all" / "demo"
            raw_dir.mkdir(parents=True)
            (raw_dir / "stats.txt").write_text(
                "\n".join(
                    [
                        "system.cpu.ipc 5.5",
                        "system.cpu.branchMispredicts 12",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            problem = parse_problem(f"{__file__}:TinyMultiObjectiveSearch")
            execution = TrialExecutionResult(
                trial_id="trial_0001",
                generation=0,
                assignments={},
                status="completed",
                return_code=0,
                duration_sec=1.0,
                outdir=str(trial_dir),
            )
            evaluated = evaluate_trial(problem, execution)
            self.assertEqual(evaluated.status, "valid")
            self.assertEqual(evaluated.objective_value, 5.5)
            self.assertEqual(
                evaluated.objective_values["max:stats:system.cpu.ipc"],
                5.5,
            )
            self.assertEqual(
                evaluated.objective_values["min:stats:system.cpu.branchMispredicts"],
                12.0,
            )

    def test_pareto_frontier_keeps_non_dominated_trials(self):
        problem = parse_problem(f"{__file__}:TinyMultiObjectiveSearch")
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=10.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 10.0,
                    "min:stats:system.cpu.branchMispredicts": 8.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0002",
                generation=1,
                assignments={"x": 2},
                status="valid",
                objective_value=9.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 9.0,
                    "min:stats:system.cpu.branchMispredicts": 5.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0002",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0003",
                generation=2,
                assignments={"x": 3},
                status="valid",
                objective_value=8.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 8.0,
                    "min:stats:system.cpu.branchMispredicts": 10.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0003",
                duration_sec=1.0,
            ),
        ]
        frontier = pareto_frontier(history, problem.objective_list())
        self.assertEqual([trial.trial_id for trial in frontier], ["trial_0001", "trial_0002"])
        best = best_trial(
            history,
            objective=problem.primary_objective(),
            objectives=problem.objective_list(),
        )
        self.assertEqual(best.trial_id, "trial_0001")


if __name__ == "__main__":
    unittest.main()

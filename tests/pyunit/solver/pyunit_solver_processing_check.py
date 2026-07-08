import tempfile
import unittest
from pathlib import Path

from util.solver.processing.aggregate import evaluate_trial
from util.solver.solver.grid import GridSolver
from util.solver.spec import Choice, Maximize, SolveSpec, Stop, TunableParam
from util.solver.types import ObjectiveSpec, ParsedProblem, StopSpec, TrialExecutionResult
from util.solver.parser.load_spec import parse_problem


class TinyGridSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob = TunableParam.Unsigned(domain=Choice([1, 2]))
    mode = TunableParam.Unsigned(domain=Choice([10, 20]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=4)


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


if __name__ == "__main__":
    unittest.main()

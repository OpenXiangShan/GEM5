import unittest

from util.solver.parser.load_spec import parse_problem
from util.solver.solver.ga import GaSolver
from util.solver.spec import Choice, Maximize, Minimize, SolveSpec, Stop, TunableParam
from util.solver.types import EvaluatedTrial


class TinyGaSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=8)
    solver_name = "ga"


class TinyGaMultiObjectiveSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=8)


class TinyGaMinSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    objective = Minimize.stats("system.cpu.branchMispredicts")
    stop = Stop(max_trials=4)


class GaSolverTestCase(unittest.TestCase):
    def test_ga_proposes_unique_trials_and_reports_progress(self):
        problem = parse_problem(f"{__file__}:TinyGaSearch")
        self.assertEqual(problem.solver_hint, "ga")
        solver = GaSolver(
            problem,
            seed=1,
            population_size=4,
            elite_count=1,
            tournament_size=2,
        )
        trials = solver.propose([], 4)
        self.assertEqual(len(trials), 4)
        seen = {tuple(sorted(trial.assignments.items())) for trial in trials}
        self.assertEqual(len(seen), 4)

        history = []
        for trial in trials:
            objective_value = float(
                trial.assignments["knob_a"] * 100 + trial.assignments["knob_b"]
            )
            history.append(
                EvaluatedTrial(
                    trial_id=trial.trial_id,
                    generation=trial.generation,
                    assignments=trial.assignments,
                    status="valid",
                    objective_value=objective_value,
                    objective_values={"max:stats:system.cpu.ipc": objective_value},
                    metrics={},
                    invalid_reason=None,
                    outdir=f"/tmp/{trial.trial_id}",
                    duration_sec=1.0,
                )
            )

        next_trials = solver.propose(history, 4)
        self.assertTrue(next_trials)
        report = solver.report_metadata()
        self.assertEqual(report["algorithm"], "Genetic Algorithm via DEAP")
        self.assertEqual(report["population_size"], 4)
        self.assertEqual(report["elite_count"], 1)
        self.assertEqual(report["tournament_size"], 2)
        self.assertGreaterEqual(report["last_selected_parent_pool"], 1)
        self.assertGreaterEqual(report["last_elite_count"], 1)
        self.assertIsNotNone(report["last_best_objective"])
        self.assertIsNotNone(report["last_mean_objective"])
        self.assertEqual(len(report["generation_history"]), 2)

    def test_ga_rejects_multi_objective_problem(self):
        problem = parse_problem(f"{__file__}:TinyGaMultiObjectiveSearch")
        with self.assertRaisesRegex(ValueError, "single-objective"):
            GaSolver(problem, seed=1)

    def test_ga_tracks_best_objective_for_min_problem(self):
        problem = parse_problem(f"{__file__}:TinyGaMinSearch")
        solver = GaSolver(problem, seed=1, population_size=4)
        trials = solver.propose([], 4)
        history = []
        for trial, objective_value in zip(trials, [9.0, 7.0, 5.0, 8.0]):
            history.append(
                EvaluatedTrial(
                    trial_id=trial.trial_id,
                    generation=trial.generation,
                    assignments=trial.assignments,
                    status="valid",
                    objective_value=objective_value,
                    objective_values={
                        "min:stats:system.cpu.branchMispredicts": objective_value
                    },
                    metrics={},
                    invalid_reason=None,
                    outdir=f"/tmp/{trial.trial_id}",
                    duration_sec=1.0,
                )
            )
        solver.propose(history, 2)
        self.assertEqual(solver.report_metadata()["last_best_objective"], 5.0)


if __name__ == "__main__":
    unittest.main()

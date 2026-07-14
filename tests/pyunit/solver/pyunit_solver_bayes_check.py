import unittest

from util.solver.parser.load_spec import parse_problem
from util.solver.solver.bayes import BayesSolver
from util.solver.spec import Choice, Maximize, Minimize, SolveSpec, Stop, TunableParam
from util.solver.types import EvaluatedTrial


class TinyBayesSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=8)
    solver_name = "bayes"


class TinyBayesVectorSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    hist_lengths = TunableParam.VectorUnsigned(
        domain=Choice([
            [0, 2, 4, 8],
            [0, 3, 6, 12],
            [0, 4, 8, 16],
        ])
    )
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=4)


class TinyBayesMultiObjectiveSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=8)


class TinyBayesMinSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    objective = Minimize.stats("system.cpu.branchMispredicts")
    stop = Stop(max_trials=4)


class TellSpy:
    def __init__(self, wrapped):
        self._wrapped = wrapped
        self.calls = []

    def tell(self, x, y, fit=True):
        self.calls.append((x, y, fit))
        return self._wrapped.tell(x, y, fit=fit)

    def __getattr__(self, name):
        return getattr(self._wrapped, name)


class BayesSolverTestCase(unittest.TestCase):
    def test_bayes_proposes_unique_trials_and_reports_progress(self):
        problem = parse_problem(f"{__file__}:TinyBayesSearch")
        self.assertEqual(problem.solver_hint, "bayes")
        solver = BayesSolver(problem, seed=1, n_initial_points=2)
        trials = solver.propose([], 3)
        self.assertEqual(len(trials), 3)
        seen = {tuple(sorted(trial.assignments.items())) for trial in trials}
        self.assertEqual(len(seen), 3)

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

        next_trials = solver.propose(history, 2)
        self.assertTrue(next_trials)
        report = solver.report_metadata()
        self.assertEqual(report["algorithm"], "Bayesian Optimization via scikit-optimize")
        self.assertEqual(report["base_estimator"], "GP")
        self.assertEqual(report["acq_func"], "LCB")
        self.assertEqual(report["n_initial_points"], 2)
        self.assertGreaterEqual(report["observed_trials"], 3)
        self.assertGreaterEqual(report["last_model_fit_size"], 3)
        self.assertIsNotNone(report["last_best_objective"])
        self.assertIsNotNone(report["last_best_transformed_objective"])
        self.assertEqual(len(report["generation_history"]), 2)

    def test_bayes_batches_new_observations_into_one_tell(self):
        problem = parse_problem(f"{__file__}:TinyBayesSearch")
        solver = BayesSolver(problem, seed=1, n_initial_points=2)
        trials = solver.propose([], 3)
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

        spy = TellSpy(solver._optimizer)
        solver._optimizer = spy
        solver.propose(history, 2)

        self.assertEqual(len(spy.calls), 1)
        points, values, fit = spy.calls[0]
        self.assertTrue(fit)
        self.assertEqual(len(points), 3)
        self.assertEqual(len(values), 3)

    def test_bayes_supports_non_scalar_choice_values(self):
        problem = parse_problem(f"{__file__}:TinyBayesVectorSearch")
        solver = BayesSolver(problem, seed=1, n_initial_points=2)
        trials = solver.propose([], 2)
        self.assertEqual(len(trials), 2)
        for trial in trials:
            self.assertIn(
                trial.assignments["hist_lengths"],
                [
                    [0, 2, 4, 8],
                    [0, 3, 6, 12],
                    [0, 4, 8, 16],
                ],
            )

    def test_bayes_tells_non_scalar_choice_values_back_to_skopt(self):
        problem = parse_problem(f"{__file__}:TinyBayesVectorSearch")
        solver = BayesSolver(problem, seed=1, n_initial_points=2)
        trials = solver.propose([], 2)
        history = []
        for index, trial in enumerate(trials):
            objective_value = float(index + 1)
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

        next_trials = solver.propose(history, 1)
        self.assertEqual(len(next_trials), 1)
        self.assertEqual(solver.report_metadata()["observed_trials"], 2)

    def test_bayes_rejects_multi_objective_problem(self):
        problem = parse_problem(f"{__file__}:TinyBayesMultiObjectiveSearch")
        with self.assertRaisesRegex(ValueError, "single-objective"):
            BayesSolver(problem, seed=1)

    def test_bayes_tracks_best_objective_for_min_problem(self):
        problem = parse_problem(f"{__file__}:TinyBayesMinSearch")
        solver = BayesSolver(problem, seed=1, n_initial_points=2)
        trials = solver.propose([], 3)
        history = []
        for trial, objective_value in zip(trials, [9.0, 7.0, 5.0]):
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
        solver.propose(history, 1)
        self.assertEqual(solver.report_metadata()["last_best_objective"], 5.0)


if __name__ == "__main__":
    unittest.main()

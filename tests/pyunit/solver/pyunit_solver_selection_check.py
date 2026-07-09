import unittest

from util.solver.parser.load_spec import parse_problem
from util.solver.run_solver import choose_solver
from util.solver.solver.ga import GaSolver
from util.solver.solver.grid import GridSolver
from util.solver.solver.nsga2 import Nsga2Solver
from util.solver.solver.random import RandomSolver
from util.solver.spec import Choice, Maximize, Minimize, SolveSpec, Stop, TunableParam


class AutoSmallSingleObjectiveSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=4)


class AutoLargeSingleObjectiveSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=4)


class AutoLargeMultiObjectiveSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=4)


class AutoHintRandomSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=4)
    solver_name = "random"


class SolverSelectionTestCase(unittest.TestCase):
    def test_auto_uses_grid_when_search_space_fits_budget(self):
        problem = parse_problem(f"{__file__}:AutoSmallSingleObjectiveSearch")
        solver = choose_solver(problem, "auto", seed=1)
        self.assertIsInstance(solver, GridSolver)

    def test_auto_uses_ga_for_large_single_objective(self):
        problem = parse_problem(f"{__file__}:AutoLargeSingleObjectiveSearch")
        solver = choose_solver(problem, "auto", seed=1)
        self.assertIsInstance(solver, GaSolver)

    def test_auto_uses_nsga2_for_large_multi_objective(self):
        problem = parse_problem(f"{__file__}:AutoLargeMultiObjectiveSearch")
        solver = choose_solver(problem, "auto", seed=1)
        self.assertIsInstance(solver, Nsga2Solver)

    def test_solver_hint_still_overrides_auto(self):
        problem = parse_problem(f"{__file__}:AutoHintRandomSearch")
        solver = choose_solver(problem, "auto", seed=1)
        self.assertIsInstance(solver, RandomSolver)


if __name__ == "__main__":
    unittest.main()

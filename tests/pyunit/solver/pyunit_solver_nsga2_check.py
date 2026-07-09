import unittest

from util.solver.parser.load_spec import parse_problem
from util.solver.solver.nsga2 import Nsga2Solver
from util.solver.spec import Choice, Maximize, Minimize, SolveSpec, Stop, TunableParam


class TinyNsga2Search(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    knob_a = TunableParam.Unsigned(domain=Choice([1, 2, 3]))
    knob_b = TunableParam.Unsigned(domain=Choice([10, 20, 30]))
    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=8)
    solver_name = "nsga2"


class Nsga2SolverTestCase(unittest.TestCase):
    def test_nsga2_proposes_unique_trials(self):
        problem = parse_problem(f"{__file__}:TinyNsga2Search")
        solver = Nsga2Solver(problem, seed=1, population_size=4)
        trials = solver.propose([], 4)
        self.assertEqual(len(trials), 4)
        seen = {tuple(sorted(trial.assignments.items())) for trial in trials}
        self.assertEqual(len(seen), 4)
        report = solver.report_metadata()
        self.assertEqual(report["algorithm"], "NSGA-II via DEAP")
        self.assertEqual(report["population_size"], 4)
        self.assertEqual(len(report["generation_history"]), 1)


if __name__ == "__main__":
    unittest.main()

import unittest

from util.solver.parser.load_spec import SpecLoadError, parse_problem
from util.solver.spec import Maximize, Minimize, SolveSpec, Stop
from util.solver.types import ObjectiveSpec


class MinStatsProblem(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    objective = Minimize.stats("system.cpu.branchMispredicts")
    stop = Stop(max_trials=1)


class InvalidMinScoreProblem(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    objective = ObjectiveSpec(
        source_kind="score_txt",
        metric="Estimated Int score per GHz",
        direction="min",
    )
    stop = Stop(max_trials=1)


class MultiObjectiveProblem(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=1)


class InvalidAggregateProblem(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    objective = Maximize.stats("system.cpu.ipc", benchmark_aggregate="median")
    stop = Stop(max_trials=1)


class BayesHintProblem(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=1)
    solver_name = "bayes"


class SolverSpecParseTestCase(unittest.TestCase):
    def test_parse_vtage_problem(self):
        problem = parse_problem(
            "configs/solver_specs/vtage_ipc_search.py:VTAGEIPCSearch"
        )
        self.assertEqual(problem.name, "VTAGEIPCSearch")
        self.assertEqual(problem.config_path, "configs/example/idealkmhv3.py")
        self.assertEqual(problem.benchmark_type, "gcc15-spec06-0.3c")
        self.assertEqual(problem.objective.source_kind, "stats")
        self.assertEqual(problem.objective.metric, "system.cpu.ipc")
        self.assertEqual(len(problem.parameters), 3)

        hist_lengths = problem.parameters[0]
        self.assertEqual(hist_lengths.declared_kind, "VectorUnsigned")
        self.assertEqual(hist_lengths.domain.cardinality(), 3)

        threshold = problem.parameters[1]
        self.assertEqual(threshold.mode, "infer")
        self.assertEqual(threshold.domain.cardinality(), 9)

        upgrade_prob = problem.parameters[2]
        self.assertEqual(upgrade_prob.domain.iter_values()[0], 0.0)
        self.assertEqual(upgrade_prob.domain.iter_values()[-1], 1.0)

    def test_parse_vtage_problem_from_spec_path(self):
        problem = parse_problem("configs/solver_specs/vtage_ipc_search.py")
        self.assertEqual(problem.name, "VTAGEIPCSearch")
        self.assertTrue(problem.problem_ref.endswith(":VTAGEIPCSearch"))

    def test_parse_vtage_problem_from_class_name(self):
        problem = parse_problem("VTAGEIPCSearch")
        self.assertEqual(problem.name, "VTAGEIPCSearch")
        self.assertTrue(problem.problem_ref.endswith(":VTAGEIPCSearch"))

    def test_parse_vtage_astar_score_problem(self):
        problem = parse_problem(
            "configs/solver_specs/vtage_astar_score_search.py:VTAGEAstarScoreSearch"
        )
        self.assertEqual(problem.name, "VTAGEAstarScoreSearch")
        self.assertEqual(problem.benchmark_type, "gcc15-spec06-0.3c")
        self.assertEqual(problem.specific_benchmarks, "astar")
        self.assertEqual(problem.objective.source_kind, "score_txt")
        self.assertEqual(problem.objective.metric, "Estimated Int score per GHz")
        self.assertEqual(problem.summary_top_n, 16)
        self.assertEqual(len(problem.parameters), 5)
        self.assertEqual(problem.parameters[0].name, "allocProbLoadL1Hit")
        self.assertEqual(problem.parameters[-1].name, "deepAllocExtraHopProb")
        for parameter in problem.parameters:
            self.assertEqual(parameter.mode, "infer")
            self.assertEqual(parameter.domain.iter_values()[0], 0.0)
            self.assertEqual(parameter.domain.iter_values()[-1], 1.0)

    def test_parse_min_stats_problem(self):
        problem = parse_problem(f"{__file__}:MinStatsProblem")
        self.assertEqual(problem.name, "MinStatsProblem")
        self.assertEqual(problem.objective.source_kind, "stats")
        self.assertEqual(problem.objective.metric, "system.cpu.branchMispredicts")
        self.assertEqual(problem.objective.direction, "min")

    def test_parse_rejects_min_score_problem(self):
        with self.assertRaisesRegex(
            SpecLoadError,
            "only supports Maximize.score_txt",
        ):
            parse_problem(f"{__file__}:InvalidMinScoreProblem")

    def test_parse_rejects_invalid_benchmark_aggregate(self):
        with self.assertRaisesRegex(
            SpecLoadError,
            "benchmark_aggregate must be 'mean' or 'geomean'",
        ):
            parse_problem(f"{__file__}:InvalidAggregateProblem")

    def test_parse_preserves_bayes_solver_hint(self):
        problem = parse_problem(f"{__file__}:BayesHintProblem")
        self.assertEqual(problem.solver_hint, "bayes")

    def test_parse_multi_objective_problem(self):
        problem = parse_problem(f"{__file__}:MultiObjectiveProblem")
        self.assertEqual(problem.name, "MultiObjectiveProblem")
        self.assertEqual(len(problem.objectives), 2)
        self.assertTrue(problem.is_multi_objective())
        self.assertEqual(problem.primary_objective().metric, "system.cpu.ipc")
        self.assertEqual(
            [objective.direction for objective in problem.objectives],
            ["max", "min"],
        )

    def test_parse_coremark_smoke_problem(self):
        problem = parse_problem(
            "configs/solver_specs/coremark_ipc_smoke.py:CoremarkIPCSmoke"
        )
        self.assertEqual(problem.name, "CoremarkIPCSmoke")
        self.assertEqual(
            problem.custom_bin,
            "/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin",
        )
        self.assertEqual(problem.objective.source_kind, "stats")
        self.assertEqual(problem.objective.metric, "system.cpu.ipc")
        self.assertEqual(problem.extra_args, "--maxinsts=1000000")
        self.assertEqual(problem.summary_top_n, 8)
        self.assertEqual(len(problem.parameters), 5)


if __name__ == "__main__":
    unittest.main()

import unittest

from util.solver.parser.load_spec import parse_problem


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
        self.assertEqual(len(problem.parameters), 2)


if __name__ == "__main__":
    unittest.main()

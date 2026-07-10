import unittest

from configs.solver_specs.tage_tablesize_numways_ga_score_search import (
    TageTableSizeNumWaysGaScoreSearch,
    _BASELINE_NUM_WAYS,
    _BASELINE_TABLE_SIZES,
    _TABLE_SIZE_TOTAL,
    _table_size_candidates,
)
from util.solver.parser.load_spec import parse_problem


class FakeParamDesc:
    def convert(self, value):
        return value


class FakeTage:
    def __init__(self):
        self.numWays = list(_BASELINE_NUM_WAYS)
        self.tableSizes = list(_BASELINE_TABLE_SIZES)
        self._params = {
            "numWays": FakeParamDesc(),
            "tableSizes": FakeParamDesc(),
        }


class FakeBranchPred:
    def __init__(self):
        self.tage = FakeTage()


class FakeCpu:
    def __init__(self):
        self.branchPred = FakeBranchPred()


class FakeSystem:
    def __init__(self):
        self.cpu = [FakeCpu()]


class FakeRoot:
    def __init__(self):
        self.system = FakeSystem()


class TageGaScoreSpecTestCase(unittest.TestCase):
    def test_table_size_candidates_preserve_sum_and_power_of_two_bounds(self):
        options = _table_size_candidates()
        self.assertEqual(len(options), 504)
        self.assertIn(_BASELINE_TABLE_SIZES, options)
        for option in options:
            self.assertEqual(len(option), 8)
            self.assertEqual(sum(option), _TABLE_SIZE_TOTAL)
            for value in option:
                self.assertGreaterEqual(value, 256)
                self.assertLessEqual(value, 8192)
                self.assertEqual(value & (value - 1), 0)

    def test_parse_tage_table_numways_ga_score_problem(self):
        problem = parse_problem(
            "configs/solver_specs/tage_tablesize_numways_ga_score_search.py:"
            "TageTableSizeNumWaysGaScoreSearch"
        )
        self.assertEqual(problem.solver_hint, "ga")
        self.assertFalse(problem.is_multi_objective())
        self.assertEqual(problem.benchmark_type, "gcc15-spec06-0.3c")
        self.assertEqual(problem.specific_benchmarks, "astar,gobmk,sjeng")
        self.assertEqual(problem.objective.source_kind, "score_txt")
        self.assertEqual(problem.objective.metric, "Estimated Int score per GHz")
        self.assertEqual(problem.stop.max_trials, 4000)
        self.assertEqual(problem.stop.timeout_hours, 30)
        self.assertEqual(problem.parameters[0].name, "tableSizes")
        self.assertEqual(problem.parameters[0].domain.cardinality(), 504)
        self.assertEqual(problem.parameters[1].name, "numWays0")
        self.assertEqual(problem.parameters[-1].name, "numWays7")

    def test_apply_trial_updates_numways_vector(self):
        root = FakeRoot()
        trial = type(
            "Trial",
            (),
            {
                "numWays0": 1,
                "numWays1": 2,
                "numWays2": 3,
                "numWays3": 4,
                "numWays4": 5,
                "numWays5": 6,
                "numWays6": 7,
                "numWays7": 8,
            },
        )()
        TageTableSizeNumWaysGaScoreSearch.apply_trial(root, trial)
        tage = root.system.cpu[0].branchPred.tage
        self.assertEqual(tage.numWays, [1, 2, 3, 4, 5, 6, 7, 8])


if __name__ == "__main__":
    unittest.main()

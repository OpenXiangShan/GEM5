import random
import unittest

from configs.solver_specs.tage_tablesize_numways_ga_score_search import (
    TageTableSizeNumWaysGaScoreSearch,
    _BASELINE_NUM_WAYS,
    _BASELINE_TABLE_SIZES,
    _BASELINE_TOTAL_SIZE,
    _NUM_TABLES,
    _NUM_WAY_VALUES,
    _TABLE_SIZE_VALUES,
    _TAGE_CONFIG_DOMAIN,
    _split_tage_config,
    _tage_total_size,
)
from util.solver.parser.load_spec import parse_problem
from util.solver.solver.ga import GaSolver


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
    def assert_valid_tage_config(self, config):
        table_sizes, num_ways = _split_tage_config(config)
        self.assertEqual(len(table_sizes), _NUM_TABLES)
        self.assertEqual(len(num_ways), _NUM_TABLES)
        self.assertEqual(_tage_total_size(table_sizes, num_ways), _BASELINE_TOTAL_SIZE)
        for value in table_sizes:
            self.assertIn(value, _TABLE_SIZE_VALUES)
        for value in num_ways:
            self.assertIn(value, _NUM_WAY_VALUES)

    def test_tage_config_domain_samples_fixed_total_size(self):
        self.assertEqual(_TAGE_CONFIG_DOMAIN.cardinality(), 75244734738)
        self.assert_valid_tage_config(_BASELINE_TABLE_SIZES + _BASELINE_NUM_WAYS)

        rng = random.Random(1)
        samples = [_TAGE_CONFIG_DOMAIN.sample(rng) for _ in range(64)]
        self.assertGreater(len({tuple(sample) for sample in samples}), 60)
        for sample in samples:
            self.assert_valid_tage_config(sample)

        mutated = _TAGE_CONFIG_DOMAIN.mutate(rng, samples[0])
        self.assert_valid_tage_config(mutated)
        child_a, child_b = _TAGE_CONFIG_DOMAIN.crossover(rng, samples[0], samples[1])
        self.assert_valid_tage_config(child_a)
        self.assert_valid_tage_config(child_b)

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
        self.assertEqual(len(problem.parameters), 1)
        self.assertEqual(problem.parameters[0].name, "tableSizesNumWays")
        self.assertEqual(problem.parameters[0].domain.cardinality(), 75244734738)
        self.assertEqual(
            problem.parameters[0].default,
            _BASELINE_TABLE_SIZES + _BASELINE_NUM_WAYS,
        )

    def test_ga_proposes_only_fixed_total_tage_configs(self):
        problem = parse_problem(
            "configs/solver_specs/tage_tablesize_numways_ga_score_search.py:"
            "TageTableSizeNumWaysGaScoreSearch"
        )
        solver = GaSolver(problem, seed=1, population_size=4)
        trials = solver.propose([], 8)
        self.assertEqual(len(trials), 8)
        for trial in trials:
            self.assert_valid_tage_config(trial.assignments["tableSizesNumWays"])

    def test_apply_trial_updates_table_sizes_and_numways_vectors(self):
        root = FakeRoot()
        table_sizes = [4096, 4096, 4096, 8192, 8192, 8192, 8192, 4096]
        num_ways = [1, 1, 8, 2, 2, 2, 2, 1]
        trial = type(
            "Trial",
            (),
            {"tableSizesNumWays": table_sizes + num_ways},
        )()
        TageTableSizeNumWaysGaScoreSearch.apply_trial(root, trial)
        tage = root.system.cpu[0].branchPred.tage
        self.assertEqual(tage.tableSizes, table_sizes)
        self.assertEqual(tage.numWays, num_ways)


if __name__ == "__main__":
    unittest.main()

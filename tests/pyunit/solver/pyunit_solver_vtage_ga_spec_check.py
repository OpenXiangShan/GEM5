import unittest

from configs.solver_specs.vtage_astar_ga_ipc_search import VTAGEAstarGaIPCSearch
from util.solver.parser.load_spec import parse_problem


class FakeParamDesc:
    def convert(self, value):
        return value


class FakePredictor:
    def __init__(self):
        self.predictConfThreshold = 0
        self.hashOnlyUpgradeThreshold = 0
        self._params = {
            "predictConfThreshold": FakeParamDesc(),
            "hashOnlyUpgradeThreshold": FakeParamDesc(),
        }


class FakeValuePred:
    def __init__(self):
        self.predictors = [object(), FakePredictor()]


class FakeCpu:
    def __init__(self):
        self.valuePred = FakeValuePred()


class FakeSystem:
    def __init__(self):
        self.cpu = [FakeCpu()]


class FakeRoot:
    def __init__(self):
        self.system = FakeSystem()


class VTAGEGaSpecTestCase(unittest.TestCase):
    def test_parse_vtage_astar_ga_problem(self):
        problem = parse_problem(
            "configs/solver_specs/vtage_astar_ga_ipc_search.py:VTAGEAstarGaIPCSearch"
        )
        self.assertEqual(problem.solver_hint, "ga")
        self.assertFalse(problem.is_multi_objective())
        self.assertEqual(problem.objective.metric, "system.cpu.ipc")
        self.assertEqual(problem.stop.max_trials, 4000)
        self.assertTrue(problem.custom_bin.endswith(".zstd"))
        self.assertEqual(problem.parameters[0].domain.iter_values()[0], 7)
        self.assertEqual(problem.parameters[0].domain.iter_values()[-1], 13)

    def test_apply_trial_derives_thresholds_from_confbits(self):
        root = FakeRoot()
        trial = type("Trial", (), {"confBits": 11})()
        VTAGEAstarGaIPCSearch.apply_trial(root, trial)
        predictor = root.system.cpu[0].valuePred.predictors[1]
        self.assertEqual(predictor.predictConfThreshold, 1024)
        self.assertEqual(predictor.hashOnlyUpgradeThreshold, 1023)


if __name__ == "__main__":
    unittest.main()

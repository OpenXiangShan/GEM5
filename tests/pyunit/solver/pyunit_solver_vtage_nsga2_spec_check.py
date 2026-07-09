import unittest

from configs.solver_specs.vtage_astar_nsga2_search import VTAGEAstarNsga2Search
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


class VTAGENsga2SpecTestCase(unittest.TestCase):
    def test_parse_vtage_astar_nsga2_problem(self):
        problem = parse_problem(
            "configs/solver_specs/vtage_astar_nsga2_search.py:VTAGEAstarNsga2Search"
        )
        self.assertEqual(problem.solver_hint, "nsga2")
        self.assertTrue(problem.is_multi_objective())
        self.assertEqual(len(problem.objectives), 4)
        self.assertEqual(problem.custom_bin.endswith(".zstd"), True)

    def test_apply_trial_derives_thresholds_from_confbits(self):
        root = FakeRoot()
        trial = type("Trial", (), {"confBits": 10})()
        VTAGEAstarNsga2Search.apply_trial(root, trial)
        predictor = root.system.cpu[0].valuePred.predictors[1]
        self.assertEqual(predictor.predictConfThreshold, 512)
        self.assertEqual(predictor.hashOnlyUpgradeThreshold, 511)


if __name__ == "__main__":
    unittest.main()

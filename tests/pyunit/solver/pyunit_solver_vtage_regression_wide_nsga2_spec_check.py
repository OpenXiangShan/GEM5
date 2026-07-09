import unittest

from configs.solver_specs.vtage_regression_wide_nsga2_search import (
    VTAGERegressionWideNsga2Search,
    _hist_length_candidates,
)
from util.solver.parser.load_spec import parse_problem


class VTAGERegressionWideNsga2SpecTestCase(unittest.TestCase):
    def test_parse_problem(self):
        problem = parse_problem(
            "configs/solver_specs/vtage_regression_wide_nsga2_search.py:"
            "VTAGERegressionWideNsga2Search"
        )
        self.assertEqual(problem.solver_hint, "nsga2")
        self.assertTrue(problem.is_multi_objective())
        self.assertEqual(len(problem.objectives), 4)
        self.assertEqual(problem.stop.max_trials, 5000)
        self.assertEqual(problem.stop.timeout_hours, 20)

    def test_custom_bin_covers_gobmk_sjeng_h264ref(self):
        custom_bin = VTAGERegressionWideNsga2Search.custom_bin.splitlines()
        self.assertEqual(len(custom_bin), 4)
        joined = "\n".join(custom_bin)
        self.assertIn("/gobmk_", joined)
        self.assertIn("/sjeng/", joined)
        self.assertIn("/h264ref_", joined)

    def test_hist_length_candidates_are_monotonic_and_bounded(self):
        options = _hist_length_candidates()
        self.assertGreaterEqual(len(options), 10)
        for option in options:
            self.assertEqual(len(option), 9)
            self.assertEqual(option[0], 0)
            self.assertEqual(option[1], 0)
            self.assertLessEqual(option[-1], 160)
            for lhs, rhs in zip(option[2:], option[3:]):
                self.assertLess(lhs, rhs)


if __name__ == "__main__":
    unittest.main()

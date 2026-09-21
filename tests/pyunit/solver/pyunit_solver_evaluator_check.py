import importlib.util
from pathlib import Path
import sys
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
EVALUATOR_PATH = REPO_ROOT / "util" / "solver" / "executor" / "evaluator.py"
SPEC = importlib.util.spec_from_file_location("solver_evaluator", EVALUATOR_PATH)
assert SPEC is not None and SPEC.loader is not None
EVALUATOR = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = EVALUATOR
SPEC.loader.exec_module(EVALUATOR)


class SolverEvaluatorTestSuite(unittest.TestCase):
    def test_spec_version_from_score_script(self):
        cases = {
            "gem5-score-ci.sh": "06",
            "gem5-score-ci-17.sh": "17",
            "gem5-score-ci-26.sh": "26",
        }

        for score_script, expected in cases.items():
            with self.subTest(score_script=score_script):
                self.assertEqual(
                    EVALUATOR._spec_version_from_score_script(score_script),
                    expected,
                )

import tempfile
import unittest

from util.solver.reporting.charts import render_charts
from util.solver.reporting.markdown import render_summary
from util.solver.types import EvaluatedTrial, ObjectiveSpec, ParsedProblem, StopSpec


class ReportingTestCase(unittest.TestCase):
    def test_render_summary_and_charts(self):
        problem = ParsedProblem(
            name="Example",
            problem_ref="example.py:Example",
            config_path="configs/example/idealkmhv3.py",
            benchmark_type="gcc15-spec06-0.3c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
            parameters=[],
            objective=ObjectiveSpec(source_kind="stats", metric="system.cpu.ipc"),
            stop=StopSpec(max_trials=2),
        )
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=1.0,
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0002",
                generation=1,
                assignments={"x": 2},
                status="invalid",
                objective_value=None,
                metrics={},
                invalid_reason="abort",
                outdir="/tmp/trial_0002",
                duration_sec=1.0,
            ),
        ]

        summary = render_summary(problem, history)
        self.assertIn("trial_0001", summary)
        self.assertIn("1 valid", summary)

        with tempfile.TemporaryDirectory() as tmpdir:
            outputs = render_charts(problem, history, tmpdir)
            self.assertEqual(len(outputs), 2)


if __name__ == "__main__":
    unittest.main()

import tempfile
import unittest
from unittest import mock

from util.solver.reporting.charts import render_charts
from util.solver.reporting.markdown import publish_step_summary, render_summary
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
            summary_top_n=16,
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

        summary = render_summary(
            problem,
            history,
            extra_sections=["## Custom Section\n\nhello"],
        )
        self.assertIn("trial_0001", summary)
        self.assertIn("1 valid", summary)
        self.assertIn("```mermaid", summary)
        self.assertIn("## Custom Section", summary)
        self.assertIn("## Top Results", summary)

        with tempfile.TemporaryDirectory() as tmpdir:
            outputs = render_charts(problem, history, tmpdir)
            self.assertEqual(len(outputs), 2)

    def test_publish_step_summary_overwrites_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            summary_path = tempfile.NamedTemporaryFile(dir=tmpdir, delete=False)
            summary_path.close()
            with open(summary_path.name, "w", encoding="utf-8") as handle:
                handle.write("old")
            with mock.patch.dict(
                "os.environ",
                {"GITHUB_STEP_SUMMARY": summary_path.name},
                clear=False,
            ):
                publish_step_summary("new summary\n")
            with open(summary_path.name, "r", encoding="utf-8") as handle:
                self.assertEqual(handle.read(), "new summary\n")


if __name__ == "__main__":
    unittest.main()

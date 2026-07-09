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

    def test_render_summary_orders_min_objective_from_small_to_large(self):
        problem = ParsedProblem(
            name="MinExample",
            problem_ref="example.py:MinExample",
            config_path="configs/example/idealkmhv3.py",
            benchmark_type="gcc15-spec06-0.3c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
            parameters=[],
            objective=ObjectiveSpec(
                source_kind="stats",
                metric="system.cpu.branchMispredicts",
                direction="min",
            ),
            stop=StopSpec(max_trials=2),
            summary_top_n=16,
        )
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=10.0,
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0002",
                generation=1,
                assignments={"x": 2},
                status="valid",
                objective_value=3.0,
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0002",
                duration_sec=1.0,
            ),
        ]

        summary = render_summary(problem, history)
        self.assertIn("trial_0002", summary)
        self.assertIn("Representative best: `trial_0002`", summary)
        self.assertLess(summary.find("| trial_0002 |"), summary.find("| trial_0001 |"))

    def test_render_multi_objective_summary_and_charts(self):
        problem = ParsedProblem(
            name="MultiExample",
            problem_ref="example.py:MultiExample",
            config_path="configs/example/idealkmhv3.py",
            benchmark_type="gcc15-spec06-0.3c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
            parameters=[],
            objective=ObjectiveSpec(source_kind="stats", metric="system.cpu.ipc"),
            objectives=[
                ObjectiveSpec(source_kind="stats", metric="system.cpu.ipc"),
                ObjectiveSpec(
                    source_kind="stats",
                    metric="system.cpu.branchMispredicts",
                    direction="min",
                ),
            ],
            stop=StopSpec(max_trials=3),
            summary_top_n=16,
        )
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=10.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 10.0,
                    "min:stats:system.cpu.branchMispredicts": 8.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0002",
                generation=1,
                assignments={"x": 2},
                status="valid",
                objective_value=9.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 9.0,
                    "min:stats:system.cpu.branchMispredicts": 5.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0002",
                duration_sec=1.0,
            ),
            EvaluatedTrial(
                trial_id="trial_0003",
                generation=2,
                assignments={"x": 3},
                status="valid",
                objective_value=8.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 8.0,
                    "min:stats:system.cpu.branchMispredicts": 10.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0003",
                duration_sec=1.0,
            ),
        ]

        summary = render_summary(problem, history)
        self.assertIn("Pareto frontier size: `2`", summary)
        self.assertIn("## Pareto Frontier", summary)
        self.assertIn("crowding_distance", summary)
        self.assertIn("non-dominated trials", summary)
        self.assertIn("contributes more diversity", summary)
        self.assertIn("trial_0001", summary)
        self.assertIn("trial_0002", summary)

        with tempfile.TemporaryDirectory() as tmpdir:
            outputs = render_charts(problem, history, tmpdir)
            self.assertEqual(len(outputs), 3)

    def test_render_summary_includes_solver_algorithm_section(self):
        problem = ParsedProblem(
            name="MultiExample",
            problem_ref="example.py:MultiExample",
            config_path="configs/example/idealkmhv3.py",
            benchmark_type="gcc15-spec06-0.3c",
            specific_benchmarks="",
            custom_bin="",
            extra_args="",
            parameters=[],
            objective=ObjectiveSpec(source_kind="stats", metric="system.cpu.ipc"),
            objectives=[
                ObjectiveSpec(source_kind="stats", metric="system.cpu.ipc"),
                ObjectiveSpec(
                    source_kind="stats",
                    metric="system.cpu.branchMispredicts",
                    direction="min",
                ),
            ],
            stop=StopSpec(max_trials=3),
            summary_top_n=16,
        )
        history = [
            EvaluatedTrial(
                trial_id="trial_0001",
                generation=0,
                assignments={"x": 1},
                status="valid",
                objective_value=10.0,
                objective_values={
                    "max:stats:system.cpu.ipc": 10.0,
                    "min:stats:system.cpu.branchMispredicts": 8.0,
                },
                metrics={},
                invalid_reason=None,
                outdir="/tmp/trial_0001",
                duration_sec=1.0,
            ),
        ]

        summary = render_summary(
            problem,
            history,
            metadata={
                "solver_kind": "nsga2",
                "solver_backend": "Nsga2Solver",
                "solver_report": {
                    "algorithm": "NSGA-II via DEAP",
                    "population_size": 8,
                    "mutation_prob": 0.3,
                    "crossover_prob": 0.9,
                    "last_frontier_size": 1,
                    "generation_history": [
                        {
                            "generation": 0,
                            "frontier_size": 1,
                            "selected_parent_pool": 0,
                            "generated_trials": 8,
                        },
                        {
                            "generation": 1,
                            "frontier_size": 2,
                            "selected_parent_pool": 8,
                            "generated_trials": 8,
                        },
                    ],
                },
            },
        )
        self.assertIn("## Solver Algorithm", summary)
        self.assertIn("NSGA-II via DEAP", summary)
        self.assertIn("population_size", summary)
        self.assertIn("last_frontier_size", summary)
        self.assertIn("candidate generator is behaving", summary)
        self.assertIn("Backend algorithm used for candidate generation", summary)
        self.assertIn("## NSGA-II Progress", summary)
        self.assertIn("Frontier Size By Generation", summary)
        self.assertIn("Parent Pool By Generation", summary)
        self.assertIn("New Samples By Generation", summary)
        self.assertIn("process health indicators", summary)

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

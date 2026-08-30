from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from util.solver.parser.load_spec import (
    SpecLoadError,
    _parse_custom_bin_weights,
    parse_problem,
)
from util.solver.processing.aggregate import evaluate_trial
from util.solver.reporting.markdown import render_summary
from util.solver.types import (
    ObjectiveSpec,
    ParsedProblem,
    StopSpec,
    TrialExecutionResult,
)


class CustomBinWeightAggregationTest(unittest.TestCase):
    def _problem(self, weights: tuple[float, ...]) -> ParsedProblem:
        objective = ObjectiveSpec(
            source_kind="stats",
            metric="system.cpu.ipc",
            direction="max",
        )
        return ParsedProblem(
            name="CustomBinWeights",
            problem_ref="test:CustomBinWeights",
            config_path="configs/example/kmhv3.py",
            benchmark_type="custom_bin",
            specific_benchmarks="",
            custom_bin="first.zstd,second.zstd",
            custom_bin_weights=weights,
            extra_args="",
            parameters=[],
            objective=objective,
            objectives=[objective],
            stop=StopSpec(max_trials=1),
        )

    def _evaluate(self, weights: tuple[float, ...]):
        temp_root = Path.home() / "temp"
        temp_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temp_root) as tmpdir:
            spec_dir = Path(tmpdir) / "raw" / "spec_all"
            for name, ipc in (("01_first", 2.0), ("02_second", 4.0)):
                workload_dir = spec_dir / name
                workload_dir.mkdir(parents=True)
                (workload_dir / "stats.txt").write_text(
                    f"system.cpu.ipc {ipc}\n", encoding="utf-8"
                )
            execution = TrialExecutionResult(
                trial_id="trial_0001",
                generation=0,
                assignments={},
                status="completed",
                return_code=0,
                duration_sec=1.0,
                outdir=tmpdir,
            )
            return evaluate_trial(self._problem(weights), execution)

    def test_weighted_custom_bin_ipc_and_metrics(self):
        trial = self._evaluate((1.0, 3.0))

        self.assertEqual(trial.status, "valid")
        self.assertAlmostEqual(trial.objective_value, 3.5)
        weighting = trial.metrics["custom_bin_weighting"]["system.cpu.ipc"]
        self.assertEqual(weighting["aggregation"], "weighted_mean")
        self.assertEqual(weighting["weights"], {"01_first": 1.0, "02_second": 3.0})
        self.assertAlmostEqual(weighting["weight_sum"], 4.0)
        self.assertAlmostEqual(weighting["weighted_numerator"], 14.0)
        summary = render_summary(
            self._problem((1.0, 3.0)),
            [trial],
        )
        self.assertIn("### Custom-Bin Weighting", summary)
        self.assertIn("| 02_second | 4.000000 | 3.000000000 | 3.000000 |", summary)

    def test_unweighted_custom_bin_remains_equal_mean(self):
        trial = self._evaluate(())

        self.assertEqual(trial.status, "valid")
        self.assertAlmostEqual(trial.objective_value, 3.0)
        self.assertNotIn("custom_bin_weighting", trial.metrics)

    def test_dse_spec_has_all_simpoint_weights(self):
        problem = parse_problem(
            "configs/solver_specs/bop_cqf_dse_nsga2.py:BOPCQFDseNsga2"
        )

        self.assertEqual(len(problem.custom_bin_weights), 32)
        self.assertAlmostEqual(sum(problem.custom_bin_weights), 2.97660699)

    def test_weight_validation_rejects_mismatched_and_invalid_values(self):
        class MismatchedWeights:
            custom_bin_weights = (1.0,)

        class NegativeWeights:
            custom_bin_weights = (1.0, -1.0)

        with self.assertRaisesRegex(SpecLoadError, "has 1 entries"):
            _parse_custom_bin_weights(
                MismatchedWeights,
                benchmark_type="custom_bin",
                custom_bin="first.zstd,second.zstd",
            )
        with self.assertRaisesRegex(SpecLoadError, "non-negative"):
            _parse_custom_bin_weights(
                NegativeWeights,
                benchmark_type="custom_bin",
                custom_bin="first.zstd,second.zstd",
            )


if __name__ == "__main__":
    unittest.main()

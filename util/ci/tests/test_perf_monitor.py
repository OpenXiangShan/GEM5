#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "perf_monitor.py"
SPEC = importlib.util.spec_from_file_location("perf_monitor", MODULE_PATH)
perf_monitor = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = perf_monitor
SPEC.loader.exec_module(perf_monitor)


def score_row(score, coverage=0.3):
    return {"score": str(score), "coverage": str(coverage)}


class PerfMonitorTest(unittest.TestCase):
    def test_metadata_compatibility_uses_performance_inputs(self):
        candidate = {
            "config_path": "configs/example/kmhv3.py",
            "benchmark_type": "gcc16-0.3c",
            "specific_benchmarks": "",
            "vector_type": "base",
            "resolved_extra_args": "--foo=1",
            "checkpoint_list": "/profiles/list",
            "cluster_config": "/profiles/weights.json",
            "commit": "candidate",
        }
        baseline = dict(candidate, commit="baseline")
        self.assertTrue(perf_monitor.metadata_compatible(candidate, baseline))
        baseline["resolved_extra_args"] = "--foo=0"
        self.assertFalse(perf_monitor.metadata_compatible(candidate, baseline))
        baseline = dict(candidate, cluster_config="/profiles/new-weights.json")
        self.assertFalse(perf_monitor.metadata_compatible(candidate, baseline))

    def test_baseline_must_be_xs_dev_push(self):
        self.assertTrue(
            perf_monitor.is_xs_dev_push(
                {"event": "push", "head_branch": "xs-dev"}
            )
        )
        self.assertFalse(
            perf_monitor.is_xs_dev_push(
                {"event": "pull_request_target", "head_branch": "xs-dev"}
            )
        )

    def test_completeness_rejects_nan_and_coverage_change(self):
        baseline = {
            "mcf": score_row(10.0, 0.3),
            "gcc": score_row(20.0, 0.4),
            "overall_avg": score_row(14.0, "nan"),
        }
        candidate = {
            "mcf": score_row("nan", 0.3),
            "gcc": score_row(20.0, 0.2),
            "overall_avg": score_row(14.0, "nan"),
        }
        completeness = perf_monitor.score_completeness(candidate, baseline)
        self.assertFalse(completeness["complete"])
        self.assertEqual(completeness["invalid_scores"], ["mcf"])
        self.assertEqual(completeness["coverage_mismatch"][0]["workload"], "gcc")

    def test_completeness_rejects_invalid_baseline(self):
        baseline = {"mcf": score_row("nan", "nan")}
        candidate = {"mcf": score_row(10.0, 0.3)}
        completeness = perf_monitor.score_completeness(candidate, baseline)
        self.assertFalse(completeness["complete"])
        self.assertEqual(completeness["baseline_invalid_scores"], ["mcf"])
        self.assertEqual(completeness["baseline_invalid_coverage"], ["mcf"])

    def test_completeness_requires_overall_score(self):
        baseline = {
            "mcf": score_row(10.0),
            "overall_avg": score_row("nan", "nan"),
        }
        candidate = {"mcf": score_row(10.0)}
        completeness = perf_monitor.score_completeness(candidate, baseline)
        self.assertFalse(completeness["complete"])
        self.assertEqual(completeness["invalid_summary_scores"], ["overall_avg"])
        self.assertEqual(
            completeness["baseline_invalid_summary_scores"], ["overall_avg"]
        )

    def test_explicit_baseline_cannot_be_candidate_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            candidate = Path(directory)
            with mock.patch.object(
                perf_monitor,
                "get_run",
                return_value={"status": "completed", "conclusion": "success"},
            ), mock.patch.object(
                perf_monitor, "locate_archive", return_value=(candidate, {})
            ):
                with self.assertRaisesRegex(
                    perf_monitor.MonitorError, "candidate archive"
                ):
                    perf_monitor.find_baseline(
                        "OpenXiangShan/GEM5", candidate, {}, "123", 10
                    )

    def test_classify_regression(self):
        run = {"conclusion": "success"}
        deltas = [
            {"workload": "overall_avg", "delta_pct": -1.2},
            {"workload": "mcf", "delta_pct": -2.0},
        ]
        severity, reasons = perf_monitor.classify(
            run,
            {"complete": True},
            deltas,
            [],
            perf_monitor.DEFAULT_POLICY,
        )
        self.assertEqual(severity, "critical")
        self.assertIn("overall score regressed", reasons[0])

    def test_skipped_source_run_detection(self):
        run = {"conclusion": "success"}
        self.assertTrue(
            perf_monitor.source_run_was_skipped(
                run, [{"conclusion": "skipped"}]
            )
        )
        self.assertFalse(
            perf_monitor.source_run_was_skipped(
                run, [{"conclusion": "failure"}]
            )
        )
        self.assertFalse(
            perf_monitor.source_run_was_skipped(
                {"conclusion": "cancelled"}, [{"conclusion": "skipped"}]
            )
        )

    def test_classify_normal(self):
        run = {"conclusion": "success"}
        deltas = [
            {"workload": "overall_avg", "delta_pct": -0.1},
            {"workload": "mcf", "delta_pct": 0.5},
        ]
        severity, reasons = perf_monitor.classify(
            run,
            {"complete": True},
            deltas,
            [],
            perf_monitor.DEFAULT_POLICY,
        )
        self.assertEqual(severity, "normal")
        self.assertEqual(reasons, [])

    def test_versioned_archive_requires_final_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory)
            self.assertFalse(
                perf_monitor.archive_finalized(
                    archive, {"archive_schema_version": "2"}
                )
            )
            (archive / "PERF_COMPLETE").touch()
            self.assertTrue(
                perf_monitor.archive_finalized(
                    archive, {"archive_schema_version": "2"}
                )
            )
            self.assertTrue(perf_monitor.archive_finalized(archive, {}))

    def test_legacy_auto_perf_profile_is_known(self):
        profile = perf_monitor.LEGACY_PROFILE_INPUTS[
            "spec06-rva23-novec-gcc16-0.3c"
        ]
        self.assertTrue(profile["cluster_config"].endswith("checkpoints_cov0.3.json"))
        self.assertTrue(profile["checkpoint_list"].endswith("spec06_0.3c.lst"))


if __name__ == "__main__":
    unittest.main()

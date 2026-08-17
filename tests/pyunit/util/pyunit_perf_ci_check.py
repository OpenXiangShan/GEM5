"""Tests for the reusable performance CI implementation."""

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
PERF_CI_PATH = REPO_ROOT / "util" / "ci" / "perf_ci.py"
PERF_CI_SPEC = importlib.util.spec_from_file_location("gem5_perf_ci", PERF_CI_PATH)
assert PERF_CI_SPEC is not None and PERF_CI_SPEC.loader is not None
perf_ci = importlib.util.module_from_spec(PERF_CI_SPEC)
sys.modules[PERF_CI_SPEC.name] = perf_ci
PERF_CI_SPEC.loader.exec_module(perf_ci)


class BenchmarkRegistryTest(unittest.TestCase):
    def test_all_supported_profiles_are_valid(self):
        supported = {
            "gcc12-spec06-0.3c",
            "gcc12-spec06-smt-0.3c",
            "gcc12-spec06-smt-1.0c",
            "gcc12-spec06-smt-int-1.0c",
            "gcc12-spec06-0.8c",
            "gcc12-spec06-1.0c",
            "gcc15-spec06-0.3c",
            "gcc15-spec06-1.0c",
            "h-spec06-0.5c",
            "h-spec06-1.0c",
            "spec17-1.0c",
            "spec06-rvv-1.0c",
            "spec06int-rvv-0.8c",
            "gcc15-spec26-0.3c",
            "gcc15-spec26-1.0c",
        }
        registry = json.loads(perf_ci.REGISTRY_PATH.read_text(encoding="utf-8"))

        self.assertEqual(set(registry), supported)

        for benchmark_type in supported:
            with self.subTest(benchmark_type=benchmark_type):
                self.assertEqual(
                    perf_ci.load_profile(benchmark_type)["benchmark_type"],
                    benchmark_type,
                )

    def test_load_profile_supplies_normal_execution_mode(self):
        profile = perf_ci.load_profile("gcc15-spec06-0.3c")

        self.assertEqual(profile["execution_mode"], "normal")
        self.assertEqual(
            profile["artifact_name"],
            "performance-score-gcc15-spec06-0.3c",
        )

    def test_load_profile_rejects_unknown_benchmark(self):
        with self.assertRaisesRegex(ValueError, "unknown benchmark_type"):
            perf_ci.load_profile("not-a-benchmark")

    def test_load_profile_validates_h_specific_fields(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            registry_path = Path(temp_dir) / "registry.json"
            registry_path.write_text(
                json.dumps(
                    {
                        "broken-h": {
                            "execution_mode": "h",
                            "checkpoint_list": "/list",
                            "checkpoint_root": "/root",
                            "score_script": "score.sh",
                            "cluster_config": "/cluster.json",
                            "artifact_name": "artifact",
                            "description": "broken",
                        }
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "H benchmark profile"):
                perf_ci.load_profile("broken-h", registry_path)


class CheckpointListTest(unittest.TestCase):
    def test_generate_checkpoint_list_from_profile_json(self):
        source = {
            "mcf": {"points": {"101": 0.7, "202": 0.3}},
            "gcc": {"points": {"303": 1.0}},
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            source_path = Path(temp_dir) / "selection.json"
            output_path = Path(temp_dir) / "selection.lst"
            source_path.write_text(json.dumps(source), encoding="utf-8")

            perf_ci.generate_checkpoint_list(source_path, output_path)

            self.assertEqual(
                output_path.read_text(encoding="utf-8"),
                "mcf_101 mcf/101 0 0 20 20\n"
                "mcf_202 mcf/202 0 0 20 20\n"
                "gcc_303 gcc/303 0 0 20 20\n",
            )


class RunCommandTest(unittest.TestCase):
    def _normal_manifest(self):
        return {
            "profile": {
                "benchmark_type": "gcc15-spec06-0.3c",
                "execution_mode": "normal",
                "checkpoint_root": "/checkpoints",
            },
            "config_path": "configs/example/kmhv3.py",
            "checkpoint_list": "/selection.lst",
            "target_dir": "/archive/run",
            "specific_benchmarks": "mcf",
            "extra_args": "--foo=$GEM5_HOME/data",
            "distributed_servers": "",
            "distributed_jobs_per_server": 32,
            "distributed_require_idle_cpus": 32,
            "distributed_idle_probe_mode": "physical",
        }

    def test_local_command_uses_parallel_runner_and_resolves_repo_root(self):
        command = perf_ci.build_run_command(
            self._normal_manifest(), Path("/repo")
        )

        self.assertEqual(command[0:2], ["bash", "/repo/util/xs_scripts/parallel_sim.sh"])
        self.assertEqual(command[-1], "--foo=/repo/data")

    def test_distributed_command_uses_distributed_runner(self):
        manifest = self._normal_manifest()
        manifest["distributed_servers"] = "node020-node021"

        command = perf_ci.build_run_command(manifest, Path("/repo"))

        self.assertEqual(
            command[0:2],
            ["python3", "/repo/util/xs_scripts/distributed_sim.py"],
        )
        self.assertIn("node020-node021", command)
        self.assertIn("UserKnownHostsFile=/archive/run/known_hosts", command)

    def test_h_command_does_not_use_removed_prefetch_profile(self):
        manifest = {
            "profile": {
                "execution_mode": "h",
                "checkpoint_root": "/checkpoints",
                "ref_so": "/ref.so",
                "restorer": "/restorer.bin",
                "maxinsts": 40000000,
            },
            "config_path": "configs/example/kmhv3.py",
            "checkpoint_list": "/selection.lst",
            "target_dir": "/archive/run",
            "specific_benchmarks": "mcf,gcc",
            "extra_args": "--foo=bar",
            "distributed_servers": "",
            "distributed_jobs_per_server": 32,
            "distributed_require_idle_cpus": 32,
            "distributed_idle_probe_mode": "physical",
        }

        command = perf_ci.build_run_command(manifest, Path("/repo"))

        self.assertEqual(command[0:3], ["python3", "/repo/util/xs_scripts/h_spec06_perf.py", "run"])
        self.assertNotIn("--pf-control-profile", command)
        self.assertIn("--extra-args", command)


class BuildCommandTest(unittest.TestCase):
    def test_build_selects_vector_pgo_and_smt_environment(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "profile": {
                            "benchmark_type": "gcc12-spec06-smt-0.3c"
                        }
                    }
                ),
                encoding="utf-8",
            )
            args = mock.Mock(
                repo_root="/repo",
                manifest=str(manifest_path),
                vector_type="simple",
            )

            with mock.patch.object(perf_ci.subprocess, "run") as run:
                self.assertEqual(perf_ci.command_build(args), 0)

            self.assertEqual(
                run.call_args.args[0],
                ["bash", "util/pgo/basic_pgo_new_vector.sh"],
            )
            self.assertEqual(run.call_args.kwargs["cwd"], Path("/repo"))
            env = run.call_args.kwargs["env"]
            self.assertIn("GCBV_MULTI_CORE_REF_SO", env)
            self.assertEqual(env["GCB_MULTI_CORE_RESTORER"], "")

    def test_build_rejects_unknown_vector_type(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {"profile": {"benchmark_type": "gcc15-spec06-0.3c"}}
                ),
                encoding="utf-8",
            )
            args = mock.Mock(
                repo_root="/repo",
                manifest=str(manifest_path),
                vector_type="wide",
            )

            with self.assertRaisesRegex(ValueError, "vector_type"):
                perf_ci.command_build(args)


class PrepareRunTest(unittest.TestCase):
    def test_prepare_run_writes_manifest_and_generated_checkpoint_list(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            repo_root = root / "repo"
            archive_root = root / "archive"
            config_path = repo_root / "configs/example/kmhv3.py"
            config_path.parent.mkdir(parents=True)
            config_path.write_text("# config\n", encoding="utf-8")
            source_path = root / "selection.json"
            source_path.write_text(
                json.dumps({"mcf": {"points": {"101": 1.0}}}),
                encoding="utf-8",
            )
            profile = {
                "benchmark_type": "test-suite",
                "execution_mode": "normal",
                "checkpoint_list_json": str(source_path),
                "checkpoint_root": "/checkpoints",
                "score_script": "score.sh",
                "cluster_config": "/cluster.json",
                "artifact_name": "score-test",
                "description": "test profile",
            }

            manifest_path = perf_ci.prepare_run(
                profile=profile,
                repo_root=repo_root,
                archive_root=archive_root,
                config_path="configs/example/kmhv3.py",
                commit="0123456789abcdef",
                run_number="7",
                timestamp="20260814_120000",
            )

            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                Path(manifest["checkpoint_list"]).read_text(encoding="utf-8"),
                "mcf_101 mcf/101 0 0 20 20\n",
            )
            self.assertEqual(manifest["profile"], profile)
            self.assertTrue(manifest["target_dir"].endswith("_run7"))


class ScoreCommandTest(unittest.TestCase):
    def test_score_runs_from_data_proc_without_merging_stderr(self):
        manifest = {
            "profile": {
                "score_script": "gem5-score-ci.sh",
                "cluster_config": "/cluster.json",
            },
            "target_dir": "/archive/run",
        }
        completed = mock.Mock(stdout="score output\n", stderr="")

        with mock.patch.object(
            perf_ci.shutil, "copytree"
        ) as copytree, mock.patch.object(
            perf_ci.shutil, "rmtree"
        ) as rmtree, mock.patch.object(
            perf_ci.subprocess,
            "run",
            return_value=completed,
        ) as run:
            score = perf_ci._score_text(manifest, Path("/repo"))

        self.assertEqual(score, "score output\n")
        self.assertTrue(copytree.call_args.kwargs["symlinks"])
        self.assertTrue(rmtree.call_args.kwargs["ignore_errors"])
        self.assertEqual(run.call_args.args[0][0:2], ["bash", "-e"])
        self.assertEqual(run.call_args.kwargs["cwd"], Path("/repo/gem5_data_proc"))
        self.assertTrue(run.call_args.kwargs["capture_output"])

    def test_score_failure_preserves_stdout_and_stderr(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            target_dir = Path(temp_dir)
            manifest = {
                "profile": {
                    "score_script": "gem5-score-ci.sh",
                    "cluster_config": "/cluster.json",
                },
                "target_dir": str(target_dir),
            }
            failure = perf_ci.subprocess.CalledProcessError(
                7,
                ["bash", "score.sh"],
                output="partial score\n",
                stderr="score traceback\n",
            )

            with mock.patch.object(perf_ci.shutil, "copytree"), mock.patch.object(
                perf_ci.subprocess,
                "run",
                side_effect=failure,
            ), mock.patch("builtins.print"):
                with self.assertRaisesRegex(RuntimeError, "exit code 7"):
                    perf_ci._score_text(manifest, Path("/repo"))

            self.assertEqual(
                (target_dir / "score.txt").read_text(encoding="utf-8"),
                "partial score\n",
            )
            diagnostic = (target_dir / "score-error.log").read_text(
                encoding="utf-8"
            )
            self.assertIn("score traceback", diagnostic)

    def test_extract_final_score_requires_indicator(self):
        self.assertEqual(
            perf_ci.extract_final_score("Estimated Int score per GHz: 2.75\n"),
            "2.75",
        )
        with self.assertRaisesRegex(RuntimeError, "missing"):
            perf_ci.extract_final_score("no score here\n")


class ArchiveCleanupTest(unittest.TestCase):
    def test_cleanup_keeps_newest_runs_and_current_target(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            benchmark_dir = Path(temp_dir) / "benchmark"
            benchmark_dir.mkdir()
            runs = []
            for index in range(4):
                run_dir = benchmark_dir / f"run-{index}"
                run_dir.mkdir()
                os.utime(run_dir, ns=(index + 1, index + 1))
                runs.append(run_dir)

            deleted = perf_ci.cleanup_old_archives(
                benchmark_dir,
                runs[-1],
                keep_count=2,
            )

            self.assertEqual(deleted, [runs[1], runs[0]])
            self.assertEqual(
                {path.name for path in benchmark_dir.iterdir()},
                {"run-2", "run-3"},
            )

    def test_cleanup_rejects_target_outside_benchmark_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            benchmark_dir = root / "benchmark"
            benchmark_dir.mkdir()

            with self.assertRaisesRegex(ValueError, "not directly under"):
                perf_ci.cleanup_old_archives(
                    benchmark_dir,
                    root / "other/run",
                )


if __name__ == "__main__":
    unittest.main()

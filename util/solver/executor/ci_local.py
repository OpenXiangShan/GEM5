from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shlex
import subprocess
import threading
import time

from util.solver.executor.base import BaseExecutor
from util.nemu_ref.resolve import ref_path
from util.xs_scripts.ci.benchmarks import resolve_benchmark, workloads
from util.xs_scripts.ci.runner import (
    Job,
    RunnerConfig,
    run_jobs,
    simulation_env,
)
from util.solver.executor.evaluator import run_score_evaluator
from util.solver.parser.load_spec import split_problem_ref
from util.solver.runtime.overlay import write_overlay
from util.solver.types import ParsedProblem, TrialExecutionResult, TrialRequest


DEFAULT_GCBV_REF_SO = str(ref_path("normal"))
DEFAULT_GEM5_DATA_PROC = "/nfs/home/share/gem5_ci/gem5_data_proc"


class CiLocalParallelExecutor(BaseExecutor):
    def __init__(
        self,
        *,
        workdir: str | Path,
        build_type: str,
        max_parallel_trials: int,
        max_parallel_workloads: int = 1,
        timeout_minutes: int | None = None,
        gem5_data_proc: str | None = None,
        distributed_config: RunnerConfig | None = None,
    ) -> None:
        if max_parallel_trials < 1:
            raise ValueError("max_parallel_trials must be >= 1")
        if max_parallel_workloads < 1:
            raise ValueError("max_parallel_workloads must be >= 1")
        if distributed_config is not None:
            distributed_config.validate()
        self.workdir = Path(workdir)
        self.build_type = build_type
        self.max_parallel_trials = max_parallel_trials
        self.max_parallel_workloads = max_parallel_workloads
        self.timeout_minutes = timeout_minutes
        self.gem5_data_proc = gem5_data_proc or DEFAULT_GEM5_DATA_PROC
        self.distributed_config = distributed_config
        self.repo_root = Path(__file__).resolve().parents[3]
        self._cancel_requested = threading.Event()

    def _log(self, message: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        print(f"[solver-exec {timestamp}] {message}", flush=True)

    def gem5_binary(self) -> Path:
        return self.repo_root / f"build/RISCV/gem5.{self.build_type}"

    def config_path(self, problem: ParsedProblem) -> Path:
        config_path = Path(problem.config_path)
        if not config_path.is_absolute():
            config_path = self.repo_root / config_path
        return config_path.resolve()

    def problem_ref(self, problem: ParsedProblem) -> str:
        spec_path, class_name = split_problem_ref(problem.problem_ref)
        return f"{spec_path}:{class_name}"

    def _resolve_input_path(self, raw_path: str) -> Path:
        path = Path(raw_path).expanduser()
        if not path.is_absolute():
            candidates = [Path.cwd() / path]
            gem5_home_raw = os.environ.get("GEM5_HOME", "")
            if gem5_home_raw:
                candidates.append(Path(gem5_home_raw).expanduser() / path)
            candidates.append(self.repo_root / path)
            for candidate in candidates:
                if candidate.exists():
                    path = candidate
                    break
            else:
                path = candidates[0]
        path = path.resolve()
        if not path.exists():
            raise FileNotFoundError(f"path does not exist: {path}")
        return path

    def _parse_custom_bin_tokens(self, raw_value: str) -> list[str]:
        return [
            token.strip()
            for token in raw_value.replace("\n", ",").split(",")
            if token.strip()
        ]

    def _custom_bin_paths(self, problem: ParsedProblem) -> list[Path]:
        return [
            self._resolve_input_path(token)
            for token in self._parse_custom_bin_tokens(problem.custom_bin)
        ]

    def _custom_bin_entries(
        self, problem: ParsedProblem
    ) -> list[tuple[str, Path]]:
        paths = self._custom_bin_paths(problem)
        if not paths:
            raise FileNotFoundError(
                "benchmark_type=custom_bin requires at least one valid custom_bin checkpoint path"
            )

        entries = []
        multi = len(paths) > 1
        for index, checkpoint in enumerate(paths, start=1):
            base_name = self._custom_workload_name(checkpoint)
            workload_name = f"{index:02d}_{base_name}" if multi else base_name
            entries.append((workload_name, checkpoint))
        return entries

    def _selected_checkpoint(self, problem: ParsedProblem) -> str:
        if problem.uses_custom_bin_mode():
            return str(self._custom_bin_entries(problem)[0][1])
        benchmark = self._profile(problem)
        return str(
            next(workloads(benchmark, problem.specific_benchmarks)).image
        )

    def _profile(self, problem):
        benchmark = resolve_benchmark(problem.benchmark_type)
        if benchmark.trace_format:
            raise ValueError(
                "Solver requires a checkpoint profile or custom_bin"
            )
        return benchmark

    def _benchmark_entries(self, problem, benchmark):
        return [
            (w.name, w.image)
            for w in workloads(benchmark, problem.specific_benchmarks)
        ]

    def _build_gem5_command(
        self,
        problem: ParsedProblem,
        checkpoint: str | Path,
        *,
        bind_output: str | Path | None = None,
        overlay_path: str | Path | None = None,
    ) -> list[str]:
        checkpoint_path = Path(checkpoint)
        cmd = [
            str(self.gem5_binary()),
            str(self.config_path(problem)),
            f"--generic-rv-cpt={checkpoint_path}",
        ]
        if bind_output is not None or overlay_path is not None:
            cmd.append(f"--solver-problem-ref={self.problem_ref(problem)}")
        if bind_output is not None:
            cmd.append(f"--solver-bind-output={bind_output}")
        if overlay_path is not None:
            cmd.append(f"--solver-overlay={overlay_path}")
        extra_args = (
            shlex.split(problem.extra_args) if problem.extra_args else []
        )
        if checkpoint_path.suffix == ".bin" and "--raw-cpt" not in extra_args:
            extra_args.append("--raw-cpt")
        cmd.extend(extra_args)
        return cmd

    def _custom_workload_name(self, checkpoint: str | Path) -> str:
        stem = Path(checkpoint).stem
        sanitized = "".join(
            ch if ch.isalnum() or ch in "._-" else "_" for ch in stem
        )
        return sanitized.strip("._-") or "custom_bin"

    def _base_env(self) -> dict[str, str]:
        env = simulation_env()
        env.setdefault("GEM5_HOME", str(self.repo_root))
        env.setdefault("GEM5_BUILD_TYPE", self.build_type)
        env.setdefault("GCBV_REF_SO", DEFAULT_GCBV_REF_SO)
        env.setdefault("M5_OVERRIDE_PY_SOURCE", "true")
        return env

    def _timeout_seconds(self) -> float | None:
        if self.timeout_minutes is None:
            return None
        return self.timeout_minutes * 60

    def _distributed_enabled(self) -> bool:
        return (
            self.distributed_config is not None
            and self.distributed_config.enabled()
        )

    def bind_problem(
        self, problem: ParsedProblem, bind_output: str | Path
    ) -> None:
        checkpoint = self._selected_checkpoint(problem)
        cmd = self._build_gem5_command(
            problem, checkpoint, bind_output=bind_output
        )
        bind_output = Path(bind_output)
        bind_output.parent.mkdir(parents=True, exist_ok=True)
        bind_log = bind_output.with_suffix(".log")
        self._log(
            f"binding {problem.name} with representative checkpoint {checkpoint}"
        )
        with bind_log.open("w", encoding="utf-8") as handle:
            subprocess.run(
                cmd,
                check=True,
                cwd=self.repo_root,
                env={**os.environ, **self._base_env()},
                stdout=handle,
                stderr=subprocess.STDOUT,
            )
        self._log(f"binding complete; log saved to {bind_log}")

    def cancel(self) -> None:
        self._cancel_requested.set()

    def _finalize_trial_result(
        self,
        problem: ParsedProblem,
        benchmark,
        trial: TrialRequest,
        *,
        trial_dir: Path,
        raw_dir: Path,
        overlay_path: Path,
        log_path: Path,
        status: str,
        return_code: int,
        duration: float,
    ) -> TrialExecutionResult:
        raw_files = {
            "overlay_json": str(overlay_path),
            "executor_log": str(log_path),
            "raw_dir": str(raw_dir),
        }
        error = None
        if problem.uses_custom_bin_mode():
            raw_files["custom_bin"] = problem.custom_bin
        if status == "cancelled":
            error = "cancelled"
        elif problem.uses_score_txt():
            self._log(f"{trial.trial_id}: generating score.txt")
        elif problem.uses_benchmark_weighted_stats():
            self._log(f"{trial.trial_id}: generating weighted stats")
        generated_files = {}
        if status != "cancelled":
            generated_files, error = self._maybe_generate_score(
                problem,
                benchmark,
                trial_dir,
            )
        if "score_txt" in generated_files:
            raw_files["score_txt"] = generated_files["score_txt"]
            self._log(
                f"{trial.trial_id}: score.txt ready at "
                f"{generated_files['score_txt']}"
            )
        if "weighted_csv" in generated_files:
            raw_files["weighted_csv"] = generated_files["weighted_csv"]
            self._log(
                f"{trial.trial_id}: weighted stats ready at "
                f"{generated_files['weighted_csv']}"
            )
        elif error:
            self._log(f"{trial.trial_id}: score generation failed: {error}")
        self._log(
            f"completed {trial.trial_id}: status={status}, return_code={return_code}, "
            f"duration={duration:.1f}s"
        )
        return TrialExecutionResult(
            trial_id=trial.trial_id,
            generation=trial.generation,
            assignments=trial.assignments,
            status=status,
            return_code=return_code,
            duration_sec=duration,
            outdir=str(trial_dir),
            raw_files=raw_files,
            error=error,
            is_baseline=trial.is_baseline,
        )

    def _aggregate_workload_results(self, workload_results) -> tuple[str, int]:
        status = "completed"
        return_code = 0
        for result in workload_results:
            if result.status == "cancelled":
                return "cancelled", 130
            if result.status == "timeout":
                status = "timeout"
                return_code = 124
                continue
            if result.status != "completed" and status != "timeout":
                status = "failed"
                return_code = result.return_code
        return status, return_code

    def _distributed_trial_duration(self, workload_results) -> float:
        started = [
            result.started_at
            for result in workload_results
            if result.started_at is not None
        ]
        finished = [
            result.finished_at
            for result in workload_results
            if result.finished_at is not None
        ]
        if not started or not finished:
            return 0.0
        return max(0.0, max(finished) - min(started))

    def _run_batch(
        self,
        problem: ParsedProblem,
        benchmark,
        trials: list[TrialRequest],
        entries,
    ) -> list[TrialExecutionResult]:
        distributed = self._distributed_enabled()
        mode = "distributed" if distributed else "local"
        trial_state = {}
        jobs: list[Job] = []
        self._log(
            f"executing batch of {len(trials)} trial(s) with {mode} scheduler; "
            f"max_parallel_trials={self.max_parallel_trials}, "
            f"max_parallel_workloads={self.max_parallel_workloads}"
        )
        for trial in trials:
            trial_dir = self.workdir / "trials" / trial.trial_id
            raw_dir = trial_dir / "raw"
            raw_dir.mkdir(parents=True, exist_ok=True)
            overlay_path = trial_dir / "overlay.json"
            write_overlay(
                overlay_path,
                trial.trial_id,
                trial.assignments,
                is_baseline=trial.is_baseline,
            )
            runtime_overlay_path = None if trial.is_baseline else overlay_path
            log_path = trial_dir / "executor.log"
            trial_state[trial.trial_id] = {
                "trial": trial,
                "trial_dir": trial_dir,
                "raw_dir": raw_dir,
                "overlay_path": overlay_path,
                "log_path": log_path,
                "entries": entries,
                "workload_results": [],
            }
            for workload_name, checkpoint in entries:
                workload_dir = raw_dir / "spec_all" / workload_name
                command = self._build_gem5_command(
                    problem,
                    checkpoint,
                    overlay_path=runtime_overlay_path,
                )
                jobs.append(
                    Job(
                        trial_id=trial.trial_id,
                        workload_name=workload_name,
                        checkpoint=checkpoint,
                        work_dir=workload_dir,
                        command=command,
                        env=self._base_env(),
                    )
                )

        deadline = None
        timeout_seconds = self._timeout_seconds()
        if timeout_seconds is not None:
            deadline = time.monotonic() + timeout_seconds
        # Local batches contain one trial. Each retains its workload limit
        # and its own deadline; SSH trials share a batch-wide slot budget.
        parallelism = self.max_parallel_workloads
        if distributed:
            parallelism *= self.max_parallel_trials
        distributed_results = run_jobs(
            jobs,
            self.distributed_config if distributed else RunnerConfig(),
            parallelism,
            deadline=deadline,
            cancelled=self._cancel_requested.is_set,
            log=self._log,
        )
        for result in distributed_results:
            state = trial_state.get(result.job.trial_id)
            if state is not None:
                state["workload_results"].append(result)

        results: list[TrialExecutionResult] = []
        for trial in trials:
            state = trial_state[trial.trial_id]
            workload_results = state["workload_results"]
            expected = len(state["entries"])
            workload_lines = []
            if len(workload_results) != expected:
                missing = expected - len(workload_results)
                status, return_code = "failed", 1
                workload_lines.append(
                    f"missing distributed workload result(s): {missing}"
                )
            else:
                status, return_code = self._aggregate_workload_results(
                    workload_results
                )
            for result in sorted(
                workload_results,
                key=lambda item: item.job.workload_name,
            ):
                workload_lines.append(
                    f"{result.job.workload_name}: {result.job.checkpoint} -> "
                    f"{result.status} on {result.server_name} "
                    f"(return_code={result.return_code}, {result.detail})"
                )
            state["log_path"].write_text(
                "\n".join(
                    [
                        f"execution_mode: {mode}",
                        f"workloads: {expected}",
                        f"max_parallel_trials: {self.max_parallel_trials}",
                        f"max_parallel_workloads: {self.max_parallel_workloads}",
                        f"duration_sec: {self._distributed_trial_duration(workload_results):.3f}",
                        *workload_lines,
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            duration = self._distributed_trial_duration(workload_results)
            results.append(
                self._finalize_trial_result(
                    problem,
                    benchmark,
                    trial,
                    trial_dir=state["trial_dir"],
                    raw_dir=state["raw_dir"],
                    overlay_path=state["overlay_path"],
                    log_path=state["log_path"],
                    status=status,
                    return_code=return_code,
                    duration=duration,
                )
            )
            if self._cancel_requested.is_set():
                break
        return results

    def run_trials(
        self, problem: ParsedProblem, trials: list[TrialRequest]
    ) -> list[TrialExecutionResult]:
        benchmark = None
        if not problem.uses_custom_bin_mode():
            benchmark = self._profile(problem)
        entries = (
            self._benchmark_entries(problem, benchmark)
            if benchmark
            else self._custom_bin_entries(problem)
        )
        if self._distributed_enabled():
            return self._run_batch(problem, benchmark, trials, entries)

        def run_trial(trial):
            return self._run_batch(problem, benchmark, [trial], entries)

        with ThreadPoolExecutor(max_workers=self.max_parallel_trials) as pool:
            return [
                result
                for batch in pool.map(run_trial, trials)
                for result in batch
            ]

    def _maybe_generate_score(
        self,
        problem: ParsedProblem,
        benchmark,
        trial_dir: Path,
    ) -> tuple[dict[str, str], str | None]:
        needs_score = problem.uses_score_txt()
        needs_weighted_stats = problem.uses_benchmark_weighted_stats()
        if problem.uses_custom_bin_mode() and needs_score:
            return (
                {},
                "score_txt objective does not support benchmark_type=custom_bin; "
                "use stats objective for standalone workload bins",
            )
        if not needs_score and not needs_weighted_stats:
            return {}, None
        if not Path(self.gem5_data_proc).exists():
            requirement = (
                "score objective"
                if needs_score
                else "benchmark-set stats objective"
            )
            return (
                {},
                f"{requirement} requires gem5_data_proc at {self.gem5_data_proc}",
            )
        raw_spec_dir = trial_dir / "raw" / "spec_all"
        score_path = trial_dir / "score.txt"
        score_log = trial_dir / "score.log"
        score_scratch_dir = trial_dir / "score_workdir"
        weighted_csv_path = (
            trial_dir / "weighted_stats.csv" if needs_weighted_stats else None
        )
        stats_metrics = [
            objective.metric
            for objective in problem.objective_list()
            if objective.source_kind == "stats"
        ]
        evaluation = run_score_evaluator(
            gem5_data_proc=self.gem5_data_proc,
            score_script=benchmark.score_script,
            raw_spec_dir=raw_spec_dir,
            cluster_config=benchmark.weights,
            repo_root=self.repo_root,
            score_path=score_path,
            score_log=score_log,
            scratch_dir=score_scratch_dir,
            weighted_csv_path=weighted_csv_path,
            emit_score=needs_score,
            stats_metrics=stats_metrics,
        )
        return_code = evaluation.return_code
        generated_files: dict[str, str] = {}
        if weighted_csv_path is not None and weighted_csv_path.is_file():
            generated_files["weighted_csv"] = str(weighted_csv_path)
        if (
            needs_score
            and score_path.is_file()
            and score_path.stat().st_size > 0
        ):
            generated_files["score_txt"] = str(score_path)
        if evaluation.error:
            return {}, evaluation.error
        if return_code == 0:
            return generated_files, None
        if needs_score:
            return {}, f"score evaluator failed with return_code={return_code}"
        if needs_weighted_stats:
            return (
                {},
                f"weighted stats evaluator failed with return_code={return_code}",
            )
        return generated_files, None

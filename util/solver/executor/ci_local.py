from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import os
from pathlib import Path
import shlex
import subprocess
import time

from util.solver.executor.base import BaseExecutor
from util.solver.executor.benchmarks import (
    iter_workload_entries,
    locate_checkpoint,
    resolve_benchmark,
    select_representative_checkpoint,
)
from util.solver.executor.evaluator import run_score_evaluator
from util.solver.parser.load_spec import split_problem_ref
from util.solver.runtime.overlay import write_overlay
from util.solver.types import ParsedProblem, TrialExecutionResult, TrialRequest


DEFAULT_GCBV_REF_SO = "/nfs/home/share/gem5_ci/ref/normal/riscv64-nemu-interpreter-so"
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
    ) -> None:
        if max_parallel_trials < 1:
            raise ValueError("max_parallel_trials must be >= 1")
        if max_parallel_workloads < 1:
            raise ValueError("max_parallel_workloads must be >= 1")
        self.workdir = Path(workdir)
        self.build_type = build_type
        self.max_parallel_trials = max_parallel_trials
        self.max_parallel_workloads = max_parallel_workloads
        self.timeout_minutes = timeout_minutes
        self.gem5_data_proc = gem5_data_proc or DEFAULT_GEM5_DATA_PROC
        self.repo_root = Path(__file__).resolve().parents[3]

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

    def _custom_bin_entries(self, problem: ParsedProblem) -> list[tuple[str, Path]]:
        paths = self._custom_bin_paths(problem)
        if not paths:
            raise FileNotFoundError("custom_bin is set but no checkpoint path was resolved")

        entries = []
        multi = len(paths) > 1
        for index, checkpoint in enumerate(paths, start=1):
            base_name = self._custom_workload_name(checkpoint)
            workload_name = f"{index:02d}_{base_name}" if multi else base_name
            entries.append((workload_name, checkpoint))
        return entries

    def _selected_checkpoint(self, problem: ParsedProblem) -> str:
        if problem.custom_bin:
            return str(self._custom_bin_entries(problem)[0][1])
        benchmark = resolve_benchmark(problem.benchmark_type)
        return select_representative_checkpoint(benchmark, problem.specific_benchmarks)

    def _benchmark_entries(self, problem: ParsedProblem, benchmark) -> list[tuple[str, Path]]:
        entries = []
        for fields in iter_workload_entries(
            benchmark.checkpoint_list,
            problem.specific_benchmarks,
        ):
            entries.append(
                (
                    fields[0],
                    Path(locate_checkpoint(benchmark.checkpoint_root, fields[1])),
                )
            )
        if not entries:
            raise FileNotFoundError(
                f"no workload matched filters {problem.specific_benchmarks!r} "
                f"in {benchmark.checkpoint_list}"
            )
        return entries

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
        extra_args = shlex.split(problem.extra_args) if problem.extra_args else []
        if checkpoint_path.suffix == ".bin" and "--raw-cpt" not in extra_args:
            extra_args.append("--raw-cpt")
        cmd.extend(extra_args)
        return cmd

    def _custom_workload_name(self, checkpoint: str | Path) -> str:
        stem = Path(checkpoint).stem
        sanitized = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in stem)
        return sanitized.strip("._-") or "custom_bin"

    def _base_env(self) -> dict[str, str]:
        env = os.environ.copy()
        env.setdefault("GEM5_HOME", str(self.repo_root))
        env.setdefault("GEM5_BUILD_TYPE", self.build_type)
        env.setdefault("GCBV_REF_SO", DEFAULT_GCBV_REF_SO)
        env.setdefault("M5_OVERRIDE_PY_SOURCE", "true")
        return env

    def _timeout_seconds(self) -> float | None:
        if self.timeout_minutes is None:
            return None
        return self.timeout_minutes * 60

    def _remaining_timeout(self, deadline: float | None) -> float | None:
        if deadline is None:
            return None
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return 0.0
        return remaining

    def bind_problem(self, problem: ParsedProblem, bind_output: str | Path) -> None:
        checkpoint = self._selected_checkpoint(problem)
        cmd = self._build_gem5_command(problem, checkpoint, bind_output=bind_output)
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
                env=self._base_env(),
                stdout=handle,
                stderr=subprocess.STDOUT,
            )
        self._log(f"binding complete; log saved to {bind_log}")

    def _run_workload(
        self,
        trial_id: str,
        problem: ParsedProblem,
        workload_name: str,
        checkpoint: Path,
        raw_dir: Path,
        overlay_path: Path,
        deadline: float | None,
    ) -> tuple[str, int]:
        workload_dir = raw_dir / "spec_all" / workload_name
        workload_dir.mkdir(parents=True, exist_ok=True)
        log_path = workload_dir / "log.txt"
        running_path = workload_dir / "running"
        completed_path = workload_dir / "completed"
        abort_path = workload_dir / "abort"
        running_path.touch()
        cmd = self._build_gem5_command(
            problem,
            checkpoint,
            overlay_path=overlay_path,
        )
        self._log(
            f"{trial_id}: start workload {workload_name} "
            f"({checkpoint.name})"
        )
        timeout_seconds = self._remaining_timeout(deadline)
        if timeout_seconds == 0.0:
            running_path.unlink(missing_ok=True)
            abort_path.touch()
            self._log(f"{trial_id}: workload {workload_name} timed out before launch")
            return "timeout", 124
        try:
            with log_path.open("w", encoding="utf-8") as handle:
                completed = subprocess.run(
                    cmd,
                    check=False,
                    cwd=workload_dir,
                    env=self._base_env(),
                    stdout=handle,
                    stderr=subprocess.STDOUT,
                    timeout=timeout_seconds,
                )
        except subprocess.TimeoutExpired:
            running_path.unlink(missing_ok=True)
            abort_path.touch()
            self._log(f"{trial_id}: workload {workload_name} timed out")
            return "timeout", 124

        running_path.unlink(missing_ok=True)
        if completed.returncode != 0:
            abort_path.touch()
            self._log(
                f"{trial_id}: workload {workload_name} failed "
                f"(return_code={completed.returncode})"
            )
            return "failed", completed.returncode
        completed_path.touch()
        self._log(f"{trial_id}: workload {workload_name} completed")
        return "completed", 0

    def _run_trial_workloads(
        self,
        trial_id: str,
        problem: ParsedProblem,
        entries: list[tuple[str, Path]],
        raw_dir: Path,
        overlay_path: Path,
        log_path: Path,
    ) -> tuple[str, int]:
        deadline = None
        timeout_seconds = self._timeout_seconds()
        if timeout_seconds is not None:
            deadline = time.monotonic() + timeout_seconds
        lines = [
            f"workloads: {len(entries)}",
            f"max_parallel_workloads: {self.max_parallel_workloads}",
        ]
        self._log(
            f"{trial_id}: launching {len(entries)} workload(s) "
            f"with max_parallel_workloads={self.max_parallel_workloads}"
        )

        def run_entry(entry: tuple[str, Path]):
            workload_name, checkpoint = entry
            status, return_code = self._run_workload(
                trial_id,
                problem,
                workload_name,
                checkpoint,
                raw_dir,
                overlay_path,
                deadline,
            )
            return workload_name, checkpoint, status, return_code

        if len(entries) == 1 or self.max_parallel_workloads <= 1:
            workload_results = [run_entry(entry) for entry in entries]
        else:
            workload_results = [None] * len(entries)
            max_workers = min(self.max_parallel_workloads, len(entries))
            with ThreadPoolExecutor(max_workers=max_workers) as pool:
                future_map = {
                    pool.submit(run_entry, entry): index
                    for index, entry in enumerate(entries)
                }
                for future in as_completed(future_map):
                    workload_results[future_map[future]] = future.result()

        trial_status = "completed"
        trial_return_code = 0
        for workload_name, checkpoint, status, return_code in workload_results:
            lines.append(
                f"{workload_name}: {checkpoint} -> {status} (return_code={return_code})"
            )
            if status == "timeout":
                trial_status = "timeout"
                trial_return_code = 124
                continue
            if status == "failed" and trial_status != "timeout":
                trial_status = "failed"
                trial_return_code = return_code

        log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self._log(
            f"{trial_id}: workloads finished with status={trial_status} "
            f"(return_code={trial_return_code})"
        )
        return trial_status, trial_return_code

    def _run_standard_trial(
        self,
        trial_id: str,
        problem: ParsedProblem,
        benchmark,
        raw_dir: Path,
        overlay_path: Path,
        log_path: Path,
    ) -> tuple[str, int]:
        entries = self._benchmark_entries(problem, benchmark)
        return self._run_trial_workloads(
            trial_id,
            problem,
            entries,
            raw_dir,
            overlay_path,
            log_path,
        )

    def _run_custom_trial(
        self,
        trial_id: str,
        problem: ParsedProblem,
        raw_dir: Path,
        overlay_path: Path,
        log_path: Path,
    ) -> tuple[str, int]:
        entries = self._custom_bin_entries(problem)
        return self._run_trial_workloads(
            trial_id,
            problem,
            entries,
            raw_dir,
            overlay_path,
            log_path,
        )

    def _run_single_trial(
        self,
        problem: ParsedProblem,
        benchmark,
        trial: TrialRequest,
    ) -> TrialExecutionResult:
        trial_dir = self.workdir / "trials" / trial.trial_id
        raw_dir = trial_dir / "raw"
        raw_dir.mkdir(parents=True, exist_ok=True)
        overlay_path = trial_dir / "overlay.json"
        write_overlay(overlay_path, trial.trial_id, trial.assignments)
        log_path = trial_dir / "executor.log"

        self._log(f"starting {trial.trial_id} with assignments={trial.assignments}")
        start = time.monotonic()
        if problem.custom_bin:
            status, return_code = self._run_custom_trial(
                trial.trial_id,
                problem,
                raw_dir,
                overlay_path,
                log_path,
            )
        else:
            status, return_code = self._run_standard_trial(
                trial.trial_id,
                problem,
                benchmark,
                raw_dir,
                overlay_path,
                log_path,
            )
        duration = time.monotonic() - start

        raw_files = {
            "overlay_json": str(overlay_path),
            "executor_log": str(log_path),
            "raw_dir": str(raw_dir),
        }
        error = None
        if problem.custom_bin:
            raw_files["custom_bin"] = problem.custom_bin
        if problem.uses_score_txt():
            self._log(f"{trial.trial_id}: generating score.txt")
        score_path, error = self._maybe_generate_score(problem, benchmark, trial_dir)
        if score_path is not None:
            raw_files["score_txt"] = str(score_path)
            self._log(f"{trial.trial_id}: score.txt ready at {score_path}")
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
        )

    def run_trials(self, problem: ParsedProblem, trials: list[TrialRequest]) -> list[TrialExecutionResult]:
        benchmark = resolve_benchmark(problem.benchmark_type)
        self._log(
            f"executing batch of {len(trials)} trial(s) "
            f"with max_parallel_trials={self.max_parallel_trials}"
        )
        if len(trials) <= 1 or self.max_parallel_trials <= 1:
            return [
                self._run_single_trial(problem, benchmark, trial)
                for trial in trials
            ]

        results = [None] * len(trials)
        max_workers = min(self.max_parallel_trials, len(trials))
        with ThreadPoolExecutor(max_workers=max_workers) as pool:
            future_map = {
                pool.submit(self._run_single_trial, problem, benchmark, trial): index
                for index, trial in enumerate(trials)
            }
            for future in as_completed(future_map):
                results[future_map[future]] = future.result()
        return results

    def _maybe_generate_score(
        self,
        problem: ParsedProblem,
        benchmark,
        trial_dir: Path,
    ) -> tuple[Path | None, str | None]:
        needs_score = problem.uses_score_txt()
        if not needs_score and not Path(self.gem5_data_proc).exists():
            return None, None
        if not Path(self.gem5_data_proc).exists():
            if needs_score:
                return (
                    None,
                    f"score objective requires gem5_data_proc at {self.gem5_data_proc}",
                )
            return None, None
        raw_spec_dir = trial_dir / "raw" / "spec_all"
        score_path = trial_dir / "score.txt"
        score_log = trial_dir / "score.log"
        score_scratch_dir = trial_dir / "score_workdir"
        return_code = run_score_evaluator(
            gem5_data_proc=self.gem5_data_proc,
            score_script=benchmark.score_script,
            raw_spec_dir=raw_spec_dir,
            cluster_config=benchmark.cluster_config,
            repo_root=self.repo_root,
            score_path=score_path,
            score_log=score_log,
            scratch_dir=score_scratch_dir,
        )
        if return_code == 0:
            return score_path, None
        if needs_score:
            return None, f"score evaluator failed with return_code={return_code}"
        if score_path.is_file() and score_path.stat().st_size > 0:
            return score_path, None
        return None, None

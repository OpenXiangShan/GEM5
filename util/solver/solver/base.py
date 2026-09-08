from __future__ import annotations

from typing import Any

from util.solver.types import ParsedProblem, TrialRequest


class BaseSolver:
    def __init__(self, problem: ParsedProblem):
        self.problem = problem
        self._generation = 0
        self._next_trial_index = 1
        self._baseline_trial_id: str | None = None

    def initialize(self) -> None:
        return None

    def propose(self, history, batch_size: int) -> list[TrialRequest]:
        if batch_size < 1:
            return []

        trials = []
        if self._baseline_trial_id is None:
            baseline = self._make_trial({}, is_baseline=True)
            self._baseline_trial_id = baseline.trial_id
            trials.append(baseline)
        remaining = batch_size - len(trials)
        if remaining > 0 and self.problem.parameters:
            trials.extend(self._propose(history, remaining))
        return trials

    def _propose(self, history, batch_size: int) -> list[TrialRequest]:
        raise NotImplementedError

    def observe(self, results) -> None:
        return None

    def report_metadata(self) -> dict[str, Any]:
        return {
            "solver_backend": self.__class__.__name__,
            "generation": self._generation,
            "next_trial_index": self._next_trial_index,
            "config_default_trial": self._baseline_trial_id,
        }

    def _make_trial(
        self,
        assignments: dict,
        *,
        is_baseline: bool = False,
    ) -> TrialRequest:
        trial_id = f"trial_{self._next_trial_index:04d}"
        self._next_trial_index += 1
        return TrialRequest(
            trial_id=trial_id,
            generation=self._generation,
            assignments=assignments,
            is_baseline=is_baseline,
        )

    def _advance_generation(self) -> None:
        self._generation += 1

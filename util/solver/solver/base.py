from __future__ import annotations

from util.solver.types import ParsedProblem, TrialRequest


class BaseSolver:
    def __init__(self, problem: ParsedProblem):
        self.problem = problem
        self._generation = 0
        self._next_trial_index = 1

    def initialize(self) -> None:
        return None

    def propose(self, history, batch_size: int) -> list[TrialRequest]:
        raise NotImplementedError

    def observe(self, results) -> None:
        return None

    def _make_trial(self, assignments: dict) -> TrialRequest:
        trial_id = f"trial_{self._next_trial_index:04d}"
        self._next_trial_index += 1
        return TrialRequest(
            trial_id=trial_id,
            generation=self._generation,
            assignments=assignments,
        )

    def _advance_generation(self) -> None:
        self._generation += 1

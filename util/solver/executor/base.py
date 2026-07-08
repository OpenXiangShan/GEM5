from __future__ import annotations

from util.solver.types import ParsedProblem, TrialExecutionResult, TrialRequest


class BaseExecutor:
    def prepare(self, problem: ParsedProblem) -> None:
        return None

    def run_trials(
        self,
        problem: ParsedProblem,
        trials: list[TrialRequest],
    ) -> list[TrialExecutionResult]:
        raise NotImplementedError

    def cleanup(self) -> None:
        return None

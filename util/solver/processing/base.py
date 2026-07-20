from __future__ import annotations

from util.solver.types import EvaluatedTrial, ParsedProblem, TrialExecutionResult


class BaseProcessor:
    def evaluate(
        self,
        problem: ParsedProblem,
        execution: TrialExecutionResult,
    ) -> EvaluatedTrial:
        raise NotImplementedError

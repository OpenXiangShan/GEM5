from __future__ import annotations

from util.solver.types import EvaluatedTrial, ParsedProblem


class BaseReporter:
    def render_summary(self, problem: ParsedProblem, history: list[EvaluatedTrial]) -> str:
        raise NotImplementedError

    def render_charts(
        self,
        problem: ParsedProblem,
        history: list[EvaluatedTrial],
        outdir: str,
    ) -> list[str]:
        raise NotImplementedError

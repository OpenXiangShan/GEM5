from __future__ import annotations

import itertools

from util.solver.solver.base import BaseSolver


class GridSolver(BaseSolver):
    def __init__(self, problem):
        super().__init__(problem)
        self._value_matrix = [parameter.domain.iter_values() for parameter in problem.parameters]
        self._product_iter = (
            itertools.product(*self._value_matrix)
            if self._value_matrix
            else iter([])
        )

    def _propose(self, history, batch_size: int):
        trials = []
        while len(trials) < batch_size:
            try:
                combo = next(self._product_iter)
            except StopIteration:
                break
            assignments = {
                parameter.name: value
                for parameter, value in zip(self.problem.parameters, combo)
            }
            trials.append(self._make_trial(assignments))
        if trials:
            self._advance_generation()
        return trials

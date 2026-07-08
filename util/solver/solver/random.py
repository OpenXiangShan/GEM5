from __future__ import annotations

import random

from util.solver.solver.base import BaseSolver
from util.solver.types import freeze_value


class RandomSolver(BaseSolver):
    def __init__(self, problem, seed: int | None = None):
        super().__init__(problem)
        self._rng = random.Random(seed)
        self._seen = set()

    def propose(self, history, batch_size: int):
        trials = []
        max_attempts = max(32, batch_size * 32)
        attempts = 0
        while len(trials) < batch_size and attempts < max_attempts:
            attempts += 1
            assignments = {
                parameter.name: parameter.domain.sample(self._rng)
                for parameter in self.problem.parameters
            }
            key = freeze_value(assignments)
            if key in self._seen:
                continue
            self._seen.add(key)
            trials.append(self._make_trial(assignments))
        if trials:
            self._advance_generation()
        return trials

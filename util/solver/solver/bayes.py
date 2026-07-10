from __future__ import annotations

import math
import random

from util.solver.processing.aggregate import objective_value_for_trial
from util.solver.solver.base import BaseSolver
from util.solver.solver.skopt_support import import_skopt
from util.solver.spec.domain import ChoiceDomain, RangeDomain
from util.solver.types import freeze_value


class BayesSolver(BaseSolver):
    def __init__(
        self,
        problem,
        seed: int | None = None,
        base_estimator: str = "GP",
        acq_func: str = "LCB",
        acq_optimizer: str = "sampling",
        n_initial_points: int = 8,
    ):
        super().__init__(problem)
        if problem.is_multi_objective():
            raise ValueError(
                "BayesSolver only supports single-objective problems; "
                "use Nsga2Solver for multi-objective search"
            )
        self._objective = problem.primary_objective()
        if self._objective is None:
            raise ValueError("BayesSolver requires one primary objective")
        self._rng = random.Random(seed)
        self._seed = seed
        self._base_estimator = base_estimator
        self._acq_func = acq_func
        self._acq_optimizer = acq_optimizer
        self._n_initial_points = max(1, n_initial_points)
        self._seen = set()
        self._pending_generation = []
        self._generation_history: list[dict] = []
        self._told_trial_ids = set()
        self._last_generation_mode = "init"
        self._last_generated_trials = 0
        self._last_observed_trials = 0
        self._last_best_objective: float | None = None
        self._last_best_transformed: float | None = None
        self._last_model_fit_size = 0
        self._last_pending_buffer = 0
        self._Optimizer, self._Integer, self._Categorical = import_skopt()
        self._dimensions, self._decoders = self._build_dimensions()
        self._optimizer = self._Optimizer(
            dimensions=self._dimensions,
            base_estimator=self._base_estimator,
            acq_func=self._acq_func,
            acq_optimizer=self._acq_optimizer,
            random_state=self._seed,
            n_initial_points=self._n_initial_points,
        )

    def _transform_objective(self, value: float) -> float:
        return -float(value) if self._objective.direction == "max" else float(value)

    def _restore_objective(self, value: float | None) -> float | None:
        if value is None:
            return None
        return -float(value) if self._objective.direction == "max" else float(value)

    def _build_dimensions(self):
        dimensions = []
        decoders = []
        for parameter in self.problem.parameters:
            domain = parameter.domain
            if isinstance(domain, RangeDomain):
                values = domain.iter_values()
                if (
                    values
                    and all(isinstance(value, int) and not isinstance(value, bool) for value in values)
                    and len(values) == int(domain.stop - domain.start) + 1
                    and domain.step == 1
                ):
                    dimensions.append(
                        self._Integer(int(domain.start), int(domain.stop), name=parameter.name)
                    )
                    decoders.append(None)
                    continue
                if _supports_categorical_values(values):
                    dimensions.append(
                        self._Categorical(values, name=parameter.name)
                    )
                    decoders.append(None)
                else:
                    dimensions.append(
                        self._Integer(0, len(values) - 1, name=parameter.name)
                    )
                    decoders.append(_index_decoder(values))
                continue
            if isinstance(domain, ChoiceDomain):
                values = domain.iter_values()
                if _supports_categorical_values(values):
                    dimensions.append(
                        self._Categorical(values, name=parameter.name)
                    )
                    decoders.append(None)
                else:
                    dimensions.append(
                        self._Integer(0, len(values) - 1, name=parameter.name)
                    )
                    decoders.append(_index_decoder(values))
                continue
            values = domain.iter_values()
            if _supports_categorical_values(values):
                dimensions.append(self._Categorical(values, name=parameter.name))
                decoders.append(None)
            else:
                dimensions.append(
                    self._Integer(0, len(values) - 1, name=parameter.name)
                )
                decoders.append(_index_decoder(values))
        return dimensions, decoders

    def _point_to_assignments(self, point) -> dict:
        assignments = {}
        for index, parameter in enumerate(self.problem.parameters):
            value = point[index]
            decoder = self._decoders[index]
            if decoder is not None:
                value = decoder(value)
            assignments[parameter.name] = value
        return assignments

    def _drain_pending(self, batch_size: int):
        if not self._pending_generation:
            return []
        trials = self._pending_generation[:batch_size]
        self._pending_generation = self._pending_generation[batch_size:]
        self._last_pending_buffer = len(self._pending_generation)
        return trials

    def _record_generation(self, *, batch_size: int) -> None:
        self._generation_history.append(
            {
                "generation": self._generation,
                "mode": self._last_generation_mode,
                "requested_batch_size": batch_size,
                "generated_trials": self._last_generated_trials,
                "observed_trials": self._last_observed_trials,
                "model_fit_size": self._last_model_fit_size,
                "pending_trials": len(self._pending_generation),
                "seen_assignments": len(self._seen),
                "best_objective": self._last_best_objective,
                "best_transformed_objective": self._last_best_transformed,
            }
        )

    def _update_observations(self, history) -> None:
        observed_this_round = 0
        for trial in history:
            if trial.status != "valid":
                continue
            if trial.trial_id in self._told_trial_ids:
                continue
            objective_value = objective_value_for_trial(trial, self._objective)
            if objective_value is None or not math.isfinite(float(objective_value)):
                continue
            point = [trial.assignments[parameter.name] for parameter in self.problem.parameters]
            transformed = self._transform_objective(float(objective_value))
            self._optimizer.tell(point, transformed)
            self._told_trial_ids.add(trial.trial_id)
            observed_this_round += 1
        self._last_observed_trials = observed_this_round
        self._last_model_fit_size = len(self._told_trial_ids)
        if self._told_trial_ids:
            try:
                best_transformed = float(min(self._optimizer.yi))
            except (TypeError, ValueError):
                best_transformed = None
            self._last_best_transformed = best_transformed
            self._last_best_objective = self._restore_objective(best_transformed)
        else:
            self._last_best_transformed = None
            self._last_best_objective = None

    def _ask_unique_trials(self, batch_size: int):
        trials = []
        attempts = 0
        max_attempts = max(128, batch_size * 128)
        while len(trials) < batch_size and attempts < max_attempts:
            attempts += 1
            point = self._optimizer.ask()
            assignments = self._point_to_assignments(point)
            key = freeze_value(assignments)
            if key in self._seen:
                continue
            self._seen.add(key)
            trials.append(self._make_trial(assignments))
        return trials

    def _random_top_up(self, batch_size: int):
        trials = []
        attempts = 0
        max_attempts = max(64, batch_size * 64)
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
        return trials

    def propose(self, history, batch_size: int):
        pending = self._drain_pending(batch_size)
        if pending:
            return pending

        self._update_observations(history)
        self._last_generation_mode = (
            "initial_sampling"
            if self._last_model_fit_size < self._n_initial_points
            else "bayesian_search"
        )
        ask_count = max(batch_size, 1)
        generated = self._ask_unique_trials(ask_count)
        if len(generated) < ask_count:
            generated.extend(self._random_top_up(ask_count - len(generated)))
        self._last_generated_trials = len(generated)

        trials = generated[:batch_size]
        self._pending_generation = generated[batch_size:]
        self._last_pending_buffer = len(self._pending_generation)
        if generated:
            self._record_generation(batch_size=batch_size)
            self._advance_generation()
        return trials

    def report_metadata(self) -> dict:
        metadata = super().report_metadata()
        metadata.update(
            {
                "solver_backend": "BayesSolver",
                "algorithm": "Bayesian Optimization via scikit-optimize",
                "base_estimator": self._base_estimator,
                "acq_func": self._acq_func,
                "acq_optimizer": self._acq_optimizer,
                "n_initial_points": self._n_initial_points,
                "rng_seen_assignments": len(self._seen),
                "observed_trials": len(self._told_trial_ids),
                "last_generation_mode": self._last_generation_mode,
                "last_generated_trials": self._last_generated_trials,
                "last_observed_trials": self._last_observed_trials,
                "last_model_fit_size": self._last_model_fit_size,
                "pending_trials": len(self._pending_generation),
                "last_best_objective": self._last_best_objective,
                "last_best_transformed_objective": self._last_best_transformed,
                "generation_history": list(self._generation_history),
            }
        )
        return metadata


def _supports_categorical_values(values) -> bool:
    for value in values:
        if isinstance(value, (str, int, float, bool)) or value is None:
            continue
        try:
            hash(value)
        except TypeError:
            return False
    return True


def _index_decoder(values):
    def decode(index):
        return values[int(index)]

    return decode

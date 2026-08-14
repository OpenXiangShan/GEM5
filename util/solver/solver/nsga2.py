from __future__ import annotations

import random

from util.solver.processing.aggregate import pareto_frontier
from util.solver.solver.base import BaseSolver
from util.solver.solver.deap_support import import_deap
from util.solver.types import freeze_value


class Nsga2Solver(BaseSolver):
    def __init__(
        self,
        problem,
        seed: int | None = None,
        population_size: int = 8,
        mutation_prob: float = 0.3,
        crossover_prob: float = 0.9,
    ):
        super().__init__(problem)
        self._rng = random.Random(seed)
        self._population_size = max(4, population_size)
        self._mutation_prob = mutation_prob
        self._crossover_prob = crossover_prob
        self._seen = set()
        self._pending_generation = []
        self._generation_history: list[dict] = []
        self._last_population_size = 0
        self._last_frontier_size = 0
        self._last_selected_parent_pool = 0
        self._last_generated_trials = 0
        self._last_generation_mode = "init"
        self._base, self._creator, self._tools = import_deap()
        self._fitness_cls = self._ensure_fitness_type()
        self._individual_cls = self._ensure_individual_type()

    def _ensure_fitness_type(self):
        name = "SolverNSGA2Fitness"
        if not hasattr(self._creator, name):
            weights = tuple(
                1.0 if objective.direction == "max" else -1.0
                for objective in self.problem.objective_list()
            )
            self._creator.create(name, self._base.Fitness, weights=weights)
        return getattr(self._creator, name)

    def _ensure_individual_type(self):
        name = "SolverNSGA2Individual"
        if not hasattr(self._creator, name):
            self._creator.create(
                name,
                dict,
                fitness=self._fitness_cls,
                trial_id=None,
            )
        return getattr(self._creator, name)

    def _make_individual(self, assignments: dict) -> dict:
        individual = self._individual_cls(assignments)
        return individual

    def _sample_assignment(self) -> dict:
        return {
            parameter.name: parameter.domain.sample(self._rng)
            for parameter in self.problem.parameters
        }

    def _unique_trial_from_assignment(self, assignments: dict):
        key = freeze_value(assignments)
        if key in self._seen:
            return None
        self._seen.add(key)
        return self._make_trial(assignments)

    def _initial_generation(self, batch_size: int):
        self._last_generation_mode = "initial_sampling"
        trials = []
        attempts = 0
        max_attempts = max(64, batch_size * 64)
        while len(trials) < batch_size and attempts < max_attempts:
            attempts += 1
            trial = self._unique_trial_from_assignment(self._sample_assignment())
            if trial is not None:
                trials.append(trial)
        self._last_generated_trials = len(trials)
        return trials

    def _history_to_population(self, history):
        population = []
        for trial in history:
            if getattr(trial, "is_baseline", False):
                continue
            if trial.status != "valid":
                continue
            if not trial.objective_values:
                continue
            individual = self._make_individual(dict(trial.assignments))
            values = []
            missing = False
            for objective in self.problem.objective_list():
                value = trial.objective_values.get(objective.key())
                if value is None:
                    missing = True
                    break
                values.append(value)
            if missing:
                continue
            individual.fitness.values = tuple(values)
            individual.trial_id = trial.trial_id
            population.append(individual)
        return population

    def _mutate_assignment(self, base_assignment: dict) -> dict:
        mutated = dict(base_assignment)
        parameters = list(self.problem.parameters)
        if not parameters:
            return mutated
        mutate_count = max(1, int(len(parameters) * self._mutation_prob))
        chosen = self._rng.sample(parameters, k=min(len(parameters), mutate_count))
        for parameter in chosen:
            mutate = getattr(parameter.domain, "mutate", None)
            if callable(mutate):
                mutated[parameter.name] = mutate(
                    self._rng,
                    mutated[parameter.name],
                )
            else:
                mutated[parameter.name] = parameter.domain.sample(self._rng)
        return mutated

    def _crossover_assignments(self, left: dict, right: dict) -> tuple[dict, dict]:
        child_a = {}
        child_b = {}
        for parameter in self.problem.parameters:
            crossover = getattr(parameter.domain, "crossover", None)
            if callable(crossover):
                child_a[parameter.name], child_b[parameter.name] = crossover(
                    self._rng,
                    left[parameter.name],
                    right[parameter.name],
                )
            elif self._rng.random() < 0.5:
                child_a[parameter.name] = left[parameter.name]
                child_b[parameter.name] = right[parameter.name]
            else:
                child_a[parameter.name] = right[parameter.name]
                child_b[parameter.name] = left[parameter.name]
        return child_a, child_b

    def _offspring_from_population(self, population, batch_size: int):
        self._last_generation_mode = "offspring"
        trials = []
        if not population:
            return self._initial_generation(batch_size)

        parents = list(population)
        selected_parent_pool = min(len(parents), self._population_size)
        self._tools.selNSGA2(parents, selected_parent_pool)
        self._last_selected_parent_pool = selected_parent_pool
        attempts = 0
        max_attempts = max(128, batch_size * 128)
        while len(trials) < batch_size and attempts < max_attempts:
            attempts += 1
            if len(parents) >= 2:
                left, right = self._rng.sample(parents, 2)
            else:
                left = right = parents[0]
            left_assign = dict(left)
            right_assign = dict(right)
            if self._rng.random() < self._crossover_prob:
                child_a, child_b = self._crossover_assignments(left_assign, right_assign)
            else:
                child_a, child_b = left_assign, right_assign
            child_a = self._mutate_assignment(child_a)
            child_b = self._mutate_assignment(child_b)
            for child in (child_a, child_b):
                trial = self._unique_trial_from_assignment(child)
                if trial is not None:
                    trials.append(trial)
                if len(trials) >= batch_size:
                    break
        if len(trials) < batch_size:
            top_up = self._initial_generation(batch_size - len(trials))
            trials.extend(top_up)
        self._last_generated_trials = len(trials)
        return trials

    def _record_generation(self, *, batch_size: int) -> None:
        self._generation_history.append(
            {
                "generation": self._generation,
                "mode": self._last_generation_mode,
                "requested_batch_size": batch_size,
                "generated_trials": self._last_generated_trials,
                "population_size": self._last_population_size,
                "frontier_size": self._last_frontier_size,
                "selected_parent_pool": self._last_selected_parent_pool,
                "pending_trials": len(self._pending_generation),
                "seen_assignments": len(self._seen),
            }
        )

    def _drain_pending(self, batch_size: int):
        if not self._pending_generation:
            return []
        trials = self._pending_generation[:batch_size]
        self._pending_generation = self._pending_generation[batch_size:]
        return trials

    def _propose(self, history, batch_size: int):
        pending = self._drain_pending(batch_size)
        if pending:
            return pending

        if not history:
            generated = self._initial_generation(batch_size)
            self._last_population_size = 0
            self._last_frontier_size = 0
        else:
            population = self._history_to_population(history)
            frontier = pareto_frontier(history, self.problem.objective_list())
            self._last_population_size = len(population)
            self._last_frontier_size = len(frontier)
            if frontier:
                frontier_ids = {trial.trial_id for trial in frontier}
                frontier_population = [
                    individual for individual in population
                    if getattr(individual, "trial_id", None) in frontier_ids
                ]
                if frontier_population:
                    population = frontier_population + [
                        individual for individual in population
                        if getattr(individual, "trial_id", None) not in frontier_ids
                    ]
            generated = self._offspring_from_population(
                population,
                max(batch_size, self._population_size),
            )

        trials = generated[:batch_size]
        self._pending_generation = generated[batch_size:]
        if generated:
            self._record_generation(batch_size=batch_size)
            self._advance_generation()
        return trials

    def report_metadata(self) -> dict:
        metadata = super().report_metadata()
        metadata.update(
            {
                "solver_backend": "Nsga2Solver",
                "algorithm": "NSGA-II via DEAP",
                "population_size": self._population_size,
                "mutation_prob": self._mutation_prob,
                "crossover_prob": self._crossover_prob,
                "rng_seen_assignments": len(self._seen),
                "last_generation_mode": self._last_generation_mode,
                "last_population_size": self._last_population_size,
                "last_frontier_size": self._last_frontier_size,
                "last_selected_parent_pool": self._last_selected_parent_pool,
                "last_generated_trials": self._last_generated_trials,
                "pending_trials": len(self._pending_generation),
                "generation_history": list(self._generation_history),
            }
        )
        return metadata

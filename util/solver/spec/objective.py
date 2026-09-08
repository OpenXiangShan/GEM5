from __future__ import annotations

from util.solver.types import ObjectiveSpec


class Maximize:
    @staticmethod
    def stats(metric: str, *, benchmark_aggregate: str = "mean") -> ObjectiveSpec:
        return ObjectiveSpec(
            source_kind="stats",
            metric=metric,
            direction="max",
            benchmark_aggregate=benchmark_aggregate,
        )

    @staticmethod
    def score_txt(metric: str) -> ObjectiveSpec:
        return ObjectiveSpec(source_kind="score_txt", metric=metric, direction="max")


class Minimize:
    @staticmethod
    def stats(metric: str, *, benchmark_aggregate: str = "mean") -> ObjectiveSpec:
        return ObjectiveSpec(
            source_kind="stats",
            metric=metric,
            direction="min",
            benchmark_aggregate=benchmark_aggregate,
        )

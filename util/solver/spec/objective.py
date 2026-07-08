from __future__ import annotations

from util.solver.types import ObjectiveSpec


class Maximize:
    @staticmethod
    def stats(metric: str) -> ObjectiveSpec:
        return ObjectiveSpec(source_kind="stats", metric=metric, direction="max")

    @staticmethod
    def score_txt(metric: str) -> ObjectiveSpec:
        return ObjectiveSpec(source_kind="score_txt", metric=metric, direction="max")

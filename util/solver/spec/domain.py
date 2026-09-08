from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from decimal import Decimal
import random
from typing import Any


class DomainError(ValueError):
    pass


class BaseDomain:
    kind = "base"

    def iter_values(self) -> list[Any]:
        raise NotImplementedError

    def sample(self, rng: random.Random) -> Any:
        values = self.iter_values()
        if not values:
            raise DomainError("domain has no values")
        return deepcopy(rng.choice(values))

    def cardinality(self) -> int:
        return len(self.iter_values())

    def to_dict(self) -> dict[str, Any]:
        raise NotImplementedError


@dataclass(frozen=True)
class RangeDomain(BaseDomain):
    start: int | float
    stop: int | float
    step: int | float = 1

    kind = "range"

    def __post_init__(self) -> None:
        if self.step == 0:
            raise DomainError("range step must be non-zero")

    def iter_values(self) -> list[int | float]:
        if all(isinstance(value, int) and not isinstance(value, bool)
               for value in (self.start, self.stop, self.step)):
            values: list[int | float] = []
            current = self.start
            if self.step > 0:
                while current <= self.stop:
                    values.append(current)
                    current += self.step
            else:
                while current >= self.stop:
                    values.append(current)
                    current += self.step
            if not values:
                raise DomainError("integer range is empty")
            return values

        start = Decimal(str(self.start))
        stop = Decimal(str(self.stop))
        step = Decimal(str(self.step))
        values = []
        current = start
        if step > 0:
            while current <= stop:
                values.append(float(current))
                current += step
        else:
            while current >= stop:
                values.append(float(current))
                current += step
        if not values:
            raise DomainError("float range is empty")
        return values

    def to_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "start": self.start,
            "stop": self.stop,
            "step": self.step,
        }


@dataclass(frozen=True)
class ChoiceDomain(BaseDomain):
    options: tuple[Any, ...]

    kind = "choice"

    def iter_values(self) -> list[Any]:
        values = [deepcopy(option) for option in self.options]
        if not values:
            raise DomainError("choice domain is empty")
        return values

    def to_dict(self) -> dict[str, Any]:
        return {"kind": self.kind, "options": list(self.options)}


def Range(start: int | float, stop: int | float, step: int | float = 1) -> RangeDomain:
    return RangeDomain(start=start, stop=stop, step=step)


def Choice(options: list[Any] | tuple[Any, ...]) -> ChoiceDomain:
    return ChoiceDomain(tuple(options))

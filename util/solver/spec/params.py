from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from util.solver.types import ParsedParameter


@dataclass
class TunableField:
    mode: str
    domain: Any
    target: str | None = None
    default: Any = None
    declared_kind: str | None = None
    field_name: str | None = None

    def __set_name__(self, owner, name: str) -> None:
        self.field_name = name

    def __get__(self, instance, owner):
        return self

    def to_parameter(self, name: str | None = None) -> ParsedParameter:
        parameter_name = name or self.field_name
        if parameter_name is None:
            raise ValueError("tunable field name is not set")
        return ParsedParameter(
            name=parameter_name,
            mode=self.mode,
            domain=self.domain,
            target=self.target,
            declared_kind=self.declared_kind,
            default=self.default,
        )


def InferTunable(*, target: str, domain: Any, default: Any = None) -> TunableField:
    return TunableField(
        mode="infer",
        target=target,
        domain=domain,
        default=default,
        declared_kind=None,
    )


class TunableParam:
    @staticmethod
    def Unsigned(*, domain: Any, target: str | None = None, default: Any = None) -> TunableField:
        return TunableField(
            mode="explicit",
            target=target,
            domain=domain,
            default=default,
            declared_kind="Unsigned",
        )

    @staticmethod
    def Float(*, domain: Any, target: str | None = None, default: Any = None) -> TunableField:
        return TunableField(
            mode="explicit",
            target=target,
            domain=domain,
            default=default,
            declared_kind="Float",
        )

    @staticmethod
    def Bool(*, domain: Any, target: str | None = None, default: Any = None) -> TunableField:
        return TunableField(
            mode="explicit",
            target=target,
            domain=domain,
            default=default,
            declared_kind="Bool",
        )

    @staticmethod
    def VectorUnsigned(*, domain: Any, target: str | None = None, default: Any = None) -> TunableField:
        return TunableField(
            mode="explicit",
            target=target,
            domain=domain,
            default=default,
            declared_kind="VectorUnsigned",
        )

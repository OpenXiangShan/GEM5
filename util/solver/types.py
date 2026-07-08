from __future__ import annotations

from dataclasses import asdict, dataclass, field, is_dataclass
import json
from typing import Any


@dataclass
class ObjectiveSpec:
    source_kind: str
    metric: str
    direction: str = "max"


@dataclass
class StopSpec:
    max_trials: int | None = None
    no_improve_trials: int | None = None
    timeout_hours: float | None = None


@dataclass
class BindingSpec:
    target: str
    owner_path: str | None = None
    param_name: str | None = None
    declared_kind: str | None = None
    resolved_kind: str | None = None
    desc_class: str | None = None
    default: Any = None
    description: str | None = None


@dataclass
class ParsedParameter:
    name: str
    mode: str
    domain: Any
    target: str | None = None
    declared_kind: str | None = None
    default: Any = None
    resolved_kind: str | None = None
    binding: BindingSpec | None = None


@dataclass
class ParsedProblem:
    name: str
    problem_ref: str
    config_path: str
    benchmark_type: str
    specific_benchmarks: str
    custom_bin: str
    extra_args: str
    parameters: list[ParsedParameter]
    objective: ObjectiveSpec
    stop: StopSpec
    solver_hint: str | None = None
    summary_top_n: int = 16

    def parameter_map(self) -> dict[str, ParsedParameter]:
        return {parameter.name: parameter for parameter in self.parameters}


@dataclass
class TrialRequest:
    trial_id: str
    generation: int
    assignments: dict[str, Any]


@dataclass
class TrialExecutionResult:
    trial_id: str
    generation: int
    assignments: dict[str, Any]
    status: str
    return_code: int | None
    duration_sec: float
    outdir: str
    raw_files: dict[str, str] = field(default_factory=dict)
    error: str | None = None


@dataclass
class EvaluatedTrial:
    trial_id: str
    generation: int
    assignments: dict[str, Any]
    status: str
    objective_value: float | None
    metrics: dict[str, Any]
    invalid_reason: str | None
    outdir: str
    duration_sec: float
    raw_files: dict[str, str] = field(default_factory=dict)


def freeze_value(value: Any) -> str:
    return json.dumps(to_jsonable(value), sort_keys=True, separators=(",", ":"))


def to_jsonable(value: Any) -> Any:
    if is_dataclass(value):
        return {key: to_jsonable(item) for key, item in asdict(value).items()}
    if hasattr(value, "to_dict") and callable(value.to_dict):
        return to_jsonable(value.to_dict())
    if isinstance(value, dict):
        return {str(key): to_jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_jsonable(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if hasattr(value, "getValue") and callable(value.getValue):
        return to_jsonable(value.getValue())
    if hasattr(value, "value"):
        return to_jsonable(value.value)
    return str(value)

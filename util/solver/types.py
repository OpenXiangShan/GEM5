from __future__ import annotations

from dataclasses import asdict, dataclass, field, is_dataclass
import json
from typing import Any

CUSTOM_BIN_BENCHMARK_TYPE = "custom_bin"
SMT_BENCHMARK_PREFIX = "gcc12-spec06-smt-"
H_PROFILE_BENCHMARK_PREFIX = "h-spec06-"
SUPPORTED_SOLVER_CONFIG_BASENAMES = (
    "kmhv2.py",
    "kmhv3.py",
    "idealkmhv3.py",
)
SUPPORTED_SOLVER_CONFIG_PATHS = tuple(
    f"configs/example/{name}" for name in SUPPORTED_SOLVER_CONFIG_BASENAMES
)


def normalize_solver_config_path(raw_config_path: str) -> str:
    config_path = raw_config_path.strip()
    if not config_path:
        raise ValueError("config_path must not be empty")
    basename_map = {
        name: f"configs/example/{name}"
        for name in SUPPORTED_SOLVER_CONFIG_BASENAMES
    }
    if config_path in basename_map:
        return basename_map[config_path]
    if config_path in SUPPORTED_SOLVER_CONFIG_PATHS:
        return config_path
    allowed = ", ".join(SUPPORTED_SOLVER_CONFIG_PATHS)
    raise ValueError(
        f"unsupported config_path {raw_config_path!r}; expected one of: {allowed}"
    )


def validate_solver_benchmark_type(benchmark_type: str) -> None:
    if benchmark_type.startswith(SMT_BENCHMARK_PREFIX):
        raise ValueError(
            f"SMT benchmark_type {benchmark_type!r} is not supported by the "
            "solver runtime yet; choose a non-SMT benchmark_type"
        )
    if benchmark_type.startswith(H_PROFILE_BENCHMARK_PREFIX):
        raise ValueError(
            f"H-profile benchmark_type {benchmark_type!r} is not supported "
            "by the solver runtime yet"
        )


@dataclass
class ObjectiveSpec:
    source_kind: str
    metric: str
    direction: str = "max"
    benchmark_aggregate: str = "mean"

    def key(self) -> str:
        return f"{self.direction}:{self.source_kind}:{self.metric}"

    def display_name(self) -> str:
        verb = "maximize" if self.direction == "max" else "minimize"
        return f"{verb} {self.source_kind}:{self.metric}"


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
    objective: ObjectiveSpec | None
    stop: StopSpec
    objectives: list[ObjectiveSpec] = field(default_factory=list)
    solver_hint: str | None = None
    summary_top_n: int = 16

    def parameter_map(self) -> dict[str, ParsedParameter]:
        return {parameter.name: parameter for parameter in self.parameters}

    def objective_list(self) -> list[ObjectiveSpec]:
        if self.objectives:
            return list(self.objectives)
        if self.objective is not None:
            return [self.objective]
        return []

    def primary_objective(self) -> ObjectiveSpec | None:
        if self.objective is not None:
            return self.objective
        if self.objectives:
            return self.objectives[0]
        return None

    def is_multi_objective(self) -> bool:
        return len(self.objective_list()) > 1

    def uses_score_txt(self) -> bool:
        return any(
            objective.source_kind == "score_txt"
            for objective in self.objective_list()
        )

    def uses_stats(self) -> bool:
        return any(
            objective.source_kind == "stats"
            for objective in self.objective_list()
        )

    def uses_custom_bin_mode(self) -> bool:
        return self.benchmark_type == CUSTOM_BIN_BENCHMARK_TYPE

    def uses_benchmark_weighted_stats(self) -> bool:
        return (not self.uses_custom_bin_mode()) and self.uses_stats()


@dataclass
class TrialRequest:
    trial_id: str
    generation: int
    assignments: dict[str, Any]
    is_baseline: bool = False


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
    is_baseline: bool = False


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
    objective_values: dict[str, float | None] = field(default_factory=dict)
    is_baseline: bool = False


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
    if hasattr(value, "tolist") and callable(value.tolist):
        return to_jsonable(value.tolist())
    if hasattr(value, "item") and callable(value.item):
        return to_jsonable(value.item())
    if hasattr(value, "getValue") and callable(value.getValue):
        return to_jsonable(value.getValue())
    if hasattr(value, "value"):
        return to_jsonable(value.value)
    return str(value)

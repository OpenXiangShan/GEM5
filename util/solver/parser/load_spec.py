from __future__ import annotations

from hashlib import sha1
import importlib.util
import os
from pathlib import Path
import sys
from typing import Any

from util.solver.spec.base import SolveSpec
from util.solver.types import BindingSpec, ObjectiveSpec, ParsedProblem, StopSpec


class SpecLoadError(RuntimeError):
    pass


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _candidate_roots() -> list[Path]:
    roots = [Path.cwd(), _repo_root()]
    gem5_home_raw = os.environ.get("GEM5_HOME", "")
    if gem5_home_raw:
        roots.insert(1, Path(gem5_home_raw).expanduser())
    deduped = []
    seen = set()
    for root in roots:
        resolved = root.resolve()
        if resolved in seen:
            continue
        deduped.append(resolved)
        seen.add(resolved)
    return deduped


def _resolve_spec_path(raw_path: str) -> Path:
    spec_path = Path(raw_path).expanduser()
    if not spec_path.is_absolute():
        candidates = [root / spec_path for root in _candidate_roots()]
        for candidate in candidates:
            if candidate.is_file():
                spec_path = candidate
                break
        else:
            spec_path = candidates[0]
    spec_path = spec_path.resolve()
    if not spec_path.is_file():
        raise SpecLoadError(f"spec file does not exist: {spec_path}")
    return spec_path


def _discover_spec_classes(module) -> dict[str, type[SolveSpec]]:
    result = {}
    for class_name, value in vars(module).items():
        if not isinstance(value, type) or not issubclass(value, SolveSpec):
            continue
        if value is SolveSpec or value.__module__ != module.__name__:
            continue
        result[class_name] = value
    return result


def _find_unique_class_in_spec(spec_path: Path) -> str:
    module = _load_module(spec_path)
    class_names = sorted(_discover_spec_classes(module))
    if not class_names:
        raise SpecLoadError(f"{spec_path} does not define any SolveSpec subclass")
    if len(class_names) > 1:
        joined = ", ".join(class_names)
        raise SpecLoadError(
            f"{spec_path} defines multiple SolveSpec subclasses ({joined}); "
            "use path.py:ClassName explicitly"
        )
    return class_names[0]


def _solver_spec_roots() -> list[Path]:
    roots = []
    for base in _candidate_roots():
        solver_root = (base / "configs/solver_specs").resolve()
        if solver_root.is_dir() and solver_root not in roots:
            roots.append(solver_root)
    return roots


def _find_spec_by_class_name(class_name: str) -> tuple[Path, str]:
    matches = []
    for solver_root in _solver_spec_roots():
        for spec_path in sorted(solver_root.rglob("*.py")):
            module = _load_module(spec_path.resolve())
            if class_name in _discover_spec_classes(module):
                matches.append(spec_path.resolve())
    if not matches:
        raise SpecLoadError(
            f"no SolveSpec subclass named {class_name!r} found under configs/solver_specs"
        )
    unique_matches = []
    seen = set()
    for spec_path in matches:
        if spec_path in seen:
            continue
        unique_matches.append(spec_path)
        seen.add(spec_path)
    if len(unique_matches) > 1:
        joined = ", ".join(str(path) for path in unique_matches)
        raise SpecLoadError(
            f"SolveSpec subclass {class_name!r} is ambiguous; matches: {joined}"
        )
    return unique_matches[0], class_name


def split_problem_ref(problem_ref: str) -> tuple[Path, str]:
    if ":" in problem_ref:
        raw_path, class_name = problem_ref.rsplit(":", 1)
        spec_path = _resolve_spec_path(raw_path)
        if not class_name:
            raise SpecLoadError(f"missing class name in problem_ref {problem_ref!r}")
        return spec_path, class_name

    token = problem_ref.strip()
    if not token:
        raise SpecLoadError("problem_ref must not be empty")
    if token.endswith(".py"):
        spec_path = _resolve_spec_path(token)
        return spec_path, _find_unique_class_in_spec(spec_path)
    return _find_spec_by_class_name(token)


def resolve_problem_class(problem_ref: str) -> tuple[Path, str, type[SolveSpec]]:
    spec_path, class_name = split_problem_ref(problem_ref)
    module = _load_module(spec_path)
    if not hasattr(module, class_name):
        raise SpecLoadError(f"{spec_path} does not define class {class_name}")
    problem_cls = getattr(module, class_name)
    if not isinstance(problem_cls, type) or not issubclass(problem_cls, SolveSpec):
        raise SpecLoadError(f"{class_name} is not a SolveSpec subclass")
    return spec_path, class_name, problem_cls


def _load_module(spec_path: Path):
    module_name = f"solver_spec_{sha1(str(spec_path).encode('utf-8')).hexdigest()}"
    if module_name in sys.modules:
        return sys.modules[module_name]

    spec = importlib.util.spec_from_file_location(module_name, spec_path)
    if spec is None or spec.loader is None:
        raise SpecLoadError(f"failed to create import spec for {spec_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def load_problem_class(problem_ref: str):
    _, _, problem_cls = resolve_problem_class(problem_ref)
    return problem_cls


def parse_problem(problem_ref: str) -> ParsedProblem:
    spec_path, class_name, problem_cls = resolve_problem_class(problem_ref)
    objective = getattr(problem_cls, "objective", None)
    stop = getattr(problem_cls, "stop", None)
    if not isinstance(objective, ObjectiveSpec):
        raise SpecLoadError(f"{problem_cls.__name__}.objective must be an ObjectiveSpec")
    if not isinstance(stop, StopSpec):
        raise SpecLoadError(f"{problem_cls.__name__}.stop must be a StopSpec")
    config_path = getattr(problem_cls, "config_path", "")
    benchmark_type = getattr(problem_cls, "benchmark_type", "")
    if not config_path:
        raise SpecLoadError(f"{problem_cls.__name__}.config_path must be set")
    if not benchmark_type:
        raise SpecLoadError(f"{problem_cls.__name__}.benchmark_type must be set")
    summary_top_n = int(getattr(problem_cls, "summary_top_n", 16) or 16)
    if summary_top_n < 1:
        raise SpecLoadError(
            f"{problem_cls.__name__}.summary_top_n must be >= 1"
        )

    parameters = [field.to_parameter(name) for name, field in problem_cls.iter_tunables()]
    return ParsedProblem(
        name=problem_cls.__name__,
        problem_ref=f"{spec_path}:{class_name}",
        config_path=config_path,
        benchmark_type=benchmark_type,
        specific_benchmarks=getattr(problem_cls, "specific_benchmarks", "") or "",
        custom_bin=getattr(problem_cls, "custom_bin", "") or "",
        extra_args=getattr(problem_cls, "extra_args", "") or "",
        parameters=parameters,
        objective=objective,
        stop=stop,
        solver_hint=getattr(problem_cls, "solver_name", None),
        summary_top_n=summary_top_n,
    )


def merge_binding_payload(problem: ParsedProblem, payload: dict[str, Any]) -> ParsedProblem:
    parameter_map = problem.parameter_map()
    for record in payload.get("parameters", []):
        name = record["name"]
        if name not in parameter_map:
            raise SpecLoadError(f"binding payload contains unknown parameter {name!r}")
        parameter = parameter_map[name]
        binding = BindingSpec(
            target=record.get("target") or parameter.target or "",
            owner_path=record.get("owner_path"),
            param_name=record.get("param_name"),
            declared_kind=parameter.declared_kind,
            resolved_kind=record.get("resolved_kind"),
            desc_class=record.get("desc_class"),
            default=record.get("default"),
            description=record.get("description"),
        )
        parameter.binding = binding
        parameter.resolved_kind = binding.resolved_kind
        if parameter.default is None and binding.default is not None:
            parameter.default = binding.default
        if parameter.declared_kind and binding.resolved_kind:
            if parameter.declared_kind != binding.resolved_kind:
                raise SpecLoadError(
                    f"parameter {name!r} expects {parameter.declared_kind}, "
                    f"but target resolved to {binding.resolved_kind}"
                )
        if parameter.mode == "infer" and not binding.resolved_kind:
            raise SpecLoadError(f"infer parameter {name!r} did not resolve a kind")
    return problem

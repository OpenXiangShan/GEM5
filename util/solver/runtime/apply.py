from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from util.solver.parser.load_spec import load_problem_class, parse_problem
from util.solver.runtime.overlay import load_overlay
from util.solver.runtime.path_resolver import resolve_target
from util.solver.types import to_jsonable


@dataclass
class OverlayAssignment:
    name: str
    value: Any
    target: str | None = None


def _resolved_kind(param_desc) -> str:
    desc_class = type(param_desc).__name__
    base = getattr(param_desc, "ptype_str", desc_class)
    if desc_class == "VectorParamDesc":
        return f"Vector{base}"
    return base


def _load_overlay(
    path: str | Path,
) -> tuple[list[OverlayAssignment], bool]:
    payload = load_overlay(path)
    assignments = []
    for record in payload.get("assignments", []):
        assignments.append(
            OverlayAssignment(
                name=record["name"],
                value=record["value"],
                target=record.get("target"),
            )
        )
    return assignments, bool(payload.get("is_baseline", False))


def dump_problem_bindings(root, problem_ref: str, output_path: str | Path) -> None:
    problem = parse_problem(problem_ref)
    payload = {
        "problem_ref": problem_ref,
        "problem_name": problem.name,
        "parameters": [],
    }
    for parameter in problem.parameters:
        record = {
            "name": parameter.name,
            "mode": parameter.mode,
            "declared_kind": parameter.declared_kind,
            "target": parameter.target,
            "default": parameter.default,
        }
        if parameter.target:
            owner, owner_path, param_name = resolve_target(root, parameter.target)
            param_desc = owner._params[param_name]
            current_value = getattr(owner, param_name)
            record.update(
                {
                    "owner_path": owner_path,
                    "param_name": param_name,
                    "resolved_kind": _resolved_kind(param_desc),
                    "desc_class": type(param_desc).__name__,
                    "default": to_jsonable(current_value),
                    "description": getattr(param_desc, "desc", None),
                }
            )
        else:
            record["resolved_kind"] = parameter.declared_kind
        payload["parameters"].append(record)

    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)


def apply_trial_overlay(root, problem_ref: str, overlay_path: str | Path) -> None:
    problem = parse_problem(problem_ref)
    parameter_map = problem.parameter_map()
    assignments, is_baseline = _load_overlay(overlay_path)
    if is_baseline:
        return
    trial_values: dict[str, Any] = {}

    for assignment in assignments:
        if assignment.name not in parameter_map:
            raise KeyError(f"overlay contains unknown parameter {assignment.name!r}")
        parameter = parameter_map[assignment.name]
        target = assignment.target or parameter.target
        trial_values[assignment.name] = assignment.value
        if not target:
            continue
        owner, _, param_name = resolve_target(root, target)
        param_desc = owner._params[param_name]
        setattr(owner, param_name, param_desc.convert(assignment.value))

    problem_cls = load_problem_class(problem_ref)
    problem_cls.apply_trial(root, SimpleNamespace(**trial_values))

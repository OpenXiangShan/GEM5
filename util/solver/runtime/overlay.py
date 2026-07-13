from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from util.solver.types import to_jsonable


def build_overlay_payload(trial_id: str, assignments: dict[str, Any]) -> dict[str, Any]:
    return {
        "trial_id": trial_id,
        "assignments": [
            {"name": name, "value": to_jsonable(value)}
            for name, value in assignments.items()
        ],
    }


def write_overlay(path: str | Path, trial_id: str, assignments: dict[str, Any]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(
            to_jsonable(build_overlay_payload(trial_id, assignments)),
            handle,
            indent=2,
            sort_keys=True,
        )


def load_overlay(path: str | Path) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)

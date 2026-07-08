from __future__ import annotations

import csv
import json
from pathlib import Path

from util.solver.types import EvaluatedTrial, ParsedProblem, to_jsonable


def write_json(path: str | Path, payload) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(to_jsonable(payload), handle, indent=2, sort_keys=True)


def write_history_jsonl(path: str | Path, history: list[EvaluatedTrial]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for trial in history:
            handle.write(json.dumps(to_jsonable(trial), sort_keys=True))
            handle.write("\n")


def write_history_csv(path: str | Path, history: list[EvaluatedTrial]) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    assignment_keys = sorted({key for trial in history for key in trial.assignments})
    fieldnames = [
        "trial_id",
        "generation",
        "status",
        "objective_value",
        "invalid_reason",
        "duration_sec",
    ] + assignment_keys
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for trial in history:
            row = {
                "trial_id": trial.trial_id,
                "generation": trial.generation,
                "status": trial.status,
                "objective_value": trial.objective_value,
                "invalid_reason": trial.invalid_reason,
                "duration_sec": trial.duration_sec,
            }
            for key in assignment_keys:
                row[key] = trial.assignments.get(key)
            writer.writerow(row)


def persist_run_state(workdir: str | Path, problem: ParsedProblem, history: list[EvaluatedTrial], best) -> None:
    workdir = Path(workdir)
    write_json(workdir / "parsed_problem.json", problem)
    write_history_jsonl(workdir / "history.jsonl", history)
    write_history_csv(workdir / "history.csv", history)
    write_json(workdir / "best_result.json", best)

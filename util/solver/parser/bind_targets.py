from __future__ import annotations

import json
from pathlib import Path

from util.solver.parser.load_spec import merge_binding_payload
from util.solver.types import ParsedProblem


def bind_problem_targets(executor, problem: ParsedProblem, bind_output: str | Path) -> ParsedProblem:
    bind_output = Path(bind_output)
    executor.bind_problem(problem, bind_output)
    with bind_output.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)
    return merge_binding_payload(problem, payload)

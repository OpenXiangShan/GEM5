from __future__ import annotations

from pathlib import Path

from util.solver.runtime.apply import apply_trial_overlay, dump_problem_bindings


def maybe_handle_solver_runtime(root, args) -> bool:
    problem_ref = getattr(args, "solver_problem_ref", None)
    bind_output = getattr(args, "solver_bind_output", None)
    overlay = getattr(args, "solver_overlay", None)

    if bind_output:
        if not problem_ref:
            raise RuntimeError("--solver-bind-output requires --solver-problem-ref")
        dump_problem_bindings(root, problem_ref, bind_output)
        return True

    if overlay:
        if not problem_ref:
            raise RuntimeError("--solver-overlay requires --solver-problem-ref")
        apply_trial_overlay(root, problem_ref, Path(overlay))

    return False

from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    SolveSpec,
    Stop,
    TunableParam,
)


_COMPOUND_BOP = "system.l2_wrappers[0].prefetcher.compound_bop"

_TEACHER_PHASES = [
    [round_max, score_max]
    for round_max in [25, 33, 50, 55, 60]
    for score_max in [16, 20, 24, 31]
    if score_max <= round_max
]

_STUDENT_SLOTS = [
    [2, 2],
    [2, 4],
    [4, 2],
    [4, 4],
    [2, 6],
    [6, 2],
]


def _set_param(root, target, value):
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


class CompoundBOPKeyNsga2ScoreSearch(SolveSpec):
    config_path = "configs/example/kmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-bop-key"
    specific_benchmarks = ""
    custom_bin = ""
    extra_args = (
        "--param=system.l2_wrappers[0].prefetcher.use_compound_bop=True"
    )
    solver_name = "nsga2"
    summary_top_n = 16

    teacher_tests_per_access = InferTunable(
        target=f"{_COMPOUND_BOP}.teacher_tests_per_access",
        domain=Choice([1, 2]),
    )

    # Keep phase thresholds coupled so every candidate is meaningful.
    teacher_phase = TunableParam.VectorUnsigned(
        domain=Choice(_TEACHER_PHASES),
        default=[50, 20],
    )

    teacher_vbop_bad_score = InferTunable(
        target=f"{_COMPOUND_BOP}.teacher_vbop_bad_score",
        domain=Choice([1, 2, 3]),
    )

    teacher_pbop_bad_score = InferTunable(
        target=f"{_COMPOUND_BOP}.teacher_pbop_bad_score",
        domain=Choice([0, 1, 2]),
    )

    student_extra_marginal_threshold_pct = InferTunable(
        target=(
            f"{_COMPOUND_BOP}.student_extra_marginal_threshold_pct"
        ),
        domain=Choice([0, 1, 2, 3, 5]),
    )

    student_conf_alpha = InferTunable(
        target=f"{_COMPOUND_BOP}.student_conf_alpha",
        domain=Choice([0.0, 0.25, 0.5, 0.75]),
    )

    student_primary_coverage_threshold_pct = InferTunable(
        target=(
            f"{_COMPOUND_BOP}.student_primary_coverage_threshold_pct"
        ),
        domain=Choice([0, 1, 2, 3, 5]),
    )

    issue_confidence_threshold = InferTunable(
        target=f"{_COMPOUND_BOP}.issue_confidence_threshold",
        domain=Choice([10, 12, 14, 15]),
    )

    recent_filter_entries = InferTunable(
        target=f"{_COMPOUND_BOP}.recent_filter_entries",
        domain=Choice([64, 128, 256]),
    )

    # Preserve family partitioning while allowing asymmetric capacity.
    student_slots = TunableParam.VectorUnsigned(
        domain=Choice(_STUDENT_SLOTS),
        default=[4, 4],
    )

    issue_confidence_pass_decrement = InferTunable(
        target=f"{_COMPOUND_BOP}.issue_confidence_pass_decrement",
        domain=Choice([1, 2]),
    )

    issue_confidence_fail_increment = InferTunable(
        target=f"{_COMPOUND_BOP}.issue_confidence_fail_increment",
        domain=Choice([1, 2, 3]),
    )

    objective = Maximize.score_txt("Estimated overall score per GHz")

    # The wall-clock limit is authoritative; max_trials is a safety cap.
    stop = Stop(max_trials=4096, timeout_hours=24)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        round_max, score_max = trial.teacher_phase
        _set_param(
            root, f"{_COMPOUND_BOP}.teacher_round_max", round_max
        )
        _set_param(
            root, f"{_COMPOUND_BOP}.teacher_score_max", score_max
        )

        vbop_slots, pbop_slots = trial.student_slots
        _set_param(
            root, f"{_COMPOUND_BOP}.student_vbop_slots", vbop_slots
        )
        _set_param(
            root, f"{_COMPOUND_BOP}.student_pbop_slots", pbop_slots
        )

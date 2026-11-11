from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Range,
    SolveSpec,
    Stop,
)


# EStride is the only child when --vp-type=estride, so predictors[0] is it.
_ESTRIDE = "system.cpu[0].valuePred.predictors[0]"

# safeStrideBonusOther must not exceed safeStrideBonusLoad. The solver has no
# constraint language, so the search variable is an index into the values that
# are <= the sampled load bonus; apply_trial writes the real parameter.
_BONUS_VALUES = [1, 2, 4, 8, 16, 32]


def _set_param(root, target, value):
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


class EStrideGaScoreSearch(SolveSpec):
    # Single-objective GA over the EStride knobs that idealkmhv3.py actually
    # overrides. Domains sit around those overrides (notably logMaxConfidence=13,
    # not the paper-sized source default of 5). SafeStride saturation and the
    # inflight-window switches stay at the config defaults.
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    extra_args = "--enable-vp --vp-type=estride"
    solver_name = "ga"
    summary_top_n = 16

    ways = InferTunable(
        target=f"{_ESTRIDE}.ways",
        domain=Choice([2, 3, 4]),
    )

    strideWidth = InferTunable(
        target=f"{_ESTRIDE}.strideWidth",
        domain=Choice([12, 16, 20, 24, 28]),
    )

    tagWidth = InferTunable(
        target=f"{_ESTRIDE}.tagWidth",
        domain=Choice([8, 12, 16, 20]),
    )

    logESTBEntrys = InferTunable(
        target=f"{_ESTRIDE}.logESTBEntrys",
        domain=Choice([6, 7, 8, 9]),
    )

    logMaxConfidence = InferTunable(
        target=f"{_ESTRIDE}.logMaxConfidence",
        domain=Choice([9, 11, 13, 15]),
    )

    thresholdPercent = InferTunable(
        target=f"{_ESTRIDE}.thresholdPercent",
        domain=Range(0.125, 0.75, step=0.0625),
    )

    safeStridePenalty = InferTunable(
        target=f"{_ESTRIDE}.safeStridePenalty",
        domain=Choice([256, 512, 1024, 2048, 4096]),
    )

    safeStrideBonusLoad = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusLoad",
        domain=Choice([2, 4, 8, 16, 32]),
    )

    # Index, not the bonus itself: 0 is the smallest legal bonus (1).
    safeStrideBonusOtherIndex = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusOther",
        domain=Choice([0, 1, 2, 3, 4]),
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")

    stop = Stop(max_trials=4000, no_improve_trials=20, timeout_hours=30)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        load_bonus = int(trial.safeStrideBonusLoad)
        legal = [value for value in _BONUS_VALUES if value <= load_bonus]
        index = min(int(trial.safeStrideBonusOtherIndex), len(legal) - 1)
        _set_param(root, f"{_ESTRIDE}.safeStrideBonusOther", legal[index])

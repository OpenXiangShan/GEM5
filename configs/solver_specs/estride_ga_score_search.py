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
    # Single-objective Bayes search over the EStride knobs that idealkmhv3.py
    # actually overrides. Run 42974977903 (GA, 40 trials, stopped by
    # no_improve_trials=20)
    # put every top result at logMaxConfidence=9 and left 11/13/15 flat at the
    # baseline, so this pass searches the confidence bar around 9 instead of
    # the config default of 13. The predict gate is
    # thresholdPercent * 2^logMaxConfidence (enhanced_stride.cc); 7/8/9/10 at
    # 0.25-0.625 covers gates of about 32..640, against the config's
    # 0.35 * 2^13 = 2867. Table geometry and SafeStride stay, but on coarser
    # grids: the previous pass could not separate them inside a 0.35% score
    # spread. SafeStride saturation and the inflight-window switches stay at
    # the config defaults.
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    extra_args = "--enable-vp --vp-type=estride"
    solver_name = "bayes"
    summary_top_n = 16

    ways = InferTunable(
        target=f"{_ESTRIDE}.ways",
        domain=Choice([2, 3, 4]),
    )

    strideWidth = InferTunable(
        target=f"{_ESTRIDE}.strideWidth",
        domain=Choice([16, 20, 24]),
    )

    tagWidth = InferTunable(
        target=f"{_ESTRIDE}.tagWidth",
        domain=Choice([12, 16, 20]),
    )

    logESTBEntrys = InferTunable(
        target=f"{_ESTRIDE}.logESTBEntrys",
        domain=Choice([6, 7, 8]),
    )

    logMaxConfidence = InferTunable(
        target=f"{_ESTRIDE}.logMaxConfidence",
        domain=Choice([7, 8, 9, 10]),
    )

    thresholdPercent = InferTunable(
        target=f"{_ESTRIDE}.thresholdPercent",
        domain=Range(0.25, 0.625, step=0.125),
    )

    safeStridePenalty = InferTunable(
        target=f"{_ESTRIDE}.safeStridePenalty",
        domain=Choice([512, 1024, 2048]),
    )

    safeStrideBonusLoad = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusLoad",
        domain=Choice([8, 16, 32]),
    )

    # Index, not the bonus itself: 0 is the smallest legal bonus (1).
    safeStrideBonusOtherIndex = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusOther",
        domain=Choice([0, 2, 4]),
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")

    # The previous no_improve_trials=20 stopped one batch after the first good
    # sample. Bayes spends its first 8 points on random startup, so the patience
    # has to outlast that before the surrogate can stall.
    stop = Stop(max_trials=120, no_improve_trials=40, timeout_hours=30)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        load_bonus = int(trial.safeStrideBonusLoad)
        legal = [value for value in _BONUS_VALUES if value <= load_bonus]
        index = min(int(trial.safeStrideBonusOtherIndex), len(legal) - 1)
        _set_param(root, f"{_ESTRIDE}.safeStrideBonusOther", legal[index])

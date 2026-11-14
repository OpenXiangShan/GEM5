from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
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
    # Single-objective Bayes search over the confidence gate and SafeStride.
    # Two full-geometry passes on spec06 0.3c (GA run 42974977903, Bayes run
    # 43394015052) left ways, strideWidth, tagWidth and logESTBEntrys flat, so
    # those stay at the idealkmhv3.py overrides. Both passes put the score at
    # logMaxConfidence 9/10 with thresholdPercent 0.25/0.375, i.e. a predict
    # gate of thresholdPercent * 2^logMaxConfidence around 64..384, against the
    # config's 0.35 * 2^13 = 2867. This pass resolves that gate and the
    # SafeStride penalty/bonus, which were only a weak lean last time.
    # SafeStride saturation and the inflight-window switches stay at the
    # config defaults.
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    extra_args = "--enable-vp --vp-type=estride"
    solver_name = "bayes"
    summary_top_n = 16

    logMaxConfidence = InferTunable(
        target=f"{_ESTRIDE}.logMaxConfidence",
        domain=Choice([8, 9, 10]),
    )

    # 1/8 .. 4/8. With logMaxConfidence 8/9/10 the gate is 32 .. 512.
    thresholdPercent = InferTunable(
        target=f"{_ESTRIDE}.thresholdPercent",
        domain=Choice([0.125, 0.25, 0.375, 0.5]),
    )

    safeStridePenalty = InferTunable(
        target=f"{_ESTRIDE}.safeStridePenalty",
        domain=Choice([256, 512, 1024, 2048]),
    )

    safeStrideBonusLoad = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusLoad",
        domain=Choice([8, 16, 32]),
    )

    # Index, not the bonus itself: 0 is the smallest legal bonus (1).
    safeStrideBonusOtherIndex = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusOther",
        domain=Choice([0, 1, 2, 3, 4]),
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")

    # 3*4*4*3*5 = 720 candidates. The first 8 observations are random, and a
    # batch only contributes one model-guided point, so the patience has to
    # cover the startup plus several surrogate updates.
    stop = Stop(max_trials=80, no_improve_trials=24, timeout_hours=30)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        load_bonus = int(trial.safeStrideBonusLoad)
        legal = [value for value in _BONUS_VALUES if value <= load_bonus]
        index = min(int(trial.safeStrideBonusOtherIndex), len(legal) - 1)
        _set_param(root, f"{_ESTRIDE}.safeStrideBonusOther", legal[index])

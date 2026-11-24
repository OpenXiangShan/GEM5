from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    SolveSpec,
    Stop,
)


# EStride is the only child when --vp-type=estride, so predictors[0] is it.
_ESTRIDE = "system.cpu[0].valuePred.predictors[0]"


class EStrideGaScoreSearch(SolveSpec):
    # Recommended EStride configuration, pinned to a single assignment. The
    # class keeps its historical name so the earlier search runs stay traceable
    # to this file; it is no longer a search.
    #
    # Three spec06 0.3c passes (GA run 42974977903, Bayes runs 43394015052 and
    # 43595703473) agreed on one knob: the predict gate
    # thresholdPercent * 2^logMaxConfidence. Top scores clustered at a gate of
    # 192-256, against the idealkmhv3.py default 0.35 * 2^13 = 2867, and the
    # best gate bucket (256) averaged 23.0643 against 23.03 at 128 and below.
    # ways, strideWidth, tagWidth and logESTBEntrys were flat across all three
    # passes, and SafeStride only showed that penalty 2048 was too heavy, so
    # both groups stay at the idealkmhv3.py values written below. The measured
    # gain is ~+0.35% Estimated Int score per GHz with FP unchanged.
    #
    # With a single candidate the run is an A/B: trial_0001 evaluates the
    # config defaults with no overlay, trial_0002 applies the values below, so
    # the only difference is logMaxConfidence 13 -> 10 and
    # thresholdPercent 0.35 -> 0.25. Grid makes that pairing deterministic.
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    extra_args = "--enable-vp --vp-type=estride"
    solver_name = "grid"
    summary_top_n = 16

    # Table geometry: idealkmhv3.py defaults, no measurable effect on 0.3c.
    ways = InferTunable(
        target=f"{_ESTRIDE}.ways",
        domain=Choice([3]),
    )

    strideWidth = InferTunable(
        target=f"{_ESTRIDE}.strideWidth",
        domain=Choice([20]),
    )

    tagWidth = InferTunable(
        target=f"{_ESTRIDE}.tagWidth",
        domain=Choice([16]),
    )

    logESTBEntrys = InferTunable(
        target=f"{_ESTRIDE}.logESTBEntrys",
        domain=Choice([7]),
    )

    # The one real change: a predict gate of 0.25 * 2^10 = 256.
    logMaxConfidence = InferTunable(
        target=f"{_ESTRIDE}.logMaxConfidence",
        domain=Choice([10]),
    )

    thresholdPercent = InferTunable(
        target=f"{_ESTRIDE}.thresholdPercent",
        domain=Choice([0.25]),
    )

    # SafeStride: idealkmhv3.py defaults. 2048 was the only penalty that lost
    # score, and the bonus values did not separate.
    safeStridePenalty = InferTunable(
        target=f"{_ESTRIDE}.safeStridePenalty",
        domain=Choice([1024]),
    )

    safeStrideBonusLoad = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusLoad",
        domain=Choice([8]),
    )

    # Must not exceed safeStrideBonusLoad; 4 <= 8 holds for these pinned values.
    safeStrideBonusOther = InferTunable(
        target=f"{_ESTRIDE}.safeStrideBonusOther",
        domain=Choice([4]),
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")

    # One candidate plus the config-default baseline. no_improve_trials is left
    # unset so the single candidate always gets evaluated.
    stop = Stop(max_trials=4, timeout_hours=30)

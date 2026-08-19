from util.solver.spec import Choice, InferTunable, Maximize, Minimize, SolveSpec, Stop


_EGDIFF = "system.cpu[0].valuePred.predictors[0]"
_EGDIFF_STATS = "system.cpu.valuePred.predictors"


class EgDiffNsga2Search(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = ""
    extra_args = "--enable-vp --vp-type=egdiff"
    solver_name = "nsga2"
    summary_top_n = 16

    order = InferTunable(
        target=f"{_EGDIFF}.order",
        domain=Choice([8, 16, 32, 64, 128]),
    )

    fpcSeed = InferTunable(
        target=f"{_EGDIFF}.fpcSeed",
        domain=Choice([1, 2, 3, 4]),
    )

    tableEntries = InferTunable(
        target=f"{_EGDIFF}.tableEntries",
        domain=Choice([1024, 2048, 4096, 8192, 16384]),
    )

    tagBits = InferTunable(
        target=f"{_EGDIFF}.tagBits",
        domain=Choice([8, 10, 12, 14, 16]),
    )

    usefulBits = InferTunable(
        target=f"{_EGDIFF}.usefulBits",
        domain=Choice([1, 2, 3, 4]),
    )

    allocationProbabilityDenominator = InferTunable(
        target=f"{_EGDIFF}.allocationProbabilityDenominator",
        domain=Choice([1, 2, 4, 8, 16, 32, 64]),
    )

    tickBits = InferTunable(
        target=f"{_EGDIFF}.tickBits",
        domain=Choice([8, 10, 12, 14]),
    )

    normalPredictionLatency = InferTunable(
        target=f"{_EGDIFF}.normalPredictionLatency",
        domain=Choice([1, 2, 3, 4]),
    )

    deferredPredictionLatency = InferTunable(
        target=f"{_EGDIFF}.deferredPredictionLatency",
        domain=Choice([1, 2, 3, 4]),
    )

    lastMispWindow = InferTunable(
        target=f"{_EGDIFF}.lastMispWindow",
        domain=Choice([128, 256, 512, 1024, 2048, 4096]),
    )

    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Maximize.stats(f"{_EGDIFF_STATS}.appliedCorrect"),
        Maximize.stats(f"{_EGDIFF_STATS}.predictionsApplied"),
        Minimize.stats(f"{_EGDIFF_STATS}.appliedIncorrect"),
        Minimize.stats(f"{_EGDIFF_STATS}.squashedSlots"),
    ]

    stop = Stop(max_trials=4000, no_improve_trials=20, timeout_hours=30)

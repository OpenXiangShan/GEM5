from util.solver.spec import Choice, InferTunable, Maximize, SolveSpec, Stop


_EGDIFF = "system.cpu[0].valuePred.predictors[0]"


class EgDiffGaScoreSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = ""
    extra_args = "--enable-vp --vp-type=egdiff"
    solver_name = "ga"
    summary_top_n = 16

    usefulBits = InferTunable(
        target=f"{_EGDIFF}.usefulBits",
        domain=Choice([2, 3, 4, 5]),
    )

    allocationProbabilityDenominator = InferTunable(
        target=f"{_EGDIFF}.allocationProbabilityDenominator",
        domain=Choice([1, 2, 4, 8, 16, 32, 64]),
    )

    tickBits = InferTunable(
        target=f"{_EGDIFF}.tickBits",
        domain=Choice([1, 5, 10, 15, 20, 30, 50]),
    )

    lastMispWindow = InferTunable(
        target=f"{_EGDIFF}.lastMispWindow",
        domain=Choice([1, 256, 512, 1024, 2048, 4096]),
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")

    stop = Stop(max_trials=4000, no_improve_trials=20, timeout_hours=30)

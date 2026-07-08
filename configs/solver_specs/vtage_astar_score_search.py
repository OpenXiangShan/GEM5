from util.solver.spec import InferTunable, Maximize, Range, SolveSpec, Stop


class VTAGEAstarScoreSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = "astar"
    extra_args = ""
    solver_name = "random"
    summary_top_n = 16

    allocProbLoadL1Hit = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].allocProbLoadL1Hit",
        domain=Range(0.0, 1.0, step=0.125),
    )

    uIncProbFastLoad = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].uIncProbFastLoad",
        domain=Range(0.0, 1.0, step=0.125),
    )

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Range(0.0, 1.0, step=0.125),
    )

    shortHistoryAllocBias = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].shortHistoryAllocBias",
        domain=Range(0.0, 1.0, step=0.125),
    )

    deepAllocExtraHopProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].deepAllocExtraHopProb",
        domain=Range(0.0, 1.0, step=0.125),
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")
    stop = Stop(max_trials=24, no_improve_trials=8, timeout_hours=12)

from util.solver.spec import Choice, InferTunable, Maximize, Range, SolveSpec, Stop, TunableParam


class VTAGEIPCSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = ""
    extra_args = ""
    solver_name = "random"

    histLengths = TunableParam.VectorUnsigned(
        target="system.cpu[0].valuePred.predictors[1].histLengths",
        domain=Choice([
            [0, 0, 3, 7, 15, 31, 63, 90, 127],
            [0, 0, 4, 8, 16, 32, 64, 96, 128],
            [0, 0, 2, 6, 14, 30, 62, 94, 126],
        ]),
    )

    predictConfThreshold = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].predictConfThreshold",
        domain=Range(512, 1536, step=128),
    )

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Range(0.0, 1.0, step=0.125),
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=16, timeout_hours=6)

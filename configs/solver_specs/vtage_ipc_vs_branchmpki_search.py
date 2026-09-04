from util.solver.spec import InferTunable, Maximize, Minimize, Range, SolveSpec, Stop


class VTAGEIPCVsBranchMispredictSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = ""
    extra_args = ""
    solver_name = "random"
    summary_top_n = 16

    predictConfThreshold = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].predictConfThreshold",
        domain=Range(512, 1536, step=128),
    )

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Range(0.0, 1.0, step=0.125),
    )

    shortHistoryAllocBias = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].shortHistoryAllocBias",
        domain=Range(0.0, 1.0, step=0.125),
    )

    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.branchMispredicts"),
    ]
    stop = Stop(max_trials=24, no_improve_trials=8, timeout_hours=12)

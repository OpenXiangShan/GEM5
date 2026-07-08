from util.solver.spec import InferTunable, Maximize, Range, SolveSpec, Stop


class CoremarkIPCSmoke(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    custom_bin = "/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin"
    extra_args = "--maxinsts=1000000"
    solver_name = "random"
    summary_top_n = 8

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Range(0.0, 1.0, step=0.25),
    )

    shortHistoryAllocBias = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].shortHistoryAllocBias",
        domain=Range(0.0, 1.0, step=0.25),
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=8, no_improve_trials=4, timeout_hours=1)

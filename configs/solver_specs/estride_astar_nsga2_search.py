from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Minimize,
    Range,
    SolveSpec,
    Stop,
)


_ESTRIDE = "system.cpu[0].valuePred.predictors[0]"


class EStrideAstarNsga2Search(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "custom_bin"
    custom_bin = (
        "/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/"
        "checkpoint-0-0-0/astar_biglakes/3421/_3421_0.140286_.zstd"
    )
    extra_args = ""
    solver_name = "nsga2"
    summary_top_n = 20

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
        domain=Choice([3, 4, 5, 6]),
    )

    thresholdPercent = InferTunable(
        target=f"{_ESTRIDE}.thresholdPercent",
        domain=Range(0.125, 0.75, step=0.0625),
    )

    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.commit.squashDueToValuePrediction"),
        Maximize.stats("system.cpu.valuePred.predictors.VPaccuracy"),
        Maximize.stats("system.cpu.valuePred.predictors.VPcoverage"),
    ]

    stop = Stop(max_trials=400, no_improve_trials=40, timeout_hours=12)

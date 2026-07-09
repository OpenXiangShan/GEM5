from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Minimize,
    Range,
    SolveSpec,
    Stop,
)


class VTAGEAstarNsga2Search(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    custom_bin = (
        "/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/"
        "checkpoint-0-0-0/astar_biglakes/3421/_3421_0.140286_.zstd"
    )
    extra_args = ""
    solver_name = "nsga2"
    summary_top_n = 16

    # confBits drives the two confidence thresholds via apply_trial().
    confBits = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].confBits",
        domain=Choice([8, 9, 10, 11, 12]),
    )

    usefulBits = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].usefulBits",
        domain=Choice([1, 2, 3, 4]),
    )

    mispredBackoffDistance = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].mispredBackoffDistance",
        domain=Choice([32, 64, 96, 128, 192, 256, 384]),
    )

    agingTickMax = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].agingTickMax",
        domain=Choice([256, 512, 768, 1024, 1536, 2048]),
    )

    agingPenaltyOnAlloc = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].agingPenaltyOnAlloc",
        domain=Choice([0, 1, 2, 3, 4]),
    )

    agingPenaltyOnNoAlloc = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].agingPenaltyOnNoAlloc",
        domain=Choice([1, 3, 5, 7, 9]),
    )

    allocProbLoadL1Hit = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].allocProbLoadL1Hit",
        domain=Range(0.125, 0.5, step=0.0625),
    )

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Range(0.125, 0.5, step=0.0625),
    )

    shortHistoryAllocBias = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].shortHistoryAllocBias",
        domain=Range(0.0, 0.375, step=0.0625),
    )

    deepAllocExtraHopProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].deepAllocExtraHopProb",
        domain=Range(0.0, 0.375, step=0.0625),
    )

    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.commit.squashDueToValuePrediction"),
        Maximize.stats("system.cpu.valuePred.VTAGE.VPaccuracy"),
        Maximize.stats("system.cpu.valuePred.VTAGE.VPcoverage"),
    ]

    # Keep the overall search bounded for an about-2-hour local run on one workload.
    stop = Stop(max_trials=400, no_improve_trials=16, timeout_hours=2)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        owner, _, param_name = resolve_target(
            root,
            "system.cpu[0].valuePred.predictors[1].predictConfThreshold",
        )
        conf_bits = int(trial.confBits)
        predict_threshold = 1 << (conf_bits - 1)
        hash_threshold = predict_threshold - 1
        owner.predictConfThreshold = owner._params[param_name].convert(predict_threshold)

        owner2, _, param_name2 = resolve_target(
            root,
            "system.cpu[0].valuePred.predictors[1].hashOnlyUpgradeThreshold",
        )
        owner2.hashOnlyUpgradeThreshold = owner2._params[param_name2].convert(hash_threshold)

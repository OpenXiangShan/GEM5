from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Minimize,
    SolveSpec,
    Stop,
    TunableParam,
)


def _hist_length_candidates():
    seed_vectors = [
        [0, 0, 3, 7, 15, 31, 63, 90, 127],
        [0, 0, 4, 8, 16, 32, 64, 96, 128],
        [0, 0, 4, 8, 15, 28, 50, 90, 160],
        [0, 0, 4, 9, 17, 29, 56, 109, 160],
        [0, 0, 5, 10, 20, 40, 80, 120, 160],
    ]
    scales_by_seed = {
        0: [0.75, 0.875, 1.0, 1.125],
        1: [0.75, 0.875, 1.0, 1.125],
        2: [0.75, 0.875, 1.0],
        3: [0.75, 0.875, 1.0],
        4: [0.75, 1.0],
    }

    options = set()
    for index, seed in enumerate(seed_vectors):
        for scale in scales_by_seed[index]:
            candidate = [0, 0]
            previous = 0
            valid = True
            for value in seed[2:]:
                scaled = int(round(value * scale))
                if scaled > 160 or scaled <= previous:
                    valid = False
                    break
                candidate.append(scaled)
                previous = scaled
            if valid:
                options.add(tuple(candidate))
    return [list(option) for option in sorted(options)]


class VTAGERegressionWideNsga2Search(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "custom_bin"
    custom_bin = "\n".join([
        (
            "/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/"
            "checkpoint-0-0-0/gobmk_nngs/4807/_4807_0.149835_.zstd"
        ),
        (
            "/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/"
            "checkpoint-0-0-0/sjeng/22213/_22213_0.085858_.zstd"
        ),
        (
            "/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/"
            "checkpoint-0-0-0/h264ref_foreman.baseline/17854/"
            "_17854_0.136505_.zstd"
        ),
        (
            "/nfs/home/share/checkpoints_profiles/spec06_gcc15_rv64gcb_base_260122/"
            "checkpoint-0-0-0/h264ref_foreman.baseline/9575/"
            "_9575_0.110028_.zstd"
        ),
    ])
    extra_args = ""
    solver_name = "nsga2"
    summary_top_n = 16

    # The workload mix is chosen from gcc15-spec06-0.8c checkpoints that are
    # both relatively high-weight and show clear IPC loss after enabling VTAGE.
    # The 4 checkpoints explicitly cover gobmk, sjeng, and h264ref.
    histLengths = TunableParam.VectorUnsigned(
        target="system.cpu[0].valuePred.predictors[1].histLengths",
        domain=Choice(_hist_length_candidates()),
    )

    confBits = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].confBits",
        domain=Choice([7, 8, 9, 10, 11, 12, 13]),
    )

    usefulBits = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].usefulBits",
        domain=Choice([1, 2, 3, 4, 5]),
    )

    mispredBackoffDistance = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].mispredBackoffDistance",
        domain=Choice([16, 32, 64, 96, 128, 192, 256, 384, 512, 768]),
    )

    agingTickMax = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].agingTickMax",
        domain=Choice([128, 256, 512, 768, 1024, 1536, 2048, 3072, 4096]),
    )

    agingPenaltyOnAlloc = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].agingPenaltyOnAlloc",
        domain=Choice([0, 1, 2, 3, 4, 5, 6, 8]),
    )

    agingPenaltyOnNoAlloc = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].agingPenaltyOnNoAlloc",
        domain=Choice([0, 1, 3, 5, 7, 9, 11, 13, 15]),
    )

    allocProbLoadL1Hit = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].allocProbLoadL1Hit",
        domain=Choice([
            0.0,
            0.0625,
            0.125,
            0.1875,
            0.25,
            0.3125,
            0.375,
            0.4375,
            0.5,
            0.5625,
            0.625,
            0.6875,
            0.75,
        ]),
    )

    valueArrayUpgradeProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].valueArrayUpgradeProb",
        domain=Choice([
            0.0,
            0.0625,
            0.125,
            0.1875,
            0.25,
            0.3125,
            0.375,
            0.4375,
            0.5,
            0.5625,
            0.625,
            0.6875,
            0.75,
        ]),
    )

    shortHistoryAllocBias = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].shortHistoryAllocBias",
        domain=Choice([
            0.0,
            0.0625,
            0.125,
            0.1875,
            0.25,
            0.3125,
            0.375,
            0.4375,
            0.5,
            0.5625,
            0.625,
        ]),
    )

    deepAllocExtraHopProb = InferTunable(
        target="system.cpu[0].valuePred.predictors[1].deepAllocExtraHopProb",
        domain=Choice([
            0.0,
            0.0625,
            0.125,
            0.1875,
            0.25,
            0.3125,
            0.375,
            0.4375,
            0.5,
            0.5625,
            0.625,
        ]),
    )

    objectives = [
        Maximize.stats("system.cpu.ipc"),
        Minimize.stats("system.cpu.commit.squashDueToValuePrediction"),
        Maximize.stats("system.cpu.valuePred.VTAGE.VPaccuracy"),
        Maximize.stats("system.cpu.valuePred.VTAGE.VPcoverage"),
    ]

    stop = Stop(max_trials=5000, timeout_hours=20)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        owner, _, param_name = resolve_target(
            root,
            "system.cpu[0].valuePred.predictors[1].predictConfThreshold",
        )
        conf_bits = int(trial.confBits)
        predict_threshold = 1 << (conf_bits - 1)
        hash_threshold = predict_threshold - 1
        owner.predictConfThreshold = owner._params[param_name].convert(
            predict_threshold
        )

        owner2, _, param_name2 = resolve_target(
            root,
            "system.cpu[0].valuePred.predictors[1].hashOnlyUpgradeThreshold",
        )
        owner2.hashOnlyUpgradeThreshold = owner2._params[param_name2].convert(
            hash_threshold
        )

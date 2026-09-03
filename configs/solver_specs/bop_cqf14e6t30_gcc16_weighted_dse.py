"""Weighted GCC16 DSE for the fixed-area BOP-CQF14E6T30 design.

The 32 custom checkpoints are the highest-sensitivity GCC16 1.0C slices
selected from the native-BOP versus student+CQF comparison.  Their SimPoint
weights are used for the IPC objective.  Quality/Feedback capacities and the
compact age layout are intentionally fixed; only BOP learner and CQF policy
parameters are explored.
"""

from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import Choice, Maximize, SolveSpec, Stop, TunableParam


_BOP_TARGETS = (
    "system.l2_wrappers[0].prefetcher.bop_large",
    "system.l2_wrappers[0].prefetcher.bop_small",
)

_CHECKPOINT_ROOT = (
    "/nfs/home/share/checkpoints_profiles/"
    "spec06_gcc16_rva23_novec_260820/checkpoint"
)

# Keep this order stable.  ci_local prefixes multi-custom-bin workloads with
# 01_, 02_, ... so the sorted workload order remains identical to the weights.
_CHECKPOINTS = (
    "mcf/6753/_6753_0.218742_memory_.zstd",
    "gcc_166/800/_800_0.155920_memory_.zstd",
    "omnetpp/18492/_18492_0.313070_memory_.zstd",
    "omnetpp/14639/_14639_0.246019_memory_.zstd",
    "gcc_s04/5007/_5007_0.127558_memory_.zstd",
    "omnetpp/11664/_11664_0.228645_memory_.zstd",
    "mcf/9639/_9639_0.095321_memory_.zstd",
    "leslie3d/11950/_11950_0.157246_memory_.zstd",
    "mcf/10206/_10206_0.055122_memory_.zstd",
    "gcc_expr2/3771/_3771_0.078963_memory_.zstd",
    "gcc_cpdecl/1944/_1944_0.216865_memory_.zstd",
    "GemsFDTD/44528/_44528_0.041582_memory_.zstd",
    "gcc_expr/2615/_2615_0.122405_memory_.zstd",
    "omnetpp/19191/_19191_0.098068_memory_.zstd",
    "mcf/3817/_3817_0.044770_memory_.zstd",
    "gcc_typeck/3417/_3417_0.081620_memory_.zstd",
    "gcc_cpdecl/2816/_2816_0.039022_memory_.zstd",
    "GemsFDTD/63877/_63877_0.043775_memory_.zstd",
    "GemsFDTD/67903/_67903_0.034936_memory_.zstd",
    "soplex_pds-50/3318/_3318_0.069296_memory_.zstd",
    "mcf/7383/_7383_0.040871_memory_.zstd",
    "omnetpp/7477/_7477_0.076878_memory_.zstd",
    "GemsFDTD/35695/_35695_0.040772_memory_.zstd",
    "gcc_cpdecl/4089/_4089_0.184346_memory_.zstd",
    "zeusmp/60982/_60982_0.048827_memory_.zstd",
    "milc/30237/_30237_0.048747_memory_.zstd",
    "milc/25162/_25162_0.050729_memory_.zstd",
    "milc/30106/_30106_0.085219_memory_.zstd",
    "mcf/11326/_11326_0.144730_memory_.zstd",
    "milc/24434/_24434_0.064698_memory_.zstd",
    "GemsFDTD/56361/_56361_0.035062_memory_.zstd",
    "GemsFDTD/53957/_53957_0.038748_memory_.zstd",
)

CUSTOM_CHECKPOINTS = ",".join(
    f"{_CHECKPOINT_ROOT}/{relative_path}" for relative_path in _CHECKPOINTS
)

# These values come from the GCC16 profile's checkpoints_all.json.  They are
# intentionally kept in the same order as _CHECKPOINTS.
CUSTOM_CHECKPOINT_WEIGHTS = (
    0.218742,
    0.155920,
    0.313070,
    0.246019,
    0.127558,
    0.228645,
    0.0953213,
    0.157246,
    0.0551223,
    0.0789625,
    0.216865,
    0.0415823,
    0.122405,
    0.0980677,
    0.0447701,
    0.0816198,
    0.0390222,
    0.0437745,
    0.0349358,
    0.0692956,
    0.0408712,
    0.0768784,
    0.0407724,
    0.184346,
    0.0488272,
    0.0487470,
    0.0507288,
    0.0852193,
    0.144730,
    0.0646976,
    0.0350615,
    0.0387478,
)


def _set_param(root, target: str, value) -> None:
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


def _set_both(root, parameter: str, value) -> None:
    for target in _BOP_TARGETS:
        _set_param(root, f"{target}.{parameter}", value)


class BOPCQF14E6T30Gcc16WeightedDse(SolveSpec):
    config_path = "configs/example/kmhv3.py"
    benchmark_type = "custom_bin"
    specific_benchmarks = ""
    custom_bin = CUSTOM_CHECKPOINTS
    custom_bin_weights = CUSTOM_CHECKPOINT_WEIGHTS
    extra_args = (
        "--enable-bop-student-cover "
        "--disable-bop-pc-control "
        "--enable-bop-direct-quality-gate "
        "--bop-direct-quality-profile=bop-cqf-dse "
        "--difftest-ref-so=/nfs/home/share/gem5_ci/ref/normal/"
        "riscv64-nemu-notama-tvalref-so"
    )
    solver_name = "nsga2"
    summary_top_n = 24

    # BOP learner parameters.  score_max is derived from round_max and the
    # ratio, preserving a compact search space while allowing early/late
    # learner termination to move together.
    round_max = TunableParam.Unsigned(
        domain=Choice([16, 24, 32, 40, 50, 64]), default=50
    )
    score_ratio = TunableParam.Unsigned(
        domain=Choice([25, 40, 50, 62, 75, 90, 100]), default=62
    )
    large_bad_score = TunableParam.Unsigned(
        domain=Choice([0, 1, 2, 4, 8]), default=2
    )
    small_bad_score = TunableParam.Unsigned(
        domain=Choice([0, 1, 2, 4, 8]), default=1
    )

    # CQF policy parameters.  Both BOP kinds share policy values so the
    # controller remains a single fixed-area design; learner bad_score stays
    # kind-specific above because Large/Small have different native streams.
    unused_per_useful = TunableParam.Unsigned(
        domain=Choice([2, 4, 6, 8, 10, 12, 16, 20]), default=10
    )
    min_samples = TunableParam.Unsigned(
        domain=Choice([16, 32, 64]), default=32
    )
    sample_period = TunableParam.Unsigned(
        domain=Choice([4, 8, 16, 32]), default=16
    )
    block_guard = TunableParam.Unsigned(
        domain=Choice([0, 2, 4, 8]), default=4
    )
    strict_ratio_multiplier = TunableParam.Unsigned(
        domain=Choice([1, 2, 3]), default=2
    )
    strict_probe_period = TunableParam.Unsigned(
        domain=Choice([16, 32, 64, 128]), default=64
    )
    reopen_probe_period = TunableParam.Unsigned(
        domain=Choice([16, 32, 64, 128]), default=64
    )
    reopen_confirm_samples = TunableParam.Unsigned(
        domain=Choice([0, 4, 8, 16]), default=0
    )
    reopen_guard = TunableParam.Unsigned(
        domain=Choice([0, 2, 4, 8]), default=4
    )
    decay_period = TunableParam.Unsigned(
        domain=Choice([32, 64, 128, 256]), default=64
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=256, timeout_hours=46)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        round_max = int(trial.round_max)
        score_max = round_max * int(trial.score_ratio) // 100
        _set_both(root, "round_max", round_max)
        _set_both(root, "score_max", score_max)
        _set_both(root, "bad_score", int(trial.large_bad_score))
        _set_param(
            root,
            f"{_BOP_TARGETS[1]}.bad_score",
            int(trial.small_bad_score),
        )

        # Explicitly reassert all area/layout knobs that define the certified
        # fixed-area BOP-CQF14E6T30 base.
        _set_both(root, "direct_quality_entries", 256)
        _set_both(root, "direct_quality_ways", 4)
        _set_both(root, "direct_quality_feedback_entries", 256)
        _set_both(root, "direct_quality_feedback_ways", 4)
        _set_both(root, "direct_quality_horizon", 2048)
        _set_both(root, "direct_quality_epoch_bits", 6)
        _set_both(root, "direct_quality_epoch_shift", 6)
        _set_both(root, "direct_quality_epoch_timeout", 30)

        ratio = int(trial.unused_per_useful)
        strict_ratio = ratio * int(trial.strict_ratio_multiplier)
        sample_period = int(trial.sample_period)
        strict_probe = int(trial.strict_probe_period)
        guard = int(trial.block_guard)

        _set_both(root, "direct_quality_min_samples", int(trial.min_samples))
        _set_both(root, "direct_quality_observe_sample_period", sample_period)
        _set_both(root, "direct_quality_open_sample_period", sample_period)
        _set_both(root, "direct_quality_unused_per_useful", ratio)
        _set_both(root, "direct_quality_strict_unused_per_useful", strict_ratio)
        _set_both(root, "direct_quality_reopen_unused_per_useful", ratio)
        _set_both(root, "direct_quality_block_guard", guard)
        _set_both(root, "direct_quality_strict_block_guard", guard)
        _set_both(root, "direct_quality_reopen_guard", int(trial.reopen_guard))
        _set_both(root, "direct_quality_block_probe_period", strict_probe)
        _set_both(
            root,
            "direct_quality_borderline_block_probe_period",
            strict_probe // 8,
        )
        _set_both(
            root,
            "direct_quality_reopen_probe_period",
            int(trial.reopen_probe_period),
        )
        _set_both(
            root,
            "direct_quality_reopen_confirm_samples",
            int(trial.reopen_confirm_samples),
        )
        _set_both(root, "direct_quality_decay_period", int(trial.decay_period))

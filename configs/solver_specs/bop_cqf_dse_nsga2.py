"""Joint IPC exploration for the compact raw-BOP DirectQuality controller.

This spec deliberately tunes neither the BOP learner nor any physical
prefetch-path component. Every trial runs the same native BOP Large/Small
learners and alters only the shared raw-candidate CQF gate.
"""

from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import Choice, Maximize, SolveSpec, Stop, TunableParam


_BOP_TARGETS = (
    "system.l2_wrappers[0].prefetcher.bop_large",
    "system.l2_wrappers[0].prefetcher.bop_small",
)

_CHECKPOINT_ROOT = (
    "/nfs/home/share/checkpoints_profiles/"
    "spec06_gcc15_rv64gcb_base_260604/checkpoint"
)
_CUSTOM_CHECKPOINTS = """
mcf/11849/_11849_0.148205_memory_.zstd
mcf/12568/_12568_0.144497_memory_.zstd
mcf/3005/_3005_0.187251_memory_.zstd
mcf/1815/_1815_0.005668_memory_.zstd
astar_biglakes/12985/_12985_0.052064_memory_.zstd
astar_biglakes/2700/_2700_0.052747_memory_.zstd
bzip2_chicken/2095/_2095_0.009256_memory_.zstd
bzip2_chicken/4032/_4032_0.033074_memory_.zstd
bzip2_combined/17199/_17199_0.019500_memory_.zstd
bzip2_chicken/7569/_7569_0.079354_memory_.zstd
gcc_200/4452/_4452_0.164923_memory_.zstd
gcc_200/536/_536_0.094140_memory_.zstd
gcc_expr/2283/_2283_0.111046_memory_.zstd
xalancbmk/29382/_29382_0.002935_memory_.zstd
xalancbmk/1705/_1705_0.008895_memory_.zstd
xalancbmk/1945/_1945_0.008053_memory_.zstd
GemsFDTD/45278/_45278_0.036942_memory_.zstd
GemsFDTD/6129/_6129_0.036578_memory_.zstd
GemsFDTD/39321/_39321_0.024885_memory_.zstd
GemsFDTD/10638/_10638_0.007679_memory_.zstd
milc/19334/_19334_0.071922_memory_.zstd
milc/13856/_13856_0.015332_memory_.zstd
soplex_pds-50/16563/_16563_0.113417_memory_.zstd
soplex_pds-50/2921/_2921_0.076168_memory_.zstd
soplex_pds-50/10739/_10739_0.058110_memory_.zstd
sphinx3/103451/_103451_0.082038_memory_.zstd
sphinx3/25193/_25193_0.047030_memory_.zstd
zeusmp/18126/_18126_0.069599_memory_.zstd
zeusmp/54211/_54211_0.028691_memory_.zstd
zeusmp/7545/_7545_0.060884_memory_.zstd
omnetpp/6881/_6881_0.962556_memory_.zstd
leslie3d/28053/_28053_0.163166_memory_.zstd
"""
CUSTOM_CHECKPOINTS = ",".join(
    f"{_CHECKPOINT_ROOT}/{checkpoint}"
    for checkpoint in _CUSTOM_CHECKPOINTS.splitlines()
    if checkpoint
)

# Extracted from /nfs/home/lijiangtao/tools/gem5_data_proc/simpoint_cpt/
# resources/checkpoints_all.json (SHA-256 b9d0c16f04791fe29bc8badd535defffb908b5be2498aca6cb7ea9206698c8a7).
# The order is identical to _CUSTOM_CHECKPOINTS, so the solver can use the
# multi-custom-bin workload order as a stable slice-to-weight mapping in CI.
CUSTOM_CHECKPOINT_WEIGHTS = (
    0.148205,
    0.144497,
    0.187251,
    0.0056679,
    0.0520644,
    0.0527474,
    0.00925612,
    0.0330737,
    0.0195004,
    0.0793543,
    0.164923,
    0.0941396,
    0.111046,
    0.00293469,
    0.00889506,
    0.00805332,
    0.0369422,
    0.0365785,
    0.0248846,
    0.0076794,
    0.0719216,
    0.0153323,
    0.113417,
    0.0761684,
    0.0581101,
    0.082038,
    0.0470301,
    0.069599,
    0.0286905,
    0.0608844,
    0.962556,
    0.163166,
)


def _set_param(root, target: str, value: int) -> None:
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


def _set_both(root, parameter: str, value: int) -> None:
    for target in _BOP_TARGETS:
        _set_param(root, f"{target}.{parameter}", value)


class BOPCQFDseNsga2(SolveSpec):
    config_path = "configs/example/kmhv3.py"
    benchmark_type = "custom_bin"
    specific_benchmarks = ""
    custom_bin = CUSTOM_CHECKPOINTS
    custom_bin_weights = CUSTOM_CHECKPOINT_WEIGHTS
    extra_args = (
        "--enable-bop-direct-quality-gate "
        "--bop-direct-quality-profile=bop-cqf-dse "
        "--difftest-ref-so="
        "/nfs/home/share/gem5_ci/ref/normal/"
        "riscv64-nemu-notama-tvalref-so"
    )
    solver_name = "nsga2"
    summary_top_n = 24

    quality_entries = TunableParam.Unsigned(
        domain=Choice([64, 128, 256]), default=256
    )
    feedback_entries = TunableParam.Unsigned(
        domain=Choice([64, 128, 256]), default=256
    )
    unused_per_useful = TunableParam.Unsigned(
        domain=Choice([2, 4, 6, 8, 10, 12, 16, 20]), default=10
    )
    min_samples = TunableParam.Unsigned(
        domain=Choice([16, 32, 64]), default=32
    )
    sample_period = TunableParam.Unsigned(
        domain=Choice([8, 16, 32]), default=16
    )
    block_guard = TunableParam.Unsigned(
        domain=Choice([2, 4, 8]), default=4
    )
    strict_probe_period = TunableParam.Unsigned(
        domain=Choice([32, 64, 128]), default=64
    )
    reopen_confirm_samples = TunableParam.Unsigned(
        domain=Choice([0, 4, 8, 16]), default=0
    )
    decay_period = TunableParam.Unsigned(
        domain=Choice([32, 64, 128]), default=64
    )
    # 0/1/2 select E7/S5, E6/S6, and E5/S7. The timeout depends on
    # Feedback capacity so no trial exceeds the nominal 2048-demand horizon.
    epoch_variant = TunableParam.Unsigned(
        domain=Choice([0, 1, 2]), default=1
    )

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=256, timeout_hours=46)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        quality_entries = int(trial.quality_entries)
        feedback_entries = int(trial.feedback_entries)
        ratio = int(trial.unused_per_useful)
        guard = int(trial.block_guard)
        strict_probe = int(trial.strict_probe_period)

        _set_both(root, "direct_quality_entries", quality_entries)
        _set_both(root, "direct_quality_feedback_entries", feedback_entries)
        _set_both(root, "direct_quality_min_samples", int(trial.min_samples))
        _set_both(root, "direct_quality_observe_sample_period", int(trial.sample_period))
        _set_both(root, "direct_quality_open_sample_period", int(trial.sample_period))
        _set_both(root, "direct_quality_unused_per_useful", ratio)
        _set_both(root, "direct_quality_strict_unused_per_useful", 2 * ratio)
        _set_both(root, "direct_quality_reopen_unused_per_useful", ratio)
        _set_both(root, "direct_quality_block_guard", guard)
        _set_both(root, "direct_quality_strict_block_guard", guard)
        _set_both(root, "direct_quality_reopen_guard", guard)
        _set_both(root, "direct_quality_block_probe_period", strict_probe)
        _set_both(root, "direct_quality_borderline_block_probe_period", strict_probe // 8)
        _set_both(root, "direct_quality_reopen_probe_period", strict_probe)
        _set_both(
            root,
            "direct_quality_reopen_confirm_samples",
            int(trial.reopen_confirm_samples),
        )
        _set_both(root, "direct_quality_decay_period", int(trial.decay_period))

        variant = int(trial.epoch_variant)
        if variant == 0:
            bits, shift = 7, 5
            timeout = {64: 63, 128: 62, 256: 60}[feedback_entries]
        elif variant == 1:
            bits, shift = 6, 6
            timeout = {64: 31, 128: 31, 256: 30}[feedback_entries]
        elif variant == 2:
            bits, shift, timeout = 5, 7, 15
        else:
            raise ValueError(f"unsupported CQF epoch variant {variant}")

        _set_both(root, "direct_quality_epoch_bits", bits)
        _set_both(root, "direct_quality_epoch_shift", shift)
        _set_both(root, "direct_quality_epoch_timeout", timeout)

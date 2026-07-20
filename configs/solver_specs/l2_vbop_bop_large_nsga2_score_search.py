from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import (
    Choice,
    InferTunable,
    Maximize,
    Minimize,
    Range,
    SolveSpec,
    Stop,
    TunableParam,
)


_BOP_LARGE = "system.l2_wrappers[0].prefetcher.bop_large"
_SCORE_RATIO_VALUES = [60, 70, 80]

_OFFSETS_WITH_ONE = [
    -117,
    -147,
    -91,
    117,
    147,
    91,
    -256,
    -250,
    -243,
    -240,
    -225,
    -216,
    -200,
    -192,
    -180,
    -162,
    -160,
    -150,
    -144,
    -135,
    -128,
    -125,
    -120,
    -108,
    -100,
    -96,
    -90,
    -81,
    -80,
    -75,
    -72,
    -64,
    -60,
    -54,
    -50,
    -48,
    -45,
    -40,
    -36,
    -32,
    -30,
    -27,
    -25,
    -24,
    -20,
    -18,
    -16,
    -15,
    -12,
    -10,
    -9,
    -8,
    -6,
    -5,
    -4,
    -3,
    -2,
    -1,
    1,
    2,
    3,
    4,
    5,
    6,
    8,
    9,
    10,
    12,
    15,
    16,
    18,
    20,
    24,
    25,
    27,
    30,
    32,
    36,
    40,
    45,
    48,
    50,
    54,
    60,
    64,
    72,
    75,
    80,
    81,
    90,
    96,
    100,
    108,
    120,
    125,
    128,
    135,
    144,
    150,
    160,
    162,
    180,
    192,
    200,
    216,
    225,
    240,
    243,
    250,
]
_OFFSETS_WITHOUT_ONE = [
    offset for offset in _OFFSETS_WITH_ONE if abs(offset) != 1
]


def _set_param(root, target, value):
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


class L2VbopBopLargeNsga2ScoreSearch(SolveSpec):
    config_path = "configs/example/kmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = ""
    extra_args = ""
    solver_name = "nsga2"
    summary_top_n = 16

    round_max = InferTunable(
        target=f"{_BOP_LARGE}.round_max",
        domain=Range(30, 80),
    )

    score_ratio = TunableParam.Unsigned(
        domain=Choice(_SCORE_RATIO_VALUES),
        default=60,
    )

    bad_score = InferTunable(
        target=f"{_BOP_LARGE}.bad_score",
        domain=Range(1, 8),
    )

    rr_size = InferTunable(
        target=f"{_BOP_LARGE}.rr_size",
        domain=Choice([128, 256, 512, 1024]),
    )

    tag_bits = InferTunable(
        target=f"{_BOP_LARGE}.tag_bits",
        domain=Choice([4, 8, 12, 16]),
    )

    delay_queue_size = InferTunable(
        target=f"{_BOP_LARGE}.delay_queue_size",
        domain=Choice([16, 32]),
    )

    delay_queue_cycles = InferTunable(
        target=f"{_BOP_LARGE}.delay_queue_cycles",
        domain=Range(180, 350, step=20),
    )

    offsets = InferTunable(
        target=f"{_BOP_LARGE}.offsets",
        domain=Choice([_OFFSETS_WITH_ONE, _OFFSETS_WITHOUT_ONE]),
    )

    objectives = [
        Maximize.score_txt("Estimated Int score per GHz"),
        Maximize.stats("system.l2_wrappers.prefetcher.accuracy"),
        Maximize.stats("system.l2_wrappers.prefetcher.coverage"),
        Minimize.stats(
            "system.l2_wrappers.slices0.inner_cache.ReadSharedReq.missRate::cpu.data"
        ),
        Minimize.stats(
            "system.l2_wrappers.slices1.inner_cache.ReadSharedReq.missRate::cpu.data"
        ),
        Minimize.stats(
            "system.l2_wrappers.slices2.inner_cache.ReadSharedReq.missRate::cpu.data"
        ),
        Minimize.stats(
            "system.l2_wrappers.slices3.inner_cache.ReadSharedReq.missRate::cpu.data"
        ),
    ]

    stop = Stop(max_trials=4000, no_improve_trials=20, timeout_hours=30)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        score_max = int(trial.round_max) * int(trial.score_ratio) // 100
        _set_param(root, f"{_BOP_LARGE}.score_max", score_max)

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


_PHAST = "system.cpu[0]"

# Use balanced coverage of the requested benchmark families: the three
# highest-weight perlbench slices, three highest-weight GCC slices, and four
# highest-weight povray slices. This deliberately avoids letting GCC crowd out
# povray solely because its individual SimPoint weights are larger.
_TOP_WEIGHTED_SLICES = (
    "perlbench_splitmail_3995,"
    "perlbench_diffmail_6951,"
    "gcc_s04_2772,"
    "perlbench_diffmail_16743,"
    "gcc_scilab_2417,"
    "gcc_cpdecl_2025,"
    "povray_8362,"
    "povray_10009,"
    "povray_34248,"
    "povray_8711"
)


def _set_param(root, target: str, value) -> None:
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


def _history_lengths(count: int, picks: list[int]) -> list[int]:
    """Map unconstrained picks to a strictly increasing PHAST history vector.

    Removing a selected item from the pool guarantees that the nonzero entries
    are distinct. Sorting them then satisfies PHAST's required strictly
    increasing order. Every vector with first element zero, 6--10 elements,
    and maximum at most 96 remains reachable.
    """

    if not 6 <= count <= 10:
        raise ValueError(f"PHAST history length count must be in [6, 10], got {count}")

    available = list(range(1, 97))
    selected = []
    for pick in picks[: count - 1]:
        selected.append(available.pop(int(pick) % len(available)))
    history_lengths = [0, *sorted(selected)]
    if any(
        left >= right
        for left, right in zip(history_lengths, history_lengths[1:])
    ):
        raise ValueError("PHAST history lengths must be strictly increasing")
    return history_lengths


class PHASTMDPNsga2ScoreMemViolationSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = _TOP_WEIGHTED_SLICES
    extra_args = ""
    solver_name = "nsga2"
    summary_top_n = 16

    phast_num_rows = InferTunable(
        target=f"{_PHAST}.phast_num_rows",
        domain=Choice([32, 64, 128, 256]),
    )

    phast_associativity = InferTunable(
        target=f"{_PHAST}.phast_associativity",
        domain=Range(2, 4),
    )

    phast_tag_bits = InferTunable(
        target=f"{_PHAST}.phast_tag_bits",
        domain=Range(8, 24),
    )

    phast_max_counter = InferTunable(
        target=f"{_PHAST}.phast_max_counter",
        domain=Range(8, 32),
    )

    # The following indexes are mapped in apply_trial() because their legal
    # ranges depend on phast_max_counter. Modulo mapping makes every legal
    # counter value reachable while preventing invalid PHAST configurations.
    counter_threshold_index = TunableParam.Unsigned(domain=Range(0, 31))
    counter_increment_index = TunableParam.Unsigned(domain=Range(0, 31))
    counter_decrement_index = TunableParam.Unsigned(domain=Range(0, 32))

    phast_selected_target_bits = InferTunable(
        target=f"{_PHAST}.phast_selected_target_bits",
        domain=Range(2, 8),
    )

    phast_history_length_count = TunableParam.Unsigned(domain=Range(6, 10))
    history_pick_0 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_1 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_2 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_3 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_4 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_5 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_6 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_7 = TunableParam.Unsigned(domain=Range(0, 95))
    history_pick_8 = TunableParam.Unsigned(domain=Range(0, 95))

    phast_second_target_max_distance = InferTunable(
        target=f"{_PHAST}.phast_second_target_max_distance",
        domain=Choice([0, 16, 32, 48, 80, 96, 128]),
    )

    objectives = [
        Maximize.score_txt("Estimated overall score per GHz"),
        Minimize.stats("system.cpu.iew.memOrderViolationEvents"),
        Minimize.stats("system.cpu.MemDepUnit__0.mdpFalseDepAtCommit"),
    ]

    # Run until the trial budget or wall-clock budget is exhausted. In a
    # multi-objective search, no_improve_trials measures Pareto-frontier
    # changes rather than raw metric changes, so it can stop while every
    # trial's metric values are still different.
    stop = Stop(max_trials=4000, timeout_hours=7)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        max_counter = int(trial.phast_max_counter)
        counter_threshold = 1 + int(trial.counter_threshold_index) % max_counter
        if not 1 <= counter_threshold <= max_counter:
            raise ValueError(
                "PHAST counter threshold must be in [1, phast_max_counter]"
            )
        _set_param(
            root,
            f"{_PHAST}.phast_counter_threshold",
            counter_threshold,
        )
        _set_param(
            root,
            f"{_PHAST}.phast_counter_increment",
            int(trial.counter_increment_index) % max_counter,
        )
        _set_param(
            root,
            f"{_PHAST}.phast_counter_decrement",
            int(trial.counter_decrement_index) % (max_counter + 1),
        )

        picks = [
            int(trial.history_pick_0),
            int(trial.history_pick_1),
            int(trial.history_pick_2),
            int(trial.history_pick_3),
            int(trial.history_pick_4),
            int(trial.history_pick_5),
            int(trial.history_pick_6),
            int(trial.history_pick_7),
            int(trial.history_pick_8),
        ]
        _set_param(
            root,
            f"{_PHAST}.phast_history_lengths",
            _history_lengths(int(trial.phast_history_length_count), picks),
        )

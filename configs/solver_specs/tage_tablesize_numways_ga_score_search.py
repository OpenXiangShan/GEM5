from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import Choice, Maximize, SolveSpec, Stop, TunableParam


_BASELINE_TABLE_SIZES = [2048, 2048, 8192, 8192, 8192, 8192, 8192, 2048]
_BASELINE_NUM_WAYS = [2, 2, 4, 2, 2, 2, 2, 2]
_TABLE_SIZE_VALUES = [256, 512, 1024, 2048, 4096, 8192]
_TABLE_SIZE_TOTAL = sum(_BASELINE_TABLE_SIZES)
_NUM_TABLES = len(_BASELINE_TABLE_SIZES)


def _table_size_candidates():
    candidates = []

    def dfs(prefix, depth, remaining):
        slots_left = _NUM_TABLES - depth
        min_possible = _TABLE_SIZE_VALUES[0] * slots_left
        max_possible = _TABLE_SIZE_VALUES[-1] * slots_left
        if remaining < min_possible or remaining > max_possible:
            return
        if depth == _NUM_TABLES:
            if remaining == 0:
                candidates.append(list(prefix))
            return
        tail_slots = slots_left - 1
        for value in _TABLE_SIZE_VALUES:
            next_remaining = remaining - value
            min_tail = _TABLE_SIZE_VALUES[0] * tail_slots
            max_tail = _TABLE_SIZE_VALUES[-1] * tail_slots
            if next_remaining < min_tail or next_remaining > max_tail:
                continue
            prefix.append(value)
            dfs(prefix, depth + 1, next_remaining)
            prefix.pop()

    dfs([], 0, _TABLE_SIZE_TOTAL)
    return candidates


class TageTableSizeNumWaysGaScoreSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "gcc15-spec06-0.3c"
    specific_benchmarks = "astar,gobmk,sjeng"
    extra_args = ""
    solver_name = "ga"
    summary_top_n = 16

    tableSizes = TunableParam.VectorUnsigned(
        target="system.cpu[0].branchPred.tage.tableSizes",
        domain=Choice(_table_size_candidates()),
        default=_BASELINE_TABLE_SIZES,
    )

    numWays0 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)
    numWays1 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)
    numWays2 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=4)
    numWays3 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)
    numWays4 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)
    numWays5 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)
    numWays6 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)
    numWays7 = TunableParam.Unsigned(domain=Choice([1, 2, 3, 4, 5, 6, 7, 8]), default=2)

    objective = Maximize.score_txt("Estimated Int score per GHz")
    stop = Stop(max_trials=4000, timeout_hours=30)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        owner, _, param_name = resolve_target(
            root,
            "system.cpu[0].branchPred.tage.numWays",
        )
        num_ways = [
            int(trial.numWays0),
            int(trial.numWays1),
            int(trial.numWays2),
            int(trial.numWays3),
            int(trial.numWays4),
            int(trial.numWays5),
            int(trial.numWays6),
            int(trial.numWays7),
        ]
        owner.numWays = owner._params[param_name].convert(num_ways)

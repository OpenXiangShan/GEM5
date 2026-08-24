from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import Maximize, SolveSpec, Stop, TunableParam
from util.solver.spec.domain import DomainError


_BASELINE_TABLE_SIZES = [2048, 2048, 8192, 8192, 8192, 8192, 8192, 2048]
_BASELINE_NUM_WAYS = [2, 2, 4, 2, 2, 2, 2, 2]
_TABLE_SIZE_VALUES = [256, 512, 1024, 2048, 4096, 8192]
_NUM_WAY_VALUES = [1, 2, 3, 4, 5, 6, 7, 8]
_NUM_TABLES = len(_BASELINE_TABLE_SIZES)
_BASELINE_TOTAL_SIZE = sum(
    table_size * num_ways
    for table_size, num_ways in zip(_BASELINE_TABLE_SIZES, _BASELINE_NUM_WAYS)
)


def _tage_total_size(table_sizes, num_ways):
    return sum(
        int(table_size) * int(num_ways)
        for table_size, num_ways in zip(table_sizes, num_ways)
    )


class _FixedTotalTageConfigDomain:
    kind = "fixed_total_tage_config"

    def __init__(
        self,
        *,
        table_size_values,
        num_way_values,
        num_tables,
        target_total_size,
    ):
        self.table_size_values = tuple(int(value) for value in table_size_values)
        self.num_way_values = tuple(int(value) for value in num_way_values)
        self.num_tables = int(num_tables)
        self.target_total_size = int(target_total_size)
        self._options = tuple(
            (table_size, num_way, table_size * num_way)
            for table_size in self.table_size_values
            for num_way in self.num_way_values
        )
        self._suffix_counts = self._build_suffix_counts()
        self._cardinality = self._suffix_counts[0].get(self.target_total_size, 0)
        if self._cardinality <= 0:
            raise DomainError("fixed-total TAGE config domain is empty")

    def _build_suffix_counts(self):
        suffix_counts = [None] * (self.num_tables + 1)
        suffix_counts[self.num_tables] = {0: 1}
        for depth in range(self.num_tables - 1, -1, -1):
            counts = {}
            for suffix_total, suffix_count in suffix_counts[depth + 1].items():
                for _, _, capacity in self._options:
                    total = suffix_total + capacity
                    if total > self.target_total_size:
                        continue
                    counts[total] = counts.get(total, 0) + suffix_count
            suffix_counts[depth] = counts
        return suffix_counts

    def cardinality(self):
        return self._cardinality

    def iter_values(self):
        raise DomainError(
            "fixed-total TAGE config domain is too large to enumerate; "
            "use ga or random sampling"
        )

    def _sample_suffix(self, rng, depth, remaining):
        table_sizes = []
        num_ways = []
        if depth == self.num_tables:
            if remaining == 0:
                return table_sizes, num_ways
            raise DomainError("fixed-total TAGE prefix is not completable")

        for current_depth in range(depth, self.num_tables):
            weighted_options = []
            total_weight = 0
            suffix_counts = self._suffix_counts[current_depth + 1]
            for table_size, num_way, capacity in self._options:
                if capacity > remaining:
                    continue
                suffix_count = suffix_counts.get(remaining - capacity, 0)
                if suffix_count <= 0:
                    continue
                weighted_options.append((table_size, num_way, capacity, suffix_count))
                total_weight += suffix_count
            if total_weight <= 0:
                raise DomainError("failed to sample fixed-total TAGE config")
            pick = rng.randrange(total_weight)
            for table_size, num_way, capacity, weight in weighted_options:
                if pick < weight:
                    table_sizes.append(table_size)
                    num_ways.append(num_way)
                    remaining -= capacity
                    break
                pick -= weight
        if remaining != 0:
            raise DomainError("sampled TAGE suffix does not preserve total size")
        return table_sizes, num_ways

    def _complete_prefix(self, rng, table_sizes, num_ways):
        depth = len(table_sizes)
        if depth != len(num_ways):
            raise DomainError("TAGE tableSizes and numWays prefix lengths differ")
        remaining = self.target_total_size - _tage_total_size(table_sizes, num_ways)
        suffix_table_sizes, suffix_num_ways = self._sample_suffix(rng, depth, remaining)
        return (
            list(table_sizes) + suffix_table_sizes,
            list(num_ways) + suffix_num_ways,
        )

    def sample(self, rng):
        table_sizes, num_ways = self._sample_suffix(rng, 0, self.target_total_size)
        return table_sizes + num_ways

    def mutate(self, rng, value):
        table_sizes, num_ways = _split_tage_config(value)
        keep_count = rng.randrange(self.num_tables)
        table_sizes, num_ways = self._complete_prefix(
            rng,
            table_sizes[:keep_count],
            num_ways[:keep_count],
        )
        return table_sizes + num_ways

    def crossover(self, rng, left, right):
        left_table_sizes, left_num_ways = _split_tage_config(left)
        right_table_sizes, right_num_ways = _split_tage_config(right)
        split = rng.randrange(1, self.num_tables)
        child_a_table_sizes, child_a_num_ways = self._complete_prefix(
            rng,
            left_table_sizes[:split],
            left_num_ways[:split],
        )
        child_b_table_sizes, child_b_num_ways = self._complete_prefix(
            rng,
            right_table_sizes[:split],
            right_num_ways[:split],
        )
        return (
            child_a_table_sizes + child_a_num_ways,
            child_b_table_sizes + child_b_num_ways,
        )

    def to_dict(self):
        return {
            "kind": self.kind,
            "table_size_values": list(self.table_size_values),
            "num_way_values": list(self.num_way_values),
            "num_tables": self.num_tables,
            "target_total_size": self.target_total_size,
            "cardinality": self.cardinality(),
            "encoding": (
                "first num_tables entries are tableSizes; "
                "remaining entries are numWays"
            ),
        }


def _split_tage_config(config):
    values = [int(value) for value in config]
    expected_length = _NUM_TABLES * 2
    if len(values) != expected_length:
        raise ValueError(
            f"tableSizesNumWays must contain {expected_length} values, got {len(values)}"
        )
    table_sizes = values[:_NUM_TABLES]
    num_ways = values[_NUM_TABLES:]
    total_size = _tage_total_size(table_sizes, num_ways)
    if total_size != _BASELINE_TOTAL_SIZE:
        raise ValueError(
            f"TAGE total size must be {_BASELINE_TOTAL_SIZE}, got {total_size}"
        )
    return table_sizes, num_ways


_TAGE_CONFIG_DOMAIN = _FixedTotalTageConfigDomain(
    table_size_values=_TABLE_SIZE_VALUES,
    num_way_values=_NUM_WAY_VALUES,
    num_tables=_NUM_TABLES,
    target_total_size=_BASELINE_TOTAL_SIZE,
)


class TageTableSizeNumWaysGaScoreSearch(SolveSpec):
    config_path = "configs/example/idealkmhv3.py"
    benchmark_type = "spec06-rva23-novec-gcc16-0.3c"
    specific_benchmarks = "astar,gobmk,sjeng"
    extra_args = ""
    solver_name = "ga"
    summary_top_n = 16

    tableSizesNumWays = TunableParam.VectorUnsigned(
        domain=_TAGE_CONFIG_DOMAIN,
        default=_BASELINE_TABLE_SIZES + _BASELINE_NUM_WAYS,
    )

    objective = Maximize.score_txt("Estimated Int score per GHz")
    stop = Stop(max_trials=4000, timeout_hours=30)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        table_sizes, num_ways = _split_tage_config(trial.tableSizesNumWays)
        for target, values in (
            ("system.cpu[0].branchPred.tage.tableSizes", table_sizes),
            ("system.cpu[0].branchPred.tage.numWays", num_ways),
        ):
            owner, _, param_name = resolve_target(root, target)
            setattr(owner, param_name, owner._params[param_name].convert(values))

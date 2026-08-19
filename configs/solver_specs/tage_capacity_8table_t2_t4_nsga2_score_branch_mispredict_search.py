from collections import Counter, defaultdict

from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import Maximize, Minimize, SolveSpec, Stop, TunableParam
from util.solver.spec.domain import DomainError


# An active BTBTAGE entry contains a valid bit, the configurable tag, a 3-bit
# direction counter, and a useful bit.  Stored PC and LRU helper state are not
# in the active lookup/replacement data path, so this is a logical SRAM budget.
_FIXED_ENTRY_BITS = 5
_NUM_PREDICTORS = 8
_FOCUS_TABLE_INDICES = (2, 3, 4)
_NON_FOCUS_TABLE_INDICES = (0, 1, 5, 6, 7)
_FOCUS_MIN_SHARE_PERCENT = 50

_BASELINE_TABLE_SIZES = [2048] * _NUM_PREDICTORS
_BASELINE_TAG_BITS = [13] * _NUM_PREDICTORS
_BASELINE_NUM_WAYS = [2] * _NUM_PREDICTORS

# The prior is deliberately expressed through table-size choices.  T2-T4 get
# the complete high-set range; the remaining tables retain a meaningful range
# but cannot consume capacity through an 4K/8K set count alone.
_FOCUS_TABLE_SIZE_VALUES = [1 << exponent for exponent in range(7, 14)]
_NON_FOCUS_TABLE_SIZE_VALUES = [1 << exponent for exponent in range(6, 12)]
_TAG_BIT_VALUES = list(range(8, 21))
_NUM_WAY_VALUES = list(range(1, 9))

# Sampling keeps the full legal tag/way domain but centers initial candidates
# around capacity combinations compatible with the 80%-130% total budget.
_FOCUS_TABLE_SIZE_WEIGHTS = [1, 2, 4, 8, 12, 8, 2]
_NON_FOCUS_TABLE_SIZE_WEIGHTS = [1, 2, 4, 8, 10, 5]
_TAG_BIT_WEIGHTS = [1, 2, 3, 5, 7, 9, 10, 9, 7, 5, 3, 2, 1]
_NUM_WAY_WEIGHTS = [8, 12, 8, 4, 2, 1, 1, 1]


def _tage_capacity_bits(table_sizes, tag_bits, num_ways):
    return sum(
        int(table_size) * int(tag_bit_size + _FIXED_ENTRY_BITS) * int(num_way)
        for table_size, tag_bit_size, num_way in zip(
            table_sizes, tag_bits, num_ways
        )
    )


_BASELINE_CAPACITY_BITS = _tage_capacity_bits(
    _BASELINE_TABLE_SIZES,
    _BASELINE_TAG_BITS,
    _BASELINE_NUM_WAYS,
)
_MIN_CAPACITY_BITS = (_BASELINE_CAPACITY_BITS * 80 + 99) // 100
_MAX_CAPACITY_BITS = _BASELINE_CAPACITY_BITS * 130 // 100


def _table_size_values(index):
    if index in _FOCUS_TABLE_INDICES:
        return _FOCUS_TABLE_SIZE_VALUES
    return _NON_FOCUS_TABLE_SIZE_VALUES


def _tage_focus_capacity_bits(table_sizes, tag_bits, num_ways):
    return sum(
        int(table_sizes[index])
        * int(tag_bits[index] + _FIXED_ENTRY_BITS)
        * int(num_ways[index])
        for index in _FOCUS_TABLE_INDICES
    )


def _encode_tage_config(table_sizes, tag_bits, num_ways):
    return list(table_sizes) + list(tag_bits) + list(num_ways)


def _decode_tage_config(config, *, require_focus=False):
    values = [int(value) for value in config]
    expected_length = _NUM_PREDICTORS * 3
    if len(values) != expected_length:
        raise ValueError(
            f"8-table TAGE configuration must contain {expected_length} values, "
            f"got {len(values)}"
        )
    table_sizes = values[:_NUM_PREDICTORS]
    tag_bits = values[_NUM_PREDICTORS : 2 * _NUM_PREDICTORS]
    num_ways = values[2 * _NUM_PREDICTORS :]
    if any(
        table_size not in _table_size_values(index)
        for index, table_size in enumerate(table_sizes)
    ):
        raise ValueError(f"invalid 8-table TAGE table sizes: {table_sizes}")
    if any(value not in _TAG_BIT_VALUES for value in tag_bits):
        raise ValueError(f"invalid TAGE tag widths: {tag_bits}")
    if any(value not in _NUM_WAY_VALUES for value in num_ways):
        raise ValueError(f"invalid TAGE associativities: {num_ways}")

    capacity_bits = _tage_capacity_bits(table_sizes, tag_bits, num_ways)
    if not _MIN_CAPACITY_BITS <= capacity_bits <= _MAX_CAPACITY_BITS:
        raise ValueError(
            f"TAGE capacity must be in [{_MIN_CAPACITY_BITS}, "
            f"{_MAX_CAPACITY_BITS}] bit, got {capacity_bits} bit"
        )
    focus_capacity_bits = _tage_focus_capacity_bits(
        table_sizes, tag_bits, num_ways
    )
    if (
        require_focus
        and focus_capacity_bits * 100
        < capacity_bits * _FOCUS_MIN_SHARE_PERCENT
    ):
        raise ValueError(
            "T2-T4 must consume at least "
            f"{_FOCUS_MIN_SHARE_PERCENT}% of TAGE capacity, got "
            f"{focus_capacity_bits}/{capacity_bits} bit"
        )
    return table_sizes, tag_bits, num_ways, capacity_bits, focus_capacity_bits


class _CapacityConstrainedTage8TableT2T4Domain:
    kind = "capacity_constrained_tage_8table_t2_t4"

    def __init__(self):
        self._cardinality = None

    def _random_table(self, rng, index):
        if index in _FOCUS_TABLE_INDICES:
            table_size = rng.choices(
                _FOCUS_TABLE_SIZE_VALUES,
                weights=_FOCUS_TABLE_SIZE_WEIGHTS,
                k=1,
            )[0]
        else:
            table_size = rng.choices(
                _NON_FOCUS_TABLE_SIZE_VALUES,
                weights=_NON_FOCUS_TABLE_SIZE_WEIGHTS,
                k=1,
            )[0]
        return (
            table_size,
            rng.choices(_TAG_BIT_VALUES, weights=_TAG_BIT_WEIGHTS, k=1)[0],
            rng.choices(_NUM_WAY_VALUES, weights=_NUM_WAY_WEIGHTS, k=1)[0],
        )

    def _is_valid(self, config):
        try:
            _decode_tage_config(config, require_focus=True)
        except ValueError:
            return False
        return True

    def sample(self, rng):
        for _ in range(16384):
            tables = [self._random_table(rng, index) for index in range(_NUM_PREDICTORS)]
            table_sizes, tag_bits, num_ways = map(list, zip(*tables))
            config = _encode_tage_config(table_sizes, tag_bits, num_ways)
            if self._is_valid(config):
                return config
        raise DomainError("failed to sample a focused capacity-constrained TAGE config")

    def mutate(self, rng, value):
        table_sizes, tag_bits, num_ways, _, _ = _decode_tage_config(
            value, require_focus=True
        )
        for _ in range(4096):
            next_table_sizes = list(table_sizes)
            next_tag_bits = list(tag_bits)
            next_num_ways = list(num_ways)
            selection = rng.random()
            if selection < 0.65:
                # Make T2-T4 set allocation the dominant local search action.
                table = rng.choice(_FOCUS_TABLE_INDICES)
                next_table_sizes[table] = rng.choice(_FOCUS_TABLE_SIZE_VALUES)
            elif selection < 0.85:
                table = rng.choice(_FOCUS_TABLE_INDICES)
                if rng.random() < 0.5:
                    next_tag_bits[table] = rng.choice(_TAG_BIT_VALUES)
                else:
                    next_num_ways[table] = rng.choice(_NUM_WAY_VALUES)
            else:
                table = rng.choice(_NON_FOCUS_TABLE_INDICES)
                knob = rng.randrange(3)
                if knob == 0:
                    next_table_sizes[table] = rng.choice(_NON_FOCUS_TABLE_SIZE_VALUES)
                elif knob == 1:
                    next_tag_bits[table] = rng.choice(_TAG_BIT_VALUES)
                else:
                    next_num_ways[table] = rng.choice(_NUM_WAY_VALUES)
            config = _encode_tage_config(
                next_table_sizes, next_tag_bits, next_num_ways
            )
            if config != list(value) and self._is_valid(config):
                return config
        return self.sample(rng)

    def crossover(self, rng, left, right):
        return self._crossover_one(rng, left, right), self._crossover_one(
            rng, right, left
        )

    def _crossover_one(self, rng, preferred, alternate):
        preferred_sizes, preferred_tags, preferred_ways, _, _ = _decode_tage_config(
            preferred, require_focus=True
        )
        alternate_sizes, alternate_tags, alternate_ways, _, _ = _decode_tage_config(
            alternate, require_focus=True
        )
        for _ in range(4096):
            table_sizes = []
            tag_bits = []
            num_ways = []
            for index in range(_NUM_PREDICTORS):
                if rng.random() < 0.5:
                    table_sizes.append(alternate_sizes[index])
                    tag_bits.append(alternate_tags[index])
                    num_ways.append(alternate_ways[index])
                else:
                    table_sizes.append(preferred_sizes[index])
                    tag_bits.append(preferred_tags[index])
                    num_ways.append(preferred_ways[index])
            config = _encode_tage_config(table_sizes, tag_bits, num_ways)
            if self._is_valid(config):
                return config
        return self.sample(rng)

    def iter_values(self):
        raise DomainError(
            "focused capacity-constrained TAGE configs are too large to enumerate; "
            "use nsga2 or random sampling"
        )

    @staticmethod
    def _capacity_distribution(table_indices, maximum_units):
        capacities = {0: 1}
        for index in table_indices:
            option_counts = Counter(
                table_size // 64 * (tag_bits + _FIXED_ENTRY_BITS) * num_way
                for table_size in _table_size_values(index)
                for tag_bits in _TAG_BIT_VALUES
                for num_way in _NUM_WAY_VALUES
            )
            next_capacities = defaultdict(int)
            for previous_capacity, multiplicity in capacities.items():
                for option_capacity, option_count in option_counts.items():
                    capacity = previous_capacity + option_capacity
                    if capacity <= maximum_units:
                        next_capacities[capacity] += multiplicity * option_count
            capacities = next_capacities
        return capacities

    def cardinality(self):
        if self._cardinality is not None:
            return self._cardinality

        # Every capacity is a 64-bit multiple.  Split the DP into focused and
        # non-focused tables, then use prefix sums to impose both total-capacity
        # and T2-T4-share constraints without enumerating configurations.
        minimum_units = (_MIN_CAPACITY_BITS + 63) // 64
        maximum_units = _MAX_CAPACITY_BITS // 64
        focus = self._capacity_distribution(_FOCUS_TABLE_INDICES, maximum_units)
        non_focus = self._capacity_distribution(
            _NON_FOCUS_TABLE_INDICES, maximum_units
        )
        prefix = [0] * (maximum_units + 2)
        for capacity, multiplicity in non_focus.items():
            prefix[capacity + 1] += multiplicity
        for index in range(1, len(prefix)):
            prefix[index] += prefix[index - 1]

        total = 0
        for focus_capacity, multiplicity in focus.items():
            lower = max(0, minimum_units - focus_capacity)
            upper = min(
                maximum_units - focus_capacity,
                focus_capacity,
            )
            if lower <= upper:
                total += multiplicity * (prefix[upper + 1] - prefix[lower])
        self._cardinality = total
        return total

    def to_dict(self):
        return {
            "kind": self.kind,
            "num_predictors": _NUM_PREDICTORS,
            "focus_tables": list(_FOCUS_TABLE_INDICES),
            "focus_min_share_percent": _FOCUS_MIN_SHARE_PERCENT,
            "focus_table_size_values": _FOCUS_TABLE_SIZE_VALUES,
            "non_focus_table_size_values": _NON_FOCUS_TABLE_SIZE_VALUES,
            "tag_bit_values": _TAG_BIT_VALUES,
            "num_way_values": _NUM_WAY_VALUES,
            "fixed_entry_bits": _FIXED_ENTRY_BITS,
            "baseline_capacity_bits": _BASELINE_CAPACITY_BITS,
            "min_capacity_bits": _MIN_CAPACITY_BITS,
            "max_capacity_bits": _MAX_CAPACITY_BITS,
            "encoding": (
                "tableSizes(T0-T7), then TTagBitSizes(T0-T7), then "
                "numWays(T0-T7); numPredictors is fixed at 8"
            ),
        }


_TAGE_CONFIG_DOMAIN = _CapacityConstrainedTage8TableT2T4Domain()
_BASELINE_CONFIG = _encode_tage_config(
    _BASELINE_TABLE_SIZES,
    _BASELINE_TAG_BITS,
    _BASELINE_NUM_WAYS,
)


def _set_param(root, target, value):
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


class TageCapacity8TableT2T4Nsga2ScoreBranchMispredictSearch(SolveSpec):
    config_path = "configs/example/kmhv3.py"
    benchmark_type = "gcc15-spec06-tage-sensitive-0.3c-260604"
    specific_benchmarks = ""
    extra_args = ""
    solver_name = "nsga2"
    summary_top_n = 16

    tageConfig = TunableParam.VectorUnsigned(
        domain=_TAGE_CONFIG_DOMAIN,
        default=_BASELINE_CONFIG,
    )

    objectives = [
        Maximize.score_txt("Estimated Int score per GHz"),
        Minimize.stats("system.cpu.iew.branchMispredicts"),
    ]
    stop = Stop(max_trials=4000, timeout_hours=36)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        table_sizes, tag_bits, num_ways, _, _ = _decode_tage_config(
            trial.tageConfig
        )
        _set_param(
            root, "system.cpu[0].branchPred.tage.numPredictors", _NUM_PREDICTORS
        )
        _set_param(root, "system.cpu[0].branchPred.tage.tableSizes", table_sizes)
        _set_param(root, "system.cpu[0].branchPred.tage.TTagBitSizes", tag_bits)
        _set_param(root, "system.cpu[0].branchPred.tage.numWays", num_ways)

from collections import Counter, defaultdict

from util.solver.runtime.path_resolver import resolve_target
from util.solver.spec import Maximize, Minimize, SolveSpec, Stop, TunableParam
from util.solver.spec.domain import DomainError


# The active BTBTAGE entry uses valid (1), tag (variable), a 3-bit direction
# counter, and useful (1).  Stored PC and LRU fields are not in the active
# lookup/replacement datapath, so they are excluded from the logical SRAM bit
# budget.
_FIXED_ENTRY_BITS = 5
_BASELINE_NUM_PREDICTORS = 8
_BASELINE_TABLE_SIZES = [2048] * _BASELINE_NUM_PREDICTORS
_BASELINE_TAG_BITS = [13] * _BASELINE_NUM_PREDICTORS
_BASELINE_NUM_WAYS = [2] * _BASELINE_NUM_PREDICTORS
_TABLE_SIZE_VALUES = [1 << exponent for exponent in range(6, 14)]
_TAG_BIT_VALUES = list(range(8, 21))
_NUM_WAY_VALUES = list(range(1, 9))
_NUM_PREDICTOR_VALUES = [6, 7, 8]


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
_MIN_CAPACITY_BITS = _BASELINE_CAPACITY_BITS * 50 // 100
_MAX_CAPACITY_BITS = _BASELINE_CAPACITY_BITS * 120 // 100


def _encode_tage_config(num_predictors, table_sizes, tag_bits, num_ways):
    return [int(num_predictors)] + list(table_sizes) + list(tag_bits) + list(num_ways)


def _decode_tage_config(config):
    values = [int(value) for value in config]
    if not values:
        raise ValueError("TAGE configuration must not be empty")
    num_predictors = values[0]
    if num_predictors not in _NUM_PREDICTOR_VALUES:
        raise ValueError(f"unsupported numPredictors {num_predictors}")
    expected_length = 1 + num_predictors * 3
    if len(values) != expected_length:
        raise ValueError(
            f"TAGE configuration for {num_predictors} tables must contain "
            f"{expected_length} values, got {len(values)}"
        )
    table_sizes = values[1 : 1 + num_predictors]
    tag_bits = values[1 + num_predictors : 1 + 2 * num_predictors]
    num_ways = values[1 + 2 * num_predictors :]
    if any(value not in _TABLE_SIZE_VALUES for value in table_sizes):
        raise ValueError(f"invalid TAGE table sizes: {table_sizes}")
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
    return num_predictors, table_sizes, tag_bits, num_ways, capacity_bits


class _CapacityConstrainedTageDomain:
    kind = "capacity_constrained_tage_config"

    def __init__(self):
        self._cardinality = None

    def _random_table(self, rng):
        return (
            rng.choice(_TABLE_SIZE_VALUES),
            rng.choice(_TAG_BIT_VALUES),
            rng.choice(_NUM_WAY_VALUES),
        )

    def _is_valid(self, config):
        try:
            _decode_tage_config(config)
        except ValueError:
            return False
        return True

    def sample(self, rng):
        # Uniform draws over the requested discrete choices, conditioned on the
        # exact capacity interval.  Acceptance is about 22% for this domain.
        for _ in range(4096):
            num_predictors = rng.choice(_NUM_PREDICTOR_VALUES)
            tables = [self._random_table(rng) for _ in range(num_predictors)]
            table_sizes, tag_bits, num_ways = map(list, zip(*tables))
            config = _encode_tage_config(
                num_predictors, table_sizes, tag_bits, num_ways
            )
            if self._is_valid(config):
                return config
        raise DomainError("failed to sample a capacity-constrained TAGE config")

    def mutate(self, rng, value):
        num_predictors, table_sizes, tag_bits, num_ways, _ = _decode_tage_config(
            value
        )
        for _ in range(512):
            next_num_predictors = num_predictors
            next_table_sizes = list(table_sizes)
            next_tag_bits = list(tag_bits)
            next_num_ways = list(num_ways)
            if rng.random() < 0.15:
                return self.sample(rng)
            table = rng.randrange(next_num_predictors)
            knob = rng.randrange(3)
            if knob == 0:
                next_table_sizes[table] = rng.choice(_TABLE_SIZE_VALUES)
            elif knob == 1:
                next_tag_bits[table] = rng.choice(_TAG_BIT_VALUES)
            else:
                next_num_ways[table] = rng.choice(_NUM_WAY_VALUES)
            config = _encode_tage_config(
                next_num_predictors,
                next_table_sizes,
                next_tag_bits,
                next_num_ways,
            )
            if config != list(value) and self._is_valid(config):
                return config
        return self.sample(rng)

    def crossover(self, rng, left, right):
        return self._crossover_one(rng, left, right), self._crossover_one(
            rng, right, left
        )

    def _crossover_one(self, rng, preferred, alternate):
        preferred_count, preferred_sizes, preferred_tags, preferred_ways, _ = (
            _decode_tage_config(preferred)
        )
        _, alternate_sizes, alternate_tags, alternate_ways, _ = _decode_tage_config(
            alternate
        )
        for _ in range(512):
            table_sizes = []
            tag_bits = []
            num_ways = []
            for table in range(preferred_count):
                if table < len(alternate_sizes) and rng.random() < 0.5:
                    table_sizes.append(alternate_sizes[table])
                    tag_bits.append(alternate_tags[table])
                    num_ways.append(alternate_ways[table])
                else:
                    table_sizes.append(preferred_sizes[table])
                    tag_bits.append(preferred_tags[table])
                    num_ways.append(preferred_ways[table])
            config = _encode_tage_config(
                preferred_count, table_sizes, tag_bits, num_ways
            )
            if self._is_valid(config):
                return config
        return self.sample(rng)

    def iter_values(self):
        raise DomainError(
            "capacity-constrained TAGE configs are too large to enumerate; "
            "use nsga2 or random sampling"
        )

    def cardinality(self):
        if self._cardinality is not None:
            return self._cardinality

        # Count configurations in 64-bit units.  Every legal table capacity is
        # a multiple of 64 because the smallest requested set count is 64.
        option_counts = Counter(
            table_size // 64 * (tag_bits + _FIXED_ENTRY_BITS) * num_ways
            for table_size in _TABLE_SIZE_VALUES
            for tag_bits in _TAG_BIT_VALUES
            for num_ways in _NUM_WAY_VALUES
        )
        minimum_units = (_MIN_CAPACITY_BITS + 63) // 64
        maximum_units = _MAX_CAPACITY_BITS // 64
        capacities = {0: 1}
        total = 0
        for table_count in range(1, max(_NUM_PREDICTOR_VALUES) + 1):
            next_capacities = defaultdict(int)
            for previous_capacity, multiplicity in capacities.items():
                for option_capacity, option_count in option_counts.items():
                    capacity = previous_capacity + option_capacity
                    if capacity <= maximum_units:
                        next_capacities[capacity] += multiplicity * option_count
            capacities = next_capacities
            if table_count in _NUM_PREDICTOR_VALUES:
                total += sum(
                    multiplicity
                    for capacity, multiplicity in capacities.items()
                    if capacity >= minimum_units
                )
        self._cardinality = total
        return total

    def to_dict(self):
        return {
            "kind": self.kind,
            "num_predictors": _NUM_PREDICTOR_VALUES,
            "table_size_values": _TABLE_SIZE_VALUES,
            "tag_bit_values": _TAG_BIT_VALUES,
            "num_way_values": _NUM_WAY_VALUES,
            "fixed_entry_bits": _FIXED_ENTRY_BITS,
            "baseline_capacity_bits": _BASELINE_CAPACITY_BITS,
            "min_capacity_bits": _MIN_CAPACITY_BITS,
            "max_capacity_bits": _MAX_CAPACITY_BITS,
            "encoding": (
                "numPredictors, then tableSizes, TTagBitSizes, and numWays; "
                "each vector has numPredictors elements"
            ),
        }


_TAGE_CONFIG_DOMAIN = _CapacityConstrainedTageDomain()
_BASELINE_CONFIG = _encode_tage_config(
    _BASELINE_NUM_PREDICTORS,
    _BASELINE_TABLE_SIZES,
    _BASELINE_TAG_BITS,
    _BASELINE_NUM_WAYS,
)


def _set_param(root, target, value):
    owner, _, param_name = resolve_target(root, target)
    setattr(owner, param_name, owner._params[param_name].convert(value))


class TageCapacityNsga2ScoreBranchMispredictSearch(SolveSpec):
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
    stop = Stop(max_trials=4000, timeout_hours=48)

    @classmethod
    def apply_trial(cls, root, trial) -> None:
        num_predictors, table_sizes, tag_bits, num_ways, _ = _decode_tage_config(
            trial.tageConfig
        )
        _set_param(
            root, "system.cpu[0].branchPred.tage.numPredictors", num_predictors
        )
        _set_param(root, "system.cpu[0].branchPred.tage.tableSizes", table_sizes)
        _set_param(root, "system.cpu[0].branchPred.tage.TTagBitSizes", tag_bits)
        _set_param(root, "system.cpu[0].branchPred.tage.numWays", num_ways)

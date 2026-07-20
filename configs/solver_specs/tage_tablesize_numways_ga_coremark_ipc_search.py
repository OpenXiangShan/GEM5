from configs.solver_specs.tage_tablesize_numways_ga_score_search import (
    TageTableSizeNumWaysGaScoreSearch,
)
from util.solver.spec import Maximize, Stop


class TageTableSizeNumWaysGaCoremarkIPCSearch(
    TageTableSizeNumWaysGaScoreSearch
):
    benchmark_type = "custom_bin"
    specific_benchmarks = ""
    custom_bin = "/nfs/home/share/gem5_ci/checkpoints/coremark-riscv64-xs.bin"
    extra_args = ""
    solver_name = "ga"

    objective = Maximize.stats("system.cpu.ipc")
    stop = Stop(max_trials=128)

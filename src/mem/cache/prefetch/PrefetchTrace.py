# Copyright (c) 2026 OpenXiangShan
# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.proxy import Parent
from m5.SimObject import SimObject


class PrefetchTrace(SimObject):
    type = "PrefetchTrace"
    cxx_class = "gem5::PrefetchTrace"
    cxx_header = "mem/cache/prefetch/prefetch_trace.hh"

    filename = Param.String(
        "prefetch-trace.csv", "Buffered event output in m5out"
    )
    start_tick = Param.Tick(0, "Inclusive trace start tick")
    end_tick = Param.Tick(MaxTick, "Exclusive trace end tick")
    after_warmup = Param.Bool(
        False, "Start at first statistics reset after tick 0"
    )
    buffer_size = Param.Unsigned(1048576, "Output buffer size in bytes")
    block_size = Param.Unsigned(Parent.cache_line_size, "Cache line size")

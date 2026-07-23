# -*- mode:python -*-

from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *


class RiscvMptUnit(ClockedObject):
    type = "RiscvMptUnit"
    cxx_class = "gem5::RiscvISA::MptUnit"
    cxx_header = "arch/riscv/mpt_unit.hh"

    port = RequestPort("Memory-side port for MPT entry reads")
    system = Param.System(Parent.any, "System object")

    enable_mpt_cache = Param.Bool(True, "Enable the dedicated MPT cache")
    hit_latency = Param.Cycles(3, "Pipelined MPT cache lookup latency")
    lookup_width = Param.Unsigned(4, "Shared MPT cache lookups per cycle")
    instruction_accept_width = Param.Unsigned(
        2, "Instruction MPT requests accepted per cycle"
    )
    data_accept_width = Param.Unsigned(
        5, "Data MPT requests accepted per cycle"
    )
    ptw_accept_width = Param.Unsigned(
        1, "PTW-protection MPT requests accepted per cycle"
    )

    instruction_queue_size = Param.Unsigned(
        8, "Instruction MPT lookup queue entries"
    )
    data_queue_size = Param.Unsigned(32, "Data MPT lookup queue entries")
    ptw_queue_size = Param.Unsigned(16, "PTW MPT lookup queue entries")

    num_mshrs = Param.Unsigned(8, "Number of MPT miss status entries")
    targets_per_mshr = Param.Unsigned(
        16, "Maximum number of coalesced targets per MPT MSHR"
    )
    memory_issue_width = Param.Unsigned(
        1, "Maximum MPT memory requests issued per cycle"
    )
    max_memory_inflight = Param.Unsigned(
        8, "Maximum number of in-flight MPT memory requests"
    )

    cache_l0_size = Param.Unsigned(32, "MPT cache L0 entries")
    cache_l1_size = Param.Unsigned(32, "MPT cache L1 entries")
    cache_l2_size = Param.Unsigned(32, "MPT cache L2 entries")
    cache_l3_size = Param.Unsigned(32, "MPT cache L3 entries")
    cache_sp_size = Param.Unsigned(32, "MPT cache superpage entries")

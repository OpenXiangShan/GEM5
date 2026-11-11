# Copyright (c) 2026
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.params import *
from m5.objects.ClockedObject import ClockedObject

# One CCHIFabric per system. It embeds CHIron's Cohestra::Instance, which
# owns the per-core Taurus upstream nodes (driven by the CCHIL1Agents) and
# one downstream CCHIInterface endpoint (the vendored Earth behavioral home
# for now; a Verilator binding plugs into the same abstraction later).
# The fabric's per-cycle event is simply Instance::Tick (CHIron's own flit
# pump), followed by the per-agent tick hooks.
class CCHIFabric(ClockedObject):
    type = 'CCHIFabric'
    cxx_header = 'mem/cchi/cchi_fabric.hh'
    cxx_class = 'gem5::CCHIFabric'

    # Port towards the gem5 memory system (membus -> MemCtrl). Serves the
    # downstream endpoint's memory traffic: the Earth model's MemoryBackend
    # reads/writes whole lines through it (Phase 1: atomic accesses).
    mem_side = RequestPort("Memory-side port towards membus/DRAM; serves "
                           "the downstream endpoint's memory traffic")

    upstream_node_count = Param.Unsigned(1,
        "Number of Taurus upstream nodes (one per core/CCHIL1Agent)")
    downstream = Param.String("earth",
        "Downstream endpoint: 'earth' (vendored behavioral home) or 'rtl' "
        "(Verilator backend; requires a WITH_CCHI_RTL build)")
    memory_start = Param.Addr(0x80000000,
        "Start of the CCHI-managed (cacheable) memory window")
    memory_end = Param.Addr(0xA0000000,
        "End (exclusive) of the CCHI-managed memory window")

    earth_latency_rsp = Param.Cycles(4,
        "Earth endpoint RSP-channel latency, in fabric cycles")
    earth_latency_dat = Param.Cycles(4,
        "Earth endpoint DAT-channel latency, in fabric cycles")

    monitor_enable = Param.Bool(True,
        "Attach the Cohestra CacheLineDataMonitor (per-line data-integrity "
        "scoreboard) to the embedded instance")
    monitor_fail_on_mismatch = Param.Bool(True,
        "Monitor data mismatches mark the Cohestra instance FAILED "
        "(raised as a gem5 fatal at the next fabric tick)")
    monitor_trace = Param.Bool(False,
        "Enable the monitor's per-access trace logging (very verbose)")
    flit_trace = Param.Bool(False,
        "Attach CHIron's CCHIFlitLogger ([cchi] verbose=1): flit-level "
        "transaction log from the Taurus nodes' channel events "
        "(very verbose; debug runs)")

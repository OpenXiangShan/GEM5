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
from m5.proxy import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CCHIFabric import CCHIFabric
from m5.objects.Prefetcher import BasePrefetcher

# Per-core bridge between a gem5 classic L1 cache hierarchy (L1I + L1D +
# MMU walker ports, all untouched) and one CCHI Taurus UpstreamNode owned
# by the CCHIFabric. The bridge translates MemCmds to Taurus Do* calls and
# reflects home-originated snoops into the L1s.
#
# Port choice: cpu_side is a VectorResponsePort so that L1I, L1D and the
# MMU walker ports of one core can all fan into the same agent without an
# intermediate crossbar (each master connects to the next vector index).
# A single ResponsePort would only accept one peer and could not host the
# walker fan-in.
class CCHIL1Agent(ClockedObject):
    type = 'CCHIL1Agent'
    cxx_header = 'mem/cchi/cchi_l1_agent.hh'
    cxx_class = 'gem5::CCHIL1Agent'

    cpu_side = VectorResponsePort(
        "CPU-side port(s): requests from the core's L1I/L1D caches and "
        "MMU walker ports arrive here; responses and snoops return here")
    mem_side = RequestPort(
        "Bypass port to the gem5 memory system for uncached/MMIO/atomic "
        "traffic (never touches CCHI)")

    fabric = Param.CCHIFabric("CCHI fabric owning this agent's Taurus node")
    system = Param.System(Parent.any, "System we belong to")
    node_id = Param.Unsigned("CCHI upstream node ID of this agent's "
                             "Taurus node (unique per core)")

    hit_latency = Param.Cycles(4,
        "Response latency for requests granted immediately (Taurus hits)")

    snoop_merge = Param.Bool(False,
        "Hold each home snoop at the fabric gate, reflect it into the L1 "
        "first, merge any dirty L1 data into the Taurus line, and only "
        "then let Taurus answer the home (exact multicore data; when "
        "False, Taurus answers immediately and L1 snoop data is dropped)")

    xaction_limit_req = Param.Unsigned(16,
        "Taurus in-flight REQ limit (raise towards the L1D MSHR count)")
    xaction_limit_evt = Param.Unsigned(16,
        "Taurus in-flight EVT (eviction) limit")
    xaction_limit_snp = Param.Unsigned(16,
        "Taurus in-flight SNP limit")

    # WIRE-UP ONLY: the L2 prefetch engine is hosted on the bridge
    # (L2CacheWrapper-style); probe/rxHint wiring is done at the config
    # level elsewhere. Emissions drained here become CCHI stash
    # transactions (DoPrefetchLoad/DoPrefetchStore) targeting the
    # endpoint-side L2.
    l2_prefetcher = Param.BasePrefetcher(NULL,
        "L2 prefetch engine hosted on the bridge (emissions become CCHI "
        "stash transactions)")

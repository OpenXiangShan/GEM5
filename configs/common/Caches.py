# Copyright (c) 2012 ARM Limited
# Copyright (c) 2020 Barkhausen Institut
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
# Copyright (c) 2006-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
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

from m5.defines import buildEnv
from m5.objects import *

# Base implementations of L1, L2, IO and TLB-walker caches. There are
# used in the regressions and also as base components in the
# system-configuration scripts. The values are meant to serve as a
# starting point, and specific parameters can be overridden in the
# specific instantiations.

class L1Cache(Cache):
    assoc = 2
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 4
    tgts_per_mshr = 20

    cache_level = 1

class L1_ICache(L1Cache):
    mshrs = 4
    is_read_only = True

    writeback_clean = False

    tag_latency = 1
    data_latency = 1
    sequential_access = False

    response_latency = 0

class L1_DCache(L1Cache):
    mshrs = 16

    writeback_clean = False

    # aligned latency:
    tag_latency = 1
    data_latency = 1
    sequential_access = False
    # recvTimingResp serviceMSHR latency, not really response latency
    response_latency = 0

    replacement_policy = TreePLRURP(num_leaves = Parent.assoc)

    wpu = MMRUWpu(use_virtual = True)

    tags = VIPTSetAssoc()
    tags.indexing_policy = VIPTSetAssociative()

    force_hit = False

    demand_mshr_reserve = 6

class L2Cache(Cache):
    mshrs = 64
    tgts_per_mshr = 20
    clusivity='mostly_incl'
    # always writeback clean when lower level is exclusive
    writeback_clean = True

    # aligned latency:
    tag_latency = 1
    data_latency = 2
    sequential_access = True
    # recvTimingResp serviceMSHR latency
    response_latency = 0

    replacement_policy = DRRIPRP(constituency_size = 64, team_size = 8)

    # reduce 2 cycles when way prediction is correct
    wpu = UTagWpu(utag_bits = 8, cycle_reduction = 2)

    cache_level = 2

    slice_num = 4

class L3Cache(Cache):
    mshrs = 64
    tgts_per_mshr = 20
    clusivity='mostly_excl'
    writeback_clean = False

    # aligned latency:
    tag_latency = 2
    data_latency = 5
    sequential_access = True
    # recvTimingResp serviceMSHR latency
    response_latency = 0

    cache_level = 3

class IOCache(Cache):
    assoc = 8
    tag_latency = 50
    data_latency = 50
    response_latency = 50
    mshrs = 20
    size = '1kB'
    tgts_per_mshr = 12

class PageTableWalkerCache(Cache):
    assoc = 2
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 10
    size = '1kB'
    tgts_per_mshr = 12
    writeback_clean = True

    # the x86 table walker actually writes to the table-walker cache
    if buildEnv['TARGET_ISA'] in ['x86', 'riscv']:
        is_read_only = False
    else:
        is_read_only = True
        # Writeback clean lines as well
        writeback_clean = True

class L1ToL2Bus(CoherentXBar):
    # 256-bit crossbar by default
    width = 64 # half one cacheline

    # Assume that most of this is covered by the cache latencies, with
    # no more than a single pipeline stage for any packet.
    frontend_latency = 0 # l1 -> l2 req additional latency
    forward_latency = 3 # l1 -> l2 req/snoop latency
    response_latency = 4 # l2 -> l1 resp latency
    snoop_response_latency = 1
    hint_wakeup_ahead_cycles = 2 # send Hint to L1 N cycles in advance with TimingResp

    # Use a snoop-filter by default, and set the latency to zero as
    # the lookup is assumed to overlap with the frontend latency of
    # the crossbar
    snoop_filter = SnoopFilter(lookup_latency = 0)

    # This specialisation of the coherent crossbar is to be considered
    # the point of unification, it connects the dcache and the icache
    # to the first level of unified cache.
    point_of_unification = True

class L2ToL3Bus(CoherentXBar):
    # 256-bit crossbar by default
    width = 64

    # Assume that most of this is covered by the cache latencies, with
    # no more than a single pipeline stage for any packet.
    frontend_latency = 0
    forward_latency = 5
    response_latency = 5
    snoop_response_latency = 1

    # Use a snoop-filter by default, and set the latency to zero as
    # the lookup is assumed to overlap with the frontend latency of
    # the crossbar
    snoop_filter = SnoopFilter(lookup_latency = 0)

    # This specialisation of the coherent crossbar is to be considered
    # the point of unification, it connects the dcache and the icache
    # to the first level of unified cache.
    point_of_unification = True

class L3ToMemBus(CoherentXBar):
    # 128-bit crossbar by default
    width = 64

    # A handful pipeline stages for each portion of the latency
    # contributions.
    frontend_latency = 0
    forward_latency = 9
    response_latency = 78
    snoop_response_latency = 4

    # Use a snoop-filter by default
    snoop_filter = SnoopFilter(lookup_latency = 1)

    # This specialisation of the coherent crossbar is to be considered
    # the point of coherency, as there are no (coherent) downstream
    # caches.
    point_of_coherency = True

    # This specialisation of the coherent crossbar is to be considered
    # the point of unification, it connects the dcache and the icache
    # to the first level of unified cache. This is needed for systems
    # without caches where the SystemXBar is also the point of
    # unification.
    point_of_unification = True

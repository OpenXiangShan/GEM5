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
    mshrs = 2
    is_read_only = True

    writeback_clean = False

    tag_latency = 1
    data_latency = 1
    sequential_access = False

    response_latency = 4
    enable_wayprediction = False

class L1_DCache(L1Cache):
    mshrs = 32

    writeback_clean = False

    # aligned latency:
    tag_latency = 1
    data_latency = 1
    sequential_access = False
    # This is communication latency between l1 & l2
    response_latency = 4

    force_hit = False

    demand_mshr_reserve = 8

class L2Cache(Cache):
    mshrs = 64
    tgts_per_mshr = 20
    clusivity='mostly_incl'
    # always writeback clean when lower level is exclusive
    writeback_clean = True

    # aligned latency:
    tag_latency = 2
    data_latency = 13
    sequential_access = True

    # This is communication latency between l2 & l3
    response_latency = 15

    cache_level = 2
    enable_wayprediction = False

class L3Cache(Cache):
    mshrs = 128
    tgts_per_mshr = 20
    clusivity='mostly_excl'
    writeback_clean = False

    # aligned latency:
    tag_latency = 2
    data_latency = 17
    sequential_access = True

    # This is L3 miss latency, which act as padding for memory controller

    # 5950x L3 miss latency (rand) = 70ns, L3 hit latency = 15ns, so Mem-->L3 = 55ns (165 cycle in 3GHz)
    # But XS's miss latency on mcf (less random) is averagely 211 on padding=112.
    # To make XS's L3 miss latency similar to 5950x, we reduce padding from 112 to (112 - (211-165)) = 66 cycle
    response_latency = 66

    cache_level = 3
    enable_wayprediction = False

class IOCache(Cache):
    assoc = 8
    tag_latency = 50
    data_latency = 50
    response_latency = 50
    mshrs = 20
    size = '1kB'
    tgts_per_mshr = 12
    enable_wayprediction = False

class PageTableWalkerCache(Cache):
    assoc = 2
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 10
    size = '1kB'
    tgts_per_mshr = 12
    writeback_clean = True
    enable_wayprediction = False

    # the x86 table walker actually writes to the table-walker cache
    if buildEnv['TARGET_ISA'] in ['x86', 'riscv']:
        is_read_only = False
    else:
        is_read_only = True
        # Writeback clean lines as well
        writeback_clean = True

# -*- mode:python -*-

# Copyright (c) 2020 ARM Limited
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

from m5.params import *
from m5.proxy import *

from m5.objects.BaseMMU import BaseMMU
from m5.objects.RiscvTLB import RiscvTLB, RiscvTLBL2
from m5.objects.PMAChecker import PMAChecker
from m5.objects.PMP import PMP
from m5.objects.XBar import NoncoherentXBar

class RiscvMMU(BaseMMU):
    type = 'RiscvMMU'
    cxx_class = 'gem5::RiscvISA::MMU'
    cxx_header = 'arch/riscv/mmu.hh'

    l2_shared = RiscvTLBL2(entry_type="unified")
    data_walker_xbar = NoncoherentXBar(
        frontend_latency=0, forward_latency=0, response_latency=0,
        width=32)

    itb = RiscvTLB(entry_type="instruction",
    next_level=Parent.l2_shared)
    dtb = RiscvTLB(entry_type="data",
                   next_level=Parent.l2_shared, is_dtlb=True)
    # Keep the original shared data TLB behavior by default. When enabled,
    # writes use stb while reads continue to use dtb.
    enable_store_tlb = Param.Bool(
        False, "Use an independent L1 TLB for write translations")
    # stb is always present in the SimObject graph so the walker topology is
    # static, but it receives translation and lifecycle traffic only when
    # enable_store_tlb is true. Both data L1 TLBs share l2_shared.
    stb = Param.RiscvTLB(
        RiscvTLB(entry_type="data", next_level=Parent.l2_shared,
                 is_dtlb=True), "Store TLB")
    pma_checker = Param.PMAChecker(PMAChecker(), "PMA Checker")
    pmp = Param.PMP(PMP(), "Physical Memory Protection Unit")

    @classmethod
    def walkerPorts(cls):
        # Expose one instruction walker port and one shared downstream data
        # port. dtb/stb connect to the XBar CPU-side ports below. The stb port
        # stays connected in shared mode because gem5's port graph is static,
        # but the disabled stb never generates a request.
        return ["mmu.itb.walker.port",
                "mmu.data_walker_xbar.mem_side_ports"]

    def connectWalkerPorts(self, iport, dport):
        # Keep load/store walkers independent up to the XBar, then make their
        # page-table memory traffic share the original data walker port. The
        # enable_store_tlb parameter controls traffic, not port construction.
        self.itb.walker.port = iport
        self.dtb.walker.port = self.data_walker_xbar.cpu_side_ports
        self.stb.walker.port = self.data_walker_xbar.cpu_side_ports
        self.data_walker_xbar.mem_side_ports = dport

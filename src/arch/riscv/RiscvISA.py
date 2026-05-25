# Copyright (c) 2012 ARM Limited
# Copyright (c) 2014 Sven Karlsson
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
# Copyright (c) 2016 RISC-V Foundation
# Copyright (c) 2016 The University of Virginia
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

from m5.objects.BaseISA import BaseISA
from m5.params import *

class RiscvISA(BaseISA):
    type = 'RiscvISA'
    cxx_class = 'gem5::RiscvISA::ISA'
    cxx_header = "arch/riscv/isa.hh"

    matrix_issue_interval_cycles = Param.Unsigned(
        1, "Analytic CUTE matrix issue interval in CPU cycles")
    matrix_load_base_cycles = Param.Unsigned(
        4, "Analytic CUTE matrix load base latency in CPU cycles")
    matrix_store_base_cycles = Param.Unsigned(
        4, "Analytic CUTE matrix store base latency in CPU cycles")
    matrix_zero_cycles = Param.Unsigned(
        1, "Analytic CUTE matrix zero latency in CPU cycles")
    matrix_compute_base_cycles = Param.Unsigned(
        2, "Fixed abstract CUTE matrix compute ready latency in CPU cycles")
    matrix_compute_read_cycles = Param.Unsigned(
        1, "Fixed abstract CUTE matrix compute source read latency in CPU cycles")
    matrix_release_cycles = Param.Unsigned(
        1, "Analytic CUTE matrix release latency in CPU cycles")
    matrix_local_mmu_issue_per_cycle = Param.Unsigned(
        1, "Analytic CUTE LocalMMU request issue throughput per CPU cycle")
    matrix_local_mmu_arb_cycles = Param.Unsigned(
        1, "Analytic CUTE LocalMMU arbitration latency in CPU cycles")
    matrix_l2_request_pipeline_cycles = Param.Unsigned(
        1, "Analytic CUTE-to-L2 request pipeline latency in CPU cycles")
    matrix_l2_response_pipeline_cycles = Param.Unsigned(
        1, "Analytic CUTE L2 response port service interval in CPU cycles")
    matrix_local_mmu_read_latency_cycles = Param.Unsigned(
        20, "Analytic CUTE LocalMMU read response latency in CPU cycles")
    matrix_local_mmu_write_ack_latency_cycles = Param.Unsigned(
        12, "Analytic CUTE LocalMMU write acknowledgement latency in CPU cycles")

# Copyright (c) 2013-2014 ARM Limited
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
from m5.SimObject import *

class ArchDBer(SimObject):
    type = 'ArchDBer'
    cxx_header = "sim/arch_db.hh"
    cxx_class = 'gem5::ArchDBer'

    cxx_exports = [
        PyBindMethod("start_recording"),
    ]

    arch_db_file = Param.String("", "Where to save arch db")
    dump_from_start = Param.Bool(True, "Dump arch db from start")
    enable_rolling = Param.Bool(False, "Dump rolling perfcnt")

    table_cmds = VectorParam.String([], "Tables to create")
    dump_mem_trace = Param.Bool(False, "Dump memory trace")
    dump_l1_pf_trace = Param.Bool(False, "Dump prefetch trace")
    dump_l1_evict_trace = Param.Bool(False, "Dump l1 evict trace")
    dump_l2_evict_trace = Param.Bool(False, "Dump l2 evict trace")
    dump_l3_evict_trace = Param.Bool(False, "Dump l3 evict trace")
    dump_l1_miss_trace = Param.Bool(False, "Dump l1 miss trace")
    dump_bop_train_trace = Param.Bool(False, "Dump bop train trace")
    dump_sms_train_trace = Param.Bool(False, "Dump sms train trace")
    dump_l1d_way_pre_trace = Param.Bool(False, "Dump l1d way predction trace")
    dump_lifetime = Param.Bool(False, "Dump inst lifetime")

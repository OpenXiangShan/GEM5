# Copyright (c) 2016 Georgia Institute of Technology
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

from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *

class SimpleTrace(ClockedObject):
    type = 'SimpleTrace'
    cxx_header = \
        "cpu/testers/simple_trace/SimpleTrace.hh"
    cxx_class = 'gem5::SimpleTrace'

    response_limit = Param.Cycles(5000000, "Cycles before exiting \
                                            due to lack of progress")
    test = RequestPort("Port to the memory system to test")
    system = Param.System(Parent.any, "System we belong to")
    sim_cycles = Param.Int(1000, "Number of simulation cycles")

    enable = Param.Bool(True, "Enable SimpleTrace")
    id = Param.Addr(0, "Context ID of this trace")
    trace_file = Param.String("", "Trace file to use")
    use_dummy_load = Param.Bool(True, "Use dummy load instead of trace file")
    default_addr = Param.Addr(0x80000000, "Address for dummy load")
    max_requests = Param.Int(100, "Maximum number of requests to send (-1 for unlimited)")

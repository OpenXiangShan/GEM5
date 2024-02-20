# Copyright (c) 2017 ARM Limited
# All rights reserved
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

from m5.SimObject import SimObject
from m5.params import *
from m5.objects.FuncUnit import *
from m5.objects.FuncUnitConfig import *

class FUPool(SimObject):
    type = 'FUPool'
    cxx_class = 'gem5::o3::FUPool'
    cxx_header = "cpu/o3/fu_pool.hh"
    FUList = VectorParam.FUDesc("list of FU's for this pool")

class DefaultFUPool(FUPool):
    FUList = [ IntALU(), IntMultDiv(), FP_MISC(), FP_SLOW(), FP_MAM(),
               FP_MAA(), ReadPort(), SIMD_Unit(), PredALU(), WritePort(),
               RdWrPort(), IprPort() ]

class XSCoreFUPool(FUPool):
    FUList = [
        IntALU(count=6),
        IntMultDiv(count=2),
        FP_MISC(count=2),
        FP_SLOW(count=2),
        FP_MAM(count=4),
        FP_MAA(count=4),
        ReadPort(count=2),
        WritePort(count=2),
        SIMD_Unit(count=2),
    ]

class XSECoreFUPool(FUPool):
    FUList = [
        IntALU(count=4),
        IntMultDiv(count=1),
        FP_MISC(count=2),
        FP_SLOW(count=1),
        FP_MAM(count=2),
        FP_MAA(count=2),
        ReadPort(count=1),
        RdWrPort(count=1),
        SIMD_Unit(count=2),
    ]

class XSECore2ReadFUPool(FUPool):
    FUList = [
        IntALU(count=4),
        IntMultDiv(count=1),
        FP_MISC(count=2),
        FP_SLOW(count=1),
        FP_MAM(count=2),
        FP_MAA(count=2),
        ReadPort(count=2),
        WritePort(count=1),
        SIMD_Unit(count=2),
    ]

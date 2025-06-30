# Copyright (c) 2010, 2017 ARM Limited
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

import sys
from m5.SimObject import SimObject
from m5.defines import buildEnv
from m5.params import *

from m5.objects.FuncUnit import *

class IntMisc(FUDesc):
    opList = [ OpDesc(opClass='No_OpClass', opLat=2),
               OpDesc(opClass='VectorConfig') ]

class IntALU(FUDesc):
    opList = [ OpDesc(opClass='IntAlu') ]

class IntBRU(FUDesc):
    opList = [ OpDesc(opClass='IntBr') ]

class IntMultDiv(FUDesc):
    opList = [ OpDesc(opClass='IntMult', opLat=3),
               OpDesc(opClass='IntDiv', opLat=20, pipelined=False) ]

    # DIV and IDIV instructions in x86 are implemented using a loop which
    # issues division microops.  The latency of these microops should really be
    # one (or a small number) cycle each since each of these computes one bit
    # of the quotient.
    if buildEnv['TARGET_ISA'] in ('x86'):
        opList[1].opLat=1

    count=2

class IntDiv(FUDesc):
    # for instructions with uncertain cycle
    # just set opLat = sys.maxsize
    # it can help us find potential bugs
    opList = [ OpDesc(opClass='IntDiv', opLat=sys.maxsize, pipelined=False) ]


class IntMult(FUDesc):
    opList = [ OpDesc(opClass='IntMult', opLat=3) ]

class FP_SLOW(FUDesc):
    opList = [ OpDesc(opClass='FloatDiv', opLat=sys.maxsize, pipelined=False),
               OpDesc(opClass='FloatSqrt', opLat=sys.maxsize, pipelined=False)]

class FP_ALU(FUDesc):
    opList = [ OpDesc(opClass='FloatCmp', opLat=3),
               OpDesc(opClass='FloatAdd', opLat=2),
               OpDesc(opClass='FloatMisc', opLat=2),
               OpDesc(opClass='FloatMult', opLat=4)]

class FP_MISC(FUDesc):
    opList = [ OpDesc(opClass='FloatCvt', opLat=5), # float -> int 5 cycles, int -> float 7 cycle
               OpDesc(opClass='FloatMv', opLat=5)]

class FP_MAC(FUDesc):
    opList = [ OpDesc(opClass='FloatMultAcc', opLat=4)]

class SIMD_Unit(FUDesc):
    opList = [ OpDesc(opClass='VectorIntegerArith'),
               OpDesc(opClass='VectorFloatArith'),
               OpDesc(opClass='VectorFloatConvert'),
               OpDesc(opClass='VectorIntegerReduce'),
               OpDesc(opClass='VectorFloatReduce'),
               OpDesc(opClass='VectorMisc'),
               OpDesc(opClass='VectorIntegerExtension')]

class ReadPort(FUDesc):
    opList = [ OpDesc(opClass='MemRead', opLat=4), # actually execute cycle = loadpipe's stage
               OpDesc(opClass='FloatMemRead', opLat=4),
               OpDesc(opClass='VectorUnitStrideLoad', opLat=4),
               OpDesc(opClass='VectorSegUnitStrideLoad', opLat=4),
               OpDesc(opClass='VectorUnitStrideMaskLoad', opLat=4),
               OpDesc(opClass='VectorSegUnitStrideMaskLoad', opLat=4),
               OpDesc(opClass='VectorStridedLoad', opLat=4),
               OpDesc(opClass='VectorSegStridedLoad', opLat=4),
               OpDesc(opClass='VectorIndexedLoad', opLat=4),
               OpDesc(opClass='VectorSegIndexedLoad', opLat=4),
               OpDesc(opClass='VectorUnitStrideFaultOnlyFirstLoad', opLat=4),
               OpDesc(opClass='VectorWholeRegisterLoad', opLat=4)]

class WritePort(FUDesc):
    opList = [ OpDesc(opClass='MemWrite', opLat=2),
               OpDesc(opClass='MemAtomic', opLat=13),
               OpDesc(opClass='FloatMemWrite', opLat=3),
               OpDesc(opClass='VectorUnitStrideStore'),
               OpDesc(opClass='VectorSegUnitStrideStore'),
               OpDesc(opClass='VectorUnitStrideMaskStore'),
               OpDesc(opClass='VectorStridedStore'),
               OpDesc(opClass='VectorIndexedStore'),
               OpDesc(opClass='VectorWholeRegisterStore')]

class StoreDataPort(FUDesc):
    opList = [ OpDesc(opClass='StoreData', opLat=1)]

class RdWrPort(FUDesc):
    opList = [ OpDesc(opClass='MemRead', opLat=2),
               OpDesc(opClass='MemWrite', opLat=2),
               OpDesc(opClass='FloatMemRead', opLat=2),
               OpDesc(opClass='FloatMemWrite', opLat=3),
               OpDesc(opClass='VectorUnitStrideLoad', opLat=3),
               OpDesc(opClass='VectorSegUnitStrideLoad', opLat=3),
               OpDesc(opClass='VectorUnitStrideMaskLoad', opLat=3),
               OpDesc(opClass='VectorSegUnitStrideMaskLoad', opLat=3),
               OpDesc(opClass='VectorStridedLoad', opLat=3),
               OpDesc(opClass='VectorSegStridedLoad', opLat=3),
               OpDesc(opClass='VectorIndexedLoad', opLat=3),
               OpDesc(opClass='VectorSegIndexedLoad', opLat=3),
               OpDesc(opClass='VectorUnitStrideFaultOnlyFirstLoad', opLat=3),
               OpDesc(opClass='VectorWholeRegisterLoad', opLat=3),
               OpDesc(opClass='VectorUnitStrideStore'),
               OpDesc(opClass='VectorSegUnitStrideStore'),
               OpDesc(opClass='VectorUnitStrideMaskStore'),
               OpDesc(opClass='VectorStridedStore'),
               OpDesc(opClass='VectorIndexedStore'),
               OpDesc(opClass='VectorWholeRegisterStore')]
    count = 0

class IprPort(FUDesc):
    opList = [ OpDesc(opClass='IprAccess', opLat = 3, pipelined = False) ]
    count = 1

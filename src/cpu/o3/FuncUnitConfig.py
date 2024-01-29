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

from m5.SimObject import SimObject
from m5.defines import buildEnv
from m5.params import *

from m5.objects.FuncUnit import *

class IntALU(FUDesc):
    opList = [ OpDesc(opClass='IntAlu') ]
    count = 6

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

class IntMult(FUDesc):
    opList = [ OpDesc(opClass='IntMult', opLat=3) ]

    count=2

class FP_SLOW(FUDesc):
    opList = [OpDesc(opClass='FloatDiv', opLat=19, pipelined=False),
              OpDesc(opClass='FloatSqrt', opLat=24, pipelined=False),
              ]
    count = 2

class FP_MISC(FUDesc):
    opList = [OpDesc(opClass='FloatCvt', opLat=3),
              OpDesc(opClass='FloatCmp', opLat=3),
              OpDesc(opClass='FloatMisc', opLat=2),]
    count = 2

class FP_MAM(FUDesc):
    opList = [ OpDesc(opClass='FMAMul', opLat=3),
               OpDesc(opClass='FloatMult', opLat=3),]
    count = 4

class FP_MAA(FUDesc):
    opList = [ OpDesc(opClass='FMAAcc', opLat=2),
               OpDesc(opClass='FloatAdd', opLat=3)]
    count = 4

class SIMD_Unit(FUDesc):
    opList = [ OpDesc(opClass='SimdAdd'),
               OpDesc(opClass='SimdAddAcc'),
               OpDesc(opClass='SimdAlu'),
               OpDesc(opClass='SimdCmp'),
               OpDesc(opClass='SimdCvt'),
               OpDesc(opClass='SimdMisc'),
               OpDesc(opClass='SimdMult'),
               OpDesc(opClass='SimdMultAcc'),
               OpDesc(opClass='SimdShift'),
               OpDesc(opClass='SimdShiftAcc'),
               OpDesc(opClass='SimdDiv'),
               OpDesc(opClass='SimdSqrt'),
               OpDesc(opClass='SimdFloatAdd'),
               OpDesc(opClass='SimdFloatAlu'),
               OpDesc(opClass='SimdFloatCmp'),
               OpDesc(opClass='SimdFloatCvt'),
               OpDesc(opClass='SimdFloatDiv'),
               OpDesc(opClass='SimdFloatMisc'),
               OpDesc(opClass='SimdFloatMult'),
               OpDesc(opClass='SimdFloatMultAcc'),
               OpDesc(opClass='SimdFloatSqrt'),
               OpDesc(opClass='SimdReduceAdd'),
               OpDesc(opClass='SimdReduceAlu'),
               OpDesc(opClass='SimdReduceCmp'),
               OpDesc(opClass='SimdFloatReduceAdd'),
               OpDesc(opClass='SimdFloatReduceCmp'),

               OpDesc(opClass='VectorIntegerArith'),
               OpDesc(opClass='VectorFloatArith'),
               OpDesc(opClass='VectorFloatConvert'),
               OpDesc(opClass='VectorIntegerReduce'),
               OpDesc(opClass='VectorFloatReduce'),
               OpDesc(opClass='VectorMisc'),
               OpDesc(opClass='VectorIntegerExtension'),
               OpDesc(opClass='VectorConfig')]
    count = 2

class PredALU(FUDesc):
    opList = [ OpDesc(opClass='SimdPredAlu') ]
    count = 1

class ReadPort(FUDesc):
    opList = [ OpDesc(opClass='MemRead', opLat=2),
               OpDesc(opClass='FloatMemRead'),
               OpDesc(opClass='VectorUnitStrideLoad', opLat=2),
               OpDesc(opClass='VectorUnitStrideMaskLoad', opLat=2),
               OpDesc(opClass='VectorStridedLoad', opLat=2),
               OpDesc(opClass='VectorIndexedLoad', opLat=2),
               OpDesc(opClass='VectorUnitStrideFaultOnlyFirstLoad', opLat=2),
               OpDesc(opClass='VectorWholeRegisterLoad', opLat=2)]
    count = 2

class WritePort(FUDesc):
    opList = [ OpDesc(opClass='MemWrite', opLat=4),
               OpDesc(opClass='FloatMemWrite'),
               OpDesc(opClass='VectorUnitStrideStore'),
               OpDesc(opClass='VectorUnitStrideMaskStore'),
               OpDesc(opClass='VectorStridedStore'),
               OpDesc(opClass='VectorIndexedStore'),
               OpDesc(opClass='VectorWholeRegisterStore')]
    count = 2

class RdWrPort(FUDesc):
    opList = [ OpDesc(opClass='MemRead', opLat=2),
               OpDesc(opClass='MemWrite', opLat=4),
               OpDesc(opClass='FloatMemRead'),
               OpDesc(opClass='FloatMemWrite'),
               OpDesc(opClass='VectorUnitStrideLoad', opLat=2),
               OpDesc(opClass='VectorUnitStrideMaskLoad', opLat=2),
               OpDesc(opClass='VectorStridedLoad', opLat=2),
               OpDesc(opClass='VectorIndexedLoad', opLat=2),
               OpDesc(opClass='VectorUnitStrideFaultOnlyFirstLoad', opLat=2),
               OpDesc(opClass='VectorWholeRegisterLoad', opLat=2),
               OpDesc(opClass='VectorUnitStrideStore'),
               OpDesc(opClass='VectorUnitStrideMaskStore'),
               OpDesc(opClass='VectorStridedStore'),
               OpDesc(opClass='VectorIndexedStore'),
               OpDesc(opClass='VectorWholeRegisterStore')
               ]
    count = 0

class IprPort(FUDesc):
    opList = [ OpDesc(opClass='IprAccess', opLat = 3, pipelined = False) ]
    count = 1


class ScheduleDelayMatrixMap(SimObject):
    type = 'ScheduleDelayMatrixMap'
    cxx_header = "cpu/o3/iew_delay_calibrator.hh"
    cxx_class = 'gem5::o3::ScheduleDelayMatrixMap'

    dep_opclass = Param.OpClass('the opclass of dep_inst (aka. consumer instructions)')
    completed_opclass = Param.OpClass('the opclass of complete_inst (aka. producer instructions)')
    delay_tick = Param.UInt32(
        'the delay tick between dep_inst and complete_inst')


class DelayCalibrator(SimObject):
    type = 'DelayCalibrator'
    cxx_header = "cpu/o3/iew_delay_calibrator.hh"
    cxx_class = 'gem5::o3::DelayCalibrator'
    # dep_inst completed_inst
    # actually the order of dep_inst and completed_inst doesn't have much
    # effect on the cycle delay????
    matrix = VectorParam.ScheduleDelayMatrixMap('')


class DefaultDelayMatrix(DelayCalibrator):
    # dep_inst = consumer instruction; completed_inst = producer instruction
    matrix = [
        ScheduleDelayMatrixMap(dep_opclass='IntAlu', completed_opclass='IntAlu', delay_tick=0),
        ScheduleDelayMatrixMap(dep_opclass='IntMult', completed_opclass='IntAlu', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='IntDiv', completed_opclass='IntDiv', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='IntDiv',
                               completed_opclass='IntMult', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='IntDiv',
                               completed_opclass='IntAlu', delay_tick=2),

        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='IntAlu', delay_tick=3),
        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='IntMult', delay_tick=3),
        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='IntDiv', delay_tick=2),

        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='FloatAdd', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='FloatMult', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='FloatDiv', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='FloatCvt',
                               completed_opclass='FloatCvt', delay_tick=2),

        ScheduleDelayMatrixMap(dep_opclass='MemRead',
                               completed_opclass='IntAlu', delay_tick=0),
        ScheduleDelayMatrixMap(dep_opclass='MemRead',
                               completed_opclass='MemRead', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='IntAlu',
                               completed_opclass='MemRead', delay_tick=0),

        ScheduleDelayMatrixMap(dep_opclass='FMAAcc',
                               completed_opclass='FloatMemRead', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='FMAMul',
                               completed_opclass='FloatMemRead', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='FloatMult',
                               completed_opclass='FloatMemRead', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='FloatAdd',
                               completed_opclass='FloatMemRead', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='FMAAcc',
                               completed_opclass='FloatCvt', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='FMAMul',
                               completed_opclass='FloatCvt', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='FloatMult',
                               completed_opclass='FloatCvt', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='FloatAdd',
                               completed_opclass='FloatCvt', delay_tick=2),

        # load to use
        # maybe we need to recalibrate in the future
        ScheduleDelayMatrixMap(dep_opclass='MemRead',
                               completed_opclass='MemWrite', delay_tick=3),
        ScheduleDelayMatrixMap(dep_opclass='MemWrite',
                               completed_opclass='MemRead', delay_tick=2),
        ScheduleDelayMatrixMap(dep_opclass='IntMult',
                               completed_opclass='MemRead', delay_tick=1),
        ScheduleDelayMatrixMap(dep_opclass='IntDiv', completed_opclass='MemRead', delay_tick=1)]

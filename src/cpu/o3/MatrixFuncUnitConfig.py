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

from m5.objects.FuncUnit import FUDesc, OpDesc

MATRIX_INT_LATENCY = 1
MATRIX_RELEASE_LATENCY = 1
MATRIX_CSR_LATENCY = 1
MATRIX_MMA_LATENCY = 1
MATRIX_ARITH_LATENCY = 1
MATRIX_SYNC_LATENCY = 1
MATRIX_MEM_LATENCY = 3

class MatrixIntFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixInt', opLat=MATRIX_INT_LATENCY) ]

class MatrixReleaseFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixRelease', opLat=MATRIX_RELEASE_LATENCY) ]

class MatrixCsrFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixCsr', opLat=MATRIX_CSR_LATENCY) ]

class MatrixMmaFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixMma', opLat=MATRIX_MMA_LATENCY) ]

class MatrixArithFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixArith', opLat=MATRIX_ARITH_LATENCY) ]

class MatrixSyncFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixSync', opLat=MATRIX_SYNC_LATENCY, pipelined=False) ]

class MatrixMemFU(FUDesc):
    opList = [ OpDesc(opClass='MatrixMem', opLat=MATRIX_MEM_LATENCY) ]

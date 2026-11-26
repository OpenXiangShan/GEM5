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

"""Configure Matrix functional units, scheduling and coherent memory ingress."""

from m5.defines import buildEnv
from m5.util import fatal

if buildEnv['GEM5_ENABLE_MATRIX']:
    from m5.objects import Cache, IssuePort, IssueQue, SpecWakeupChannel
    from m5.objects.FuncUnit import FUDesc, OpDesc
    from common.FUScheduler import IntRD, IntWR, KunminghuScheduler
    from common.xiangshan import XiangshanCore

    class MatrixIntFU(FUDesc):
        opList = [OpDesc(opClass='MatrixInt')]

    class MatrixReleaseFU(FUDesc):
        opList = [OpDesc(opClass='MatrixRelease')]

    class MatrixCsrFU(FUDesc):
        opList = [OpDesc(opClass='MatrixCsr')]

    class MatrixMmaFU(FUDesc):
        opList = [OpDesc(opClass='MatrixMma')]

    class MatrixArithFU(FUDesc):
        opList = [OpDesc(opClass='MatrixArith')]

    class MatrixSyncFU(FUDesc):
        opList = [OpDesc(opClass='MatrixSync', pipelined=False)]

    class MatrixMemFU(FUDesc):
        opList = [OpDesc(opClass='MatrixMem', opLat=3)]

    class MatrixXiangshanCore(XiangshanCore):
        enableMatrixBackend = True
        enableMatrixMemPort = True

        def __init__(self, **kwargs):
            super().__init__(**kwargs)
            self._sync_matrix_cached_port()

        def _sync_matrix_cached_port(self):
            self._cached_ports = [port for port in self._cached_ports
                                  if port not in ('matrix_mem_port',
                                                  'matrix_cache.mem_side')]
            if not self.enableMatrixMemPort:
                return
            if not hasattr(self, 'matrix_cache'):
                # This small classic cache keeps Matrix accesses coherent
                # with the CPU DCache through the existing L2. It serves
                # hits and absorbs writes, affecting latency and L2 traffic.
                self.matrix_cache = Cache(
                    size='1KiB', assoc=2, tag_latency=1, data_latency=1,
                    response_latency=1, mshrs=16, demand_mshr_reserve=0,
                    tgts_per_mshr=4, write_buffers=8, do_fast_writeline=False)
                self.matrix_mem_port = self.matrix_cache.cpu_side
            if self.matrix_cache.mem_side.peer is None:
                self._cached_ports.append('matrix_cache.mem_side')

        def addPrivateSplitL1Caches(self, ic, dc, iwc=None, dwc=None):
            super().addPrivateSplitL1Caches(ic, dc, iwc, dwc)
            self._sync_matrix_cached_port()

        def connectCachedPorts(self, in_ports):
            self._sync_matrix_cached_port()
            super().connectCachedPorts(in_ports)


def RMiscRD(id, p):
    assert id < 16
    assert p < 4
    return (2 << 6) | (id << 2) | p


def RMiscWR(id, p):
    return (1 << 8) | RMiscRD(id, p)


def _add_matrix(scheduler, sync_iq, sync_port):
    # Release the existing vector before extending it so every child is
    # adopted together, including the new Matrix objects.
    iqs = list(scheduler.IQs)
    scheduler.clear_child('IQs')
    for iq in iqs:
        if iq.name == sync_iq:
            port = iq.oports[sync_port]
            units = list(port.fu)
            port.clear_child('fu')
            port.fu = units + [MatrixSyncFU()]
    int_mem = [iq.name for iq in iqs
               if str(iq.name).startswith(('intIQ', 'load', 'store', 'std'))]
    matrix_iqs = [
        IssueQue(name='matrixIntIQ', inports=2, size=16, oports=[
            IssuePort(fu=[MatrixIntFU()],
                      rp=[IntRD(12, 0), RMiscWR(0, 0)]),
            IssuePort(fu=[MatrixReleaseFU()],
                      rp=[IntRD(13, 1)])
        ], scheduleToExecDelay=2),
        IssueQue(name='matrixCsrIQ', inports=2, size=16, oports=[
            IssuePort(fu=[MatrixCsrFU()],
                      rp=[IntRD(12, 0), RMiscRD(0, 0),
                          IntWR(3, 0), RMiscWR(1, 0)])
        ], scheduleToExecDelay=2),
        IssueQue(name='matrixExecIQ', inports=2, size=16, oports=[
            IssuePort(fu=[MatrixMmaFU()],
                      rp=[RMiscRD(1, 0), RMiscRD(2, 0), RMiscRD(3, 0),
                          RMiscRD(4, 0), RMiscRD(5, 0), RMiscRD(6, 0)]),
            IssuePort(fu=[MatrixArithFU()],
                      rp=[RMiscRD(1, 1), RMiscRD(2, 1)])
        ], scheduleToExecDelay=2),
        IssueQue(name='matrixMemIQ', inports=2, size=16, oports=[
            IssuePort(fu=[MatrixMemFU()],
                      rp=[IntRD(14, 0), IntRD(15, 1),
                          RMiscRD(7, 0), RMiscRD(8, 0)])
        ], scheduleToExecDelay=3),
    ]

    matrix_bank = [iq.name for iq in matrix_iqs]
    scheduler.IQs = iqs + matrix_iqs
    channels = list(scheduler.specWakeupNetwork)
    scheduler.clear_child('specWakeupNetwork')
    scheduler.specWakeupNetwork = channels + [
        SpecWakeupChannel(srcs=int_mem, dsts=matrix_bank),
        SpecWakeupChannel(srcs=['matrixIntIQ'], dsts=matrix_bank),
        SpecWakeupChannel(srcs=['matrixCsrIQ'], dsts=[
            iq.name for iq in scheduler.IQs
            if str(iq.name).startswith('intIQ')] + matrix_bank),
    ]
    return scheduler


def MatrixKunminghuScheduler():
    if not buildEnv['GEM5_ENABLE_MATRIX']:
        fatal("Matrix scheduling requires a build with --enable-matrix")
    return _add_matrix(KunminghuScheduler(), 'intIQ3', 1)


def config_matrix(options, system):
    if not buildEnv['GEM5_ENABLE_MATRIX']:
        fatal("Matrix configuration requires a build with --enable-matrix")
    if options.xiangshan_ecore:
        fatal("Matrix configuration requires the Kunminghu O3 preset")
    if (getattr(options, 'ruby', False) or not options.caches or
            not options.l2cache or options.num_cpus != 1 or
            options.external_memory_system or options.memchecker):
        fatal("Matrix memory requires the single-core L1/shared-L2 topology")
    if not options.classic_l2:
        fatal("Matrix memory requires --classic-l2")
    if options.cacheline_size != 64:
        fatal("Matrix memory plans require 64-byte cache lines")
    for cpu in system.cpu:
        if not isinstance(cpu, MatrixXiangshanCore):
            fatal("Matrix configuration requires MatrixXiangshanCore")
        if int(cpu.numThreads) != 1:
            fatal("Matrix requires a single hardware thread")
        cpu.enableMatrixBackend = True
        cpu.enableMatrixMemPort = True
        cpu.scheduler = MatrixKunminghuScheduler()

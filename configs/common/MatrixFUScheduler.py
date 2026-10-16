"""Explicit Matrix scheduling resources for the Kunminghu O3 presets."""

from m5.defines import buildEnv
from m5.util import fatal

if not buildEnv['GEM5_ENABLE_MATRIX']:
    fatal("Matrix scheduling requires a build with --enable-matrix")

from m5.objects import IssuePort, IssueQue, SpecWakeupChannel
from m5.objects.MatrixFuncUnitConfig import (
    MatrixIntFU, MatrixReleaseFU, MatrixCsrFU, MatrixMmaFU,
    MatrixArithFU, MatrixSyncFU, MatrixMemFU,
)
from common.FUScheduler import IntRD, IntWR, KunminghuScheduler, KMHV3Scheduler


def RMiscRD(id, p):
    assert id < 16
    assert p < 4
    return (2 << 6) | (id << 2) | p


def RMiscWR(id, p):
    assert id < 16
    assert p < 4
    return (1 << 8) | (2 << 6) | (id << 2) | p


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
    return _add_matrix(KunminghuScheduler(), 'intIQ3', 1)


def MatrixKMHV3Scheduler():
    return _add_matrix(KMHV3Scheduler(), 'intIQ5', 0)



from m5.SimObject import SimObject
from m5.params import *
from m5.objects.FuncUnit import *
from m5.objects.FuncUnitConfig import *
from m5.objects.FuncScheduler import *

#  must be consistent with issue_queue.cc
maxTotalRFPorts = (1 << 6) - 1
# portid, priority
# smaller value get higher priority
def IntRD(id, p):
    # [7:6] [5:2] [1:0]
    assert id < 16
    assert p < 4
    ret = (0 << 6) | (id << 2) | (p)
    return ret

def FpRD(id, p):
    # [7:6] [5:2] [1:0]
    assert id < 16
    assert p < 4
    ret = (1 << 6) | (id << 2) | (p)
    return ret

class ECoreScheduler(Scheduler):
    IQs = [
        IssueQue(name='intIQ0' , inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU(), IntBRU()]),
            IssuePort(fu=[IntALU(), IntBRU()])
        ]),
        IssueQue(name='intIQ1' , inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU(), IntBRU()]),
            IssuePort(fu=[IntALU(), IntBRU()])
        ]),
        IssueQue(name='intIQ2' , inports=2, size=2*12, oports=[
            IssuePort(fu=[IntMult(), IntDiv(), IntMisc()])
        ]),
        IssueQue(name='memIQ0' , inports=2, size=2*16, oports=[
            IssuePort(fu=[ReadPort()])
        ]),
        IssueQue(name='memIQ1' , inports=2, size=2*16, oports=[
            IssuePort(fu=[RdWrPort()])
        ]),
        IssueQue(name='fpIQ0' , inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()]),
            IssuePort(fu=[FP_ALU(), FP_MAC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ1' , inports=2, size=18, oports=[
            IssuePort(fu=[FP_MISC(), FP_SLOW()])
        ], scheduleToExecDelay=3),
        IssueQue(name='vecIQ0' , inports=2, size=16, oports=[
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()])
        ], scheduleToExecDelay=3),
    ]
    xbarWakeup = True

class ECore2ReadScheduler(Scheduler):
    IQs = [
        IssueQue(name='intIQ0' , inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU(), IntBRU()]),
            IssuePort(fu=[IntALU(), IntBRU()])
        ]),
        IssueQue(name='intIQ1' , inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU(), IntBRU()]),
            IssuePort(fu=[IntALU(), IntBRU()])
        ]),
        IssueQue(name='intIQ2' , inports=2, size=2*12, oports=[
            IssuePort(fu=[IntMult(), IntDiv(), IntMisc()])
        ]),
        IssueQue(name='memIQ0' , inports=2, size=2*16, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()])
        ]),
        IssueQue(name='memIQ1' , inports=2, size=2*16, oports=[
            IssuePort(fu=[WritePort()])
        ]),
        IssueQue(name='fpIQ0' , inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()]),
            IssuePort(fu=[FP_ALU(), FP_MAC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ1' , inports=2, size=18, oports=[
            IssuePort(fu=[FP_MISC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ4' , inports=2, size=18, oports=[
            IssuePort(fu=[FP_SLOW()])
        ], scheduleToExecDelay=3),
        IssueQue(name='vecIQ0' , inports=2, size=16, oports=[
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()])
        ], scheduleToExecDelay=3),
    ]
    xbarWakeup = True


class KunminghuScheduler(Scheduler):
    __intIQs = [
        IssueQue(name='intIQ0', inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU(), IntMult()], rp=[IntRD(0, 0), IntRD(1, 0)]),
            IssuePort(fu=[IntBRU()], rp=[IntRD(6, 1), IntRD(7, 1)])
        ]),
        IssueQue(name='intIQ1', inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU(), IntMult()], rp=[IntRD(2, 0), IntRD(3, 0)]),
            IssuePort(fu=[IntBRU()], rp=[IntRD(4, 1), IntRD(5, 1)])
        ]),
        IssueQue(name='intIQ2', inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU()], rp=[IntRD(4, 0), IntRD(5, 0)]),
            IssuePort(fu=[IntBRU(), IntMisc()], rp=[IntRD(2, 1), IntRD(3, 1)])
        ]),
        IssueQue(name='intIQ3', inports=2, size=2*12, oports=[
            IssuePort(fu=[IntALU()], rp=[IntRD(6, 0), IntRD(7, 0)]),
            IssuePort(fu=[IntDiv()], rp=[IntRD(0, 1), IntRD(1, 1)])
        ])
    ]
    __memIQs = [
        IssueQue(name='load0', inports=2, size=16, oports=[
            IssuePort(fu=[ReadPort()], rp=[IntRD(8, 0)])
        ]),
        IssueQue(name='load1', inports=2, size=16, oports=[
            IssuePort(fu=[ReadPort()], rp=[IntRD(9, 0)])
        ]),
        IssueQue(name='load2', inports=2, size=16, oports=[
            IssuePort(fu=[ReadPort()], rp=[IntRD(10, 0)])
        ]),
        IssueQue(name='store0', inports=2, size=16, oports=[
            IssuePort(fu=[WritePort()], rp=[IntRD(7, 2)])
        ]),
        IssueQue(name='store1', inports=2, size=16, oports=[
            IssuePort(fu=[WritePort()], rp=[IntRD(6, 2)])
        ]),
        IssueQue(name='std0', inports=2, size=16, oports=[
            IssuePort(fu=[StoreDataPort()], rp=[IntRD(5,2), FpRD(9,0)])
        ]),
        IssueQue(name='std1', inports=2, size=16, oports=[
            IssuePort(fu=[StoreDataPort()], rp=[IntRD(3,2), FpRD(10,0)])
        ])
    ]
    __fpIQs = [
        IssueQue(name='fpIQ0', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MISC(), FP_MAC()], rp=[FpRD(0,0), FpRD(1, 0), FpRD(2,0)]),
            IssuePort(fu=[FP_SLOW()], rp=[FpRD(2,1), FpRD(5,1)])
        ], scheduleToExecDelay=2),
        IssueQue(name='fpIQ1', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()], rp=[FpRD(3,0), FpRD(4,0), FpRD(5,0)]),
            IssuePort(fu=[FP_SLOW()], rp=[FpRD(8,1), FpRD(9,1)]),
        ], scheduleToExecDelay=2),
        IssueQue(name='fpIQ2', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()], rp=[FpRD(6,0), FpRD(7,0), FpRD(8,0)])
        ], scheduleToExecDelay=2),
        IssueQue(name='vecIQ0', inports=5, size=16+16+10, oports=[
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()])
        ], scheduleToExecDelay=3)
    ]
    IQs = __intIQs + __memIQs + __fpIQs

    __int_bank = [i.name for i in __intIQs]
    __mem_bank = [i.name for i in __memIQs]
    __fp_bank = [i.name for i in __fpIQs]
    specWakeupNetwork = [
        SpecWakeupChannel(srcs=__int_bank + __mem_bank, dsts=__int_bank + __mem_bank),
        SpecWakeupChannel(srcs=__fp_bank, dsts=__fp_bank)
    ]

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # self.disableAllRegArb()

    def disableAllRegArb(self):
        print("Disable regfile arbitration")
        for iq in self.IQs:
            for port in iq.oports:
                port.rp.clear()

class KMHV3Scheduler(Scheduler):
    __intIQs = [
        IssueQue(name='intIQ0', inports=2, size=16, oports=[
            IssuePort(fu=[IntALU(), IntMult()],
                      rp=[IntRD(0, 0), IntRD(1, 0)])]),
        IssueQue(name='intIQ1', inports=2, size=16, oports=[
            IssuePort(fu=[IntALU(), IntMult()],
                      rp=[IntRD(2, 0), IntRD(3, 0)])]),
        IssueQue(name='intIQ2', inports=2, size=16, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(4, 0), IntRD(5, 0)])]),
        IssueQue(name='intIQ3', inports=2, size=16, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(6, 0), IntRD(7, 0)])]),
        IssueQue(name='intIQ4', inports=2, size=16, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(8, 0), IntRD(9, 0)])]),
        IssueQue(name='intIQ5', inports=2, size=16, oports=[
            IssuePort(fu=[IntALU(), IntDiv(), IntMisc()],
                      rp=[IntRD(10, 0), IntRD(11, 0)])]),
    ]
    __memIQs = [
        IssueQue(name='ld0', inports=2, size=16, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(12, 0)])]),
        IssueQue(name='ld1', inports=2, size=16, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(13, 0)])]),
        IssueQue(name='ld2', inports=2, size=16, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(14, 0)])]),
        IssueQue(name='sta0', inports=2, size=16, oports=[
            IssuePort(fu=[WritePort()],
                      rp=[IntRD(5, 1)])]),
        IssueQue(name='sta1', inports=2, size=16, oports=[
            IssuePort(fu=[WritePort()],
                      rp=[IntRD(7, 1)])]),
        IssueQue(name='std0', inports=2, size=16, oports=[
            IssuePort(fu=[StoreDataPort()],
                      rp=[IntRD(9, 1), FpRD(12, 0)])]),
        IssueQue(name='std1', inports=2, size=16, oports=[
            IssuePort(fu=[StoreDataPort()],
                      rp=[IntRD(11, 1), FpRD(13, 0)])]),
    ]
    __fpIQs = [
        IssueQue(name='fpIQ0', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MISC(), FP_MAC()],
                      rp=[FpRD(0,0), FpRD(1, 0), FpRD(2,0)]),
            IssuePort(fu=[FP_SLOW()],
                      rp=[FpRD(2,1), FpRD(5,1)])
        ]),
        IssueQue(name='fpIQ1', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(3,0), FpRD(4,0), FpRD(5,0)]),
            IssuePort(fu=[FP_SLOW()],
                      rp=[FpRD(8,1), FpRD(11,1)]),
        ]),
        IssueQue(name='fpIQ2', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(6,0), FpRD(7,0), FpRD(8,0)])
        ]),
        IssueQue(name='fpIQ3', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(9,0), FpRD(10,0), FpRD(11,0)])
        ]),
        IssueQue(name='vecIQ0', inports=5, size=16+16+10, oports=[
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()])
        ], scheduleToExecDelay=3),
    ]

    IQs = __intIQs + __memIQs + __fpIQs
    __int_bank = [i.name for i in __intIQs]
    __mem_bank = [i.name for i in __memIQs]
    __fp_bank = [i.name for i in __fpIQs]
    specWakeupNetwork = [
        SpecWakeupChannel(srcs=__int_bank + __mem_bank, dsts=__int_bank + __mem_bank),
        SpecWakeupChannel(srcs=__fp_bank, dsts=__fp_bank)
    ]

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # self.disableAllRegArb()

    def disableAllRegArb(self):
        print("Disable regfile arbitration")
        for iq in self.IQs:
            for port in iq.oports:
                port.rp.clear()

class IdealScheduler(Scheduler):
    __intIQs = [
        IssueQue(name='intIQ0', inports=2, size=2*24, oports=[
            IssuePort(fu=[IntALU(), IntMult()]),
            IssuePort(fu=[IntBRU()])
        ]),
        IssueQue(name='intIQ1', inports=2, size=2*24, oports=[
            IssuePort(fu=[IntALU(), IntMult()]),
            IssuePort(fu=[IntBRU()])
        ]),
        IssueQue(name='intIQ2', inports=2, size=2*24, oports=[
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntBRU(), IntMisc()])
        ]),
        IssueQue(name='intIQ3', inports=2, size=2*24, oports=[
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntDiv()])
        ])
    ]
    __memIQs = [
        IssueQue(name='load0', inports=6, size=3*32, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()])
        ]),
        IssueQue(name='store0', inports=4, size=2*32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()])
        ]),
        IssueQue(name='std0', inports=4, size=2*32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()])
        ])
    ]
    __fpIQs = [
        IssueQue(name='fpIQ0', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MISC(), FP_MAC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ1', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ2', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ3', inports=2, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()])
        ], scheduleToExecDelay=3),
        IssueQue(name='fpIQ4', inports=2, size=18, oports=[
            IssuePort(fu=[FP_SLOW()]),
            IssuePort(fu=[FP_SLOW()])
        ], scheduleToExecDelay=3),
        IssueQue(name='vecIQ0', inports=5, size=16+16+10, oports=[
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()]),
            IssuePort(fu=[SIMD_Unit()])
        ], scheduleToExecDelay=3),
    ]
    IQs = __intIQs + __memIQs + __fpIQs
    __int_bank = [i.name for i in __intIQs]
    __mem_bank = [i.name for i in __memIQs]
    __fp_bank = [i.name for i in __fpIQs]
    specWakeupNetwork = [
        SpecWakeupChannel(srcs=__int_bank + __mem_bank, dsts=__int_bank + __mem_bank),
        SpecWakeupChannel(srcs=__fp_bank, dsts=__fp_bank)
    ]

    useOldDisp = True


DefaultScheduler = KunminghuScheduler

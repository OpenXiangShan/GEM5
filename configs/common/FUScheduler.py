

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


# Experimental Scheduler Configurations for Performance Exploration

class KMHV3_NoRegArb(KMHV3Scheduler):
    """
    Experiment 1: Remove register file port arbitration constraints
    Target: Eliminate register port conflicts (highest potential)
    """
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.disableAllRegArb()  # Use existing function to remove all rp constraints

class KMHV3_BigIQ(KMHV3Scheduler):
    """
    Experiment 2: Increase IQ depths to approach Ideal scheduler
    Target: Larger instruction window (high potential)
    """
    __intIQs = [
        IssueQue(name='intIQ0', inports=2, size=32, oports=[
            IssuePort(fu=[IntALU(), IntMult()],
                      rp=[IntRD(0, 0), IntRD(1, 0)])]),
        IssueQue(name='intIQ1', inports=2, size=32, oports=[
            IssuePort(fu=[IntALU(), IntMult()],
                      rp=[IntRD(2, 0), IntRD(3, 0)])]),
        IssueQue(name='intIQ2', inports=2, size=32, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(4, 0), IntRD(5, 0)])]),
        IssueQue(name='intIQ3', inports=2, size=32, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(6, 0), IntRD(7, 0)])]),
        IssueQue(name='intIQ4', inports=2, size=32, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(8, 0), IntRD(9, 0)])]),
        IssueQue(name='intIQ5', inports=2, size=32, oports=[
            IssuePort(fu=[IntALU(), IntDiv(), IntMisc()],
                      rp=[IntRD(10, 0), IntRD(11, 0)])]),
    ]
    __memIQs = [
        IssueQue(name='ld0', inports=2, size=32, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(12, 0)])]),
        IssueQue(name='ld1', inports=2, size=32, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(13, 0)])]),
        IssueQue(name='ld2', inports=2, size=32, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(14, 0)])]),
        IssueQue(name='sta0', inports=2, size=32, oports=[
            IssuePort(fu=[WritePort()],
                      rp=[IntRD(5, 1)])]),
        IssueQue(name='sta1', inports=2, size=32, oports=[
            IssuePort(fu=[WritePort()],
                      rp=[IntRD(7, 1)])]),
        IssueQue(name='std0', inports=2, size=32, oports=[
            IssuePort(fu=[StoreDataPort()],
                      rp=[IntRD(9, 1), FpRD(12, 0)])]),
        IssueQue(name='std1', inports=2, size=32, oports=[
            IssuePort(fu=[StoreDataPort()],
                      rp=[IntRD(11, 1), FpRD(13, 0)])]),
    ]
    __fpIQs = [
        IssueQue(name='fpIQ0', inports=2, size=32, oports=[
            IssuePort(fu=[FP_ALU(), FP_MISC(), FP_MAC()],
                      rp=[FpRD(0,0), FpRD(1, 0), FpRD(2,0)]),
            IssuePort(fu=[FP_SLOW()],
                      rp=[FpRD(2,1), FpRD(5,1)])
        ]),
        IssueQue(name='fpIQ1', inports=2, size=32, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(3,0), FpRD(4,0), FpRD(5,0)]),
            IssuePort(fu=[FP_SLOW()],
                      rp=[FpRD(8,1), FpRD(11,1)]),
        ]),
        IssueQue(name='fpIQ2', inports=2, size=32, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(6,0), FpRD(7,0), FpRD(8,0)])
        ]),
        IssueQue(name='fpIQ3', inports=2, size=32, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(9,0), FpRD(10,0), FpRD(11,0)])
        ]),
        IssueQue(name='vecIQ0', inports=5, size=42, oports=[
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

class KMHV3_WideInport(KMHV3Scheduler):
    """
    Experiment 3: Increase dispatch bandwidth (inports)
    Target: Reduce dispatch bottleneck (medium-high potential)
    """
    __intIQs = [
        IssueQue(name='intIQ0', inports=4, size=16, oports=[
            IssuePort(fu=[IntALU(), IntMult()],
                      rp=[IntRD(0, 0), IntRD(1, 0)])]),
        IssueQue(name='intIQ1', inports=4, size=16, oports=[
            IssuePort(fu=[IntALU(), IntMult()],
                      rp=[IntRD(2, 0), IntRD(3, 0)])]),
        IssueQue(name='intIQ2', inports=4, size=16, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(4, 0), IntRD(5, 0)])]),
        IssueQue(name='intIQ3', inports=4, size=16, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(6, 0), IntRD(7, 0)])]),
        IssueQue(name='intIQ4', inports=4, size=16, oports=[
            IssuePort(fu=[IntALU(), IntBRU()],
                      rp=[IntRD(8, 0), IntRD(9, 0)])]),
        IssueQue(name='intIQ5', inports=4, size=16, oports=[
            IssuePort(fu=[IntALU(), IntDiv(), IntMisc()],
                      rp=[IntRD(10, 0), IntRD(11, 0)])]),
    ]
    __memIQs = [
        IssueQue(name='ld0', inports=4, size=16, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(12, 0)])]),
        IssueQue(name='ld1', inports=4, size=16, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(13, 0)])]),
        IssueQue(name='ld2', inports=4, size=16, oports=[
            IssuePort(fu=[ReadPort()],
                      rp=[IntRD(14, 0)])]),
        IssueQue(name='sta0', inports=4, size=16, oports=[
            IssuePort(fu=[WritePort()],
                      rp=[IntRD(5, 1)])]),
        IssueQue(name='sta1', inports=4, size=16, oports=[
            IssuePort(fu=[WritePort()],
                      rp=[IntRD(7, 1)])]),
        IssueQue(name='std0', inports=4, size=16, oports=[
            IssuePort(fu=[StoreDataPort()],
                      rp=[IntRD(9, 1), FpRD(12, 0)])]),
        IssueQue(name='std1', inports=4, size=16, oports=[
            IssuePort(fu=[StoreDataPort()],
                      rp=[IntRD(11, 1), FpRD(13, 0)])]),
    ]
    __fpIQs = [
        IssueQue(name='fpIQ0', inports=4, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MISC(), FP_MAC()],
                      rp=[FpRD(0,0), FpRD(1, 0), FpRD(2,0)]),
            IssuePort(fu=[FP_SLOW()],
                      rp=[FpRD(2,1), FpRD(5,1)])
        ]),
        IssueQue(name='fpIQ1', inports=4, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(3,0), FpRD(4,0), FpRD(5,0)]),
            IssuePort(fu=[FP_SLOW()],
                      rp=[FpRD(8,1), FpRD(11,1)]),
        ]),
        IssueQue(name='fpIQ2', inports=4, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(6,0), FpRD(7,0), FpRD(8,0)])
        ]),
        IssueQue(name='fpIQ3', inports=4, size=18, oports=[
            IssuePort(fu=[FP_ALU(), FP_MAC()],
                      rp=[FpRD(9,0), FpRD(10,0), FpRD(11,0)])
        ]),
        IssueQue(name='vecIQ0', inports=8, size=16+16+10, oports=[
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

class KMHV3_MemOpt(KMHV3Scheduler):
    """
    Experiment 4: Optimize memory subsystem like Ideal scheduler
    Target: Improve memory parallelism (medium-high potential)
    """
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
        # Consolidate load IQs with more ports (like Ideal)
        IssueQue(name='load0', inports=6, size=48, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()])
        ]),
        # Consolidate store IQs
        IssueQue(name='store0', inports=4, size=32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()])
        ]),
        IssueQue(name='std0', inports=4, size=32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()])
        ])
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

class UnifyIQ(Scheduler):
    """
    Experiment 5: Unified IQ for maximum performance upper bound
    Target: Test theoretical maximum with unified integer IQ and no constraints
    """
    __intIQs = [
        IssueQue(name='intIQ0', inports=8, size=48, oports=[
            # 6 ALU ports for maximum integer throughput
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            # 3 BRU ports for branch-heavy workloads
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            # 2 Mult ports for multiply-heavy workloads
            IssuePort(fu=[IntMult()]),
            IssuePort(fu=[IntMult()]),
            # 1 Div/Misc port (div is typically low throughput)
            IssuePort(fu=[IntDiv(), IntMisc()]),
        ])
    ]

    __memIQs = [
        IssueQue(name='load0', inports=6, size=48, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
        ]),
        IssueQue(name='sta0', inports=4, size=32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()]),
        ]),
        IssueQue(name='std0', inports=4, size=32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()]),
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
        IssueQue(name='vecIQ0', inports=5, size=42, oports=[
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

# UnifyIQ to Ideal experiment, for performance analysis
# Control variables: keep memory and floating-point IQ configurations unchanged,
# only change the integer IQ organization

class UnifyIQ_2Split(Scheduler):
    """
    experiment 1: UnifyIQ to Ideal, ALU/MULT vs BRU/DIV
    target: test the impact of separating ALU/MULT vs BRU/DIV
    """
    __intIQs = [
        # IQ0: ALU + MULT (compute-intensive)
        IssueQue(name='intIQ0', inports=6, size=32, oports=[
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntMult()]),
            IssuePort(fu=[IntMult()]),
        ]),
        # IQ1: BRU + DIV/MISC (control and special operations)
        IssueQue(name='intIQ1', inports=4, size=24, oports=[
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntDiv(), IntMisc()]),
        ])
    ]

    # reuse the memory and floating-point configurations from UnifyIQ
    __memIQs = [
        IssueQue(name='load0', inports=6, size=48, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
        ]),
        IssueQue(name='sta0', inports=4, size=32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()]),
        ]),
        IssueQue(name='std0', inports=4, size=32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()]),
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
        IssueQue(name='vecIQ0', inports=5, size=42, oports=[
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

class UnifyIQ_3Split(Scheduler):
    """
    experiment 2: UnifyIQ to Ideal, ALU vs MULT vs BRU/DIV
    target: test the impact of separating ALU vs MULT vs BRU/DIV
    """
    __intIQs = [
        # IQ0: dedicated ALU queue
        IssueQue(name='intIQ0', inports=4, size=24, oports=[
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
        ]),
        # IQ1: dedicated MULT queue
        IssueQue(name='intIQ1', inports=3, size=20, oports=[
            IssuePort(fu=[IntMult()]),
            IssuePort(fu=[IntMult()]),
        ]),
        # IQ2: BRU + DIV/MISC queue
        IssueQue(name='intIQ2', inports=4, size=24, oports=[
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntDiv(), IntMisc()]),
        ])
    ]

    # reuse the same memory and floating-point configurations
    __memIQs = [
        IssueQue(name='load0', inports=6, size=48, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
        ]),
        IssueQue(name='sta0', inports=4, size=32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()]),
        ]),
        IssueQue(name='std0', inports=4, size=32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()]),
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
        IssueQue(name='vecIQ0', inports=5, size=42, oports=[
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

class UnifyIQ_4Split(Scheduler):
    """
    experiment 3: UnifyIQ to Ideal, ALU vs MULT vs BRU/DIV
    target: test the impact of separating ALU vs MULT vs BRU/DIV
    """
    __intIQs = [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=3, size=20, oports=[
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
        ]),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=3, size=20, oports=[
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
        ]),
        # IQ2: MULT dedicated queue
        IssueQue(name='intIQ2', inports=3, size=18, oports=[
            IssuePort(fu=[IntMult()]),
            IssuePort(fu=[IntMult()]),
        ]),
        # IQ3: BRU + DIV/MISC queue
        IssueQue(name='intIQ3', inports=4, size=22, oports=[
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntDiv(), IntMisc()]),
        ])
    ]

    # reuse the same memory and floating-point configurations
    __memIQs = [
        IssueQue(name='load0', inports=6, size=48, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
        ]),
        IssueQue(name='sta0', inports=4, size=32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()]),
        ]),
        IssueQue(name='std0', inports=4, size=32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()]),
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
        IssueQue(name='vecIQ0', inports=5, size=42, oports=[
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

class UnifyIQ_ReducedPorts(Scheduler):
    """
    experiment 4: UnifyIQ to Ideal, ALU vs MULT vs BRU/DIV
    target: test the impact of separating ALU vs MULT vs BRU/DIV
    """
    __intIQs = [
        IssueQue(name='intIQ0', inports=6, size=48, oports=[
            # reduce to the configuration close to Ideal: 4ALU + 2MULT + 1BRU + 1DIV
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntALU()]),
            IssuePort(fu=[IntMult()]),
            IssuePort(fu=[IntMult()]),
            IssuePort(fu=[IntBRU()]),
            IssuePort(fu=[IntDiv(), IntMisc()]),
        ])
    ]

    # reuse the same memory and floating-point configurations
    __memIQs = [
        IssueQue(name='load0', inports=6, size=48, oports=[
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
            IssuePort(fu=[ReadPort()]),
        ]),
        IssueQue(name='sta0', inports=4, size=32, oports=[
            IssuePort(fu=[WritePort()]),
            IssuePort(fu=[WritePort()]),
        ]),
        IssueQue(name='std0', inports=4, size=32, oports=[
            IssuePort(fu=[StoreDataPort()]),
            IssuePort(fu=[StoreDataPort()]),
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
        IssueQue(name='vecIQ0', inports=5, size=42, oports=[
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

DefaultScheduler = KunminghuScheduler

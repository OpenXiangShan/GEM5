

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

# Common configurations for UnifyIQ variants to reduce code duplication
def get_unify_mem_config():
    """Memory IQ configuration used by all UnifyIQ variants"""
    return [
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

def get_unify_fp_config():
    """Floating-point IQ configuration used by all UnifyIQ variants"""
    return [
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

def create_issue_ports(fu_list, count):
    """Helper function to create multiple IssuePort instances"""
    return [IssuePort(fu=fu_list) for _ in range(count)]

# Integer IQ configurations for different UnifyIQ variants
INT_IQ_CONFIGS = {
    '2split': [
        # IQ0: ALU + MULT (compute-intensive operations)
        IssueQue(name='intIQ0', inports=6, size=32, oports=
            create_issue_ports([IntALU()], 4) +
            create_issue_ports([IntMult()], 2)
        ),
        # IQ1: BRU + DIV/MISC (control and special operations)
        IssueQue(name='intIQ1', inports=4, size=24, oports=
            create_issue_ports([IntBRU()], 3) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '3split': [
        # IQ0: dedicated ALU queue
        IssueQue(name='intIQ0', inports=4, size=24, oports=
            create_issue_ports([IntALU()], 4)
        ),
        # IQ1: dedicated MULT queue
        IssueQue(name='intIQ1', inports=3, size=20, oports=
            create_issue_ports([IntMult()], 2)
        ),
        # IQ2: BRU + DIV/MISC queue
        IssueQue(name='intIQ2', inports=4, size=24, oports=
            create_issue_ports([IntBRU()], 3) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '4split': [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=3, size=20, oports=
            create_issue_ports([IntALU()], 3)
        ),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=3, size=20, oports=
            create_issue_ports([IntALU()], 3)
        ),
        # IQ2: MULT dedicated queue
        IssueQue(name='intIQ2', inports=3, size=18, oports=
            create_issue_ports([IntMult()], 2)
        ),
        # IQ3: BRU + DIV/MISC queue
        IssueQue(name='intIQ3', inports=4, size=22, oports=
            create_issue_ports([IntBRU()], 3) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '5split': [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ2: ALU queue 3 (3 ports)
        IssueQue(name='intIQ2', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ3: ALU queue 4 (3 ports)
        IssueQue(name='intIQ3', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '5split_opt': [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ2: ALU queue 3 (2 ports) - BRU
        IssueQue(name='intIQ2', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1)
        ),
        # IQ3: ALU queue 4 (3 ports)
        IssueQue(name='intIQ3', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '5split_opt2': [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ2: ALU queue 3 (2 ports)
        IssueQue(name='intIQ2', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1)
        ),
        # IQ3: ALU queue 4 (2 ports) - ALU
        IssueQue(name='intIQ3', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '6split': [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=2, size=16, oports=
            create_issue_ports([IntALU(), IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=2, size=16, oports=
            create_issue_ports([IntALU(), IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ2: ALU queue 3 (3 ports)
        IssueQue(name='intIQ2', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ3: ALU queue 4 (3 ports)
        IssueQue(name='intIQ3', inports=2, size=16, oports=
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntALU()], 1) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    '7split': [
        # IQ0: ALU queue 1 (3 ports)
        IssueQue(name='intIQ0', inports=2, size=16, oports=
            create_issue_ports([IntALU(), IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ1: ALU queue 2 (3 ports)
        IssueQue(name='intIQ1', inports=2, size=16, oports=
            create_issue_ports([IntALU(), IntMult()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ2: ALU queue 3 (3 ports)
        IssueQue(name='intIQ2', inports=2, size=16, oports=
            create_issue_ports([IntALU(), IntALU()], 1) +
            create_issue_ports([IntBRU()], 1)
        ),
        # IQ3: ALU queue 4 (3 ports)
        IssueQue(name='intIQ3', inports=2, size=16, oports=
            create_issue_ports([IntALU(), IntALU()], 1) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ],
    'reduced_ports': [
        # Unified IQ with port count close to Ideal: 4ALU + 2MULT + 1BRU + 1DIV
        IssueQue(name='intIQ0', inports=6, size=48, oports=
            create_issue_ports([IntALU()], 4) +
            create_issue_ports([IntMult()], 2) +
            create_issue_ports([IntBRU()], 1) +
            create_issue_ports([IntDiv(), IntMisc()], 1)
        )
    ]
}

class UnifyIQBase(Scheduler):
    """Base class for UnifyIQ variants to reduce code duplication"""

    def __init__(self, config_name, *args, **kwargs):
        self._intIQs = INT_IQ_CONFIGS[config_name]
        self._memIQs = get_unify_mem_config()
        self._fpIQs = get_unify_fp_config()

        self.IQs = self._intIQs + self._memIQs + self._fpIQs
        self._int_bank = [i.name for i in self._intIQs]
        self._mem_bank = [i.name for i in self._memIQs]
        self._fp_bank = [i.name for i in self._fpIQs]
        self.specWakeupNetwork = [
            SpecWakeupChannel(srcs=self._int_bank + self._mem_bank,
                            dsts=self._int_bank + self._mem_bank),
            SpecWakeupChannel(srcs=self._fp_bank, dsts=self._fp_bank)
        ]

        super().__init__(*args, **kwargs)

# UnifyIQ to Ideal progressive experiment configurations
# Control variables: keep memory and floating-point IQ configurations unchanged,
# only change the integer IQ organization

class UnifyIQ_2Split(UnifyIQBase):
    """
    Experiment 1: UnifyIQ to Ideal - 2-way split (ALU/MULT vs BRU/DIV)
    Target: test the impact of separating compute vs control operations
    """
    def __init__(self, *args, **kwargs):
        super().__init__('2split', *args, **kwargs)

class UnifyIQ_3Split(UnifyIQBase):
    """
    Experiment 2: UnifyIQ to Ideal - 3-way split (ALU vs MULT vs BRU/DIV)
    Target: test the impact of fine-grained functional unit separation
    """
    def __init__(self, *args, **kwargs):
        super().__init__('3split', *args, **kwargs)

class UnifyIQ_4Split(UnifyIQBase):
    """
    Experiment 3: UnifyIQ to Ideal - 4-way split (dual ALU + MULT + BRU/DIV)
    Target: test the impact of distributed ALU with more IQ counts
    """
    def __init__(self, *args, **kwargs):
        super().__init__('4split', *args, **kwargs)

class UnifyIQ_5Split(UnifyIQBase):
    """
    Experiment 4: UnifyIQ to Ideal - 5-way split (ALU vs MULT vs BRU/DIV)
    Target: test the impact of fine-grained functional unit separation
    """
    def __init__(self, *args, **kwargs):
        super().__init__('5split', *args, **kwargs)

class UnifyIQ_5SplitOpt(UnifyIQBase):
    """
    Experiment 5: UnifyIQ to Ideal - 5-way split (ALU vs MULT vs BRU/DIV)
    Target: test the impact of fine-grained functional unit separation
    """
    def __init__(self, *args, **kwargs):
        super().__init__('5split_opt', *args, **kwargs)

class UnifyIQ_5SplitOpt2(UnifyIQBase):

    """
    Experiment 6: UnifyIQ to Ideal - 5-way split (ALU vs MULT vs BRU/DIV)
    Target: test the impact of fine-grained functional unit separation
    """
    def __init__(self, *args, **kwargs):
        super().__init__('5split_opt2', *args, **kwargs)

class UnifyIQ_6Split(UnifyIQBase):
    """
    Experiment 5: UnifyIQ to Ideal - 6-way split (ALU vs MULT vs BRU/DIV)
    Target: test the impact of fine-grained functional unit separation
    """
    def __init__(self, *args, **kwargs):
        super().__init__('6split', *args, **kwargs)

class UnifyIQ_7Split(UnifyIQBase):

    """
    Experiment 6: UnifyIQ to Ideal - 7-way split (ALU vs MULT vs BRU/DIV)
    Target: test the impact of fine-grained functional unit separation
    """
    def __init__(self, *args, **kwargs):
        super().__init__('7split', *args, **kwargs)

class UnifyIQ_ReducedPorts(UnifyIQBase):
    """
    Experiment 6: UnifyIQ to Ideal - unified IQ with reduced port count
    Target: test the trade-off between unified scheduling vs port count
    """
    def __init__(self, *args, **kwargs):
        super().__init__('reduced_ports', *args, **kwargs)

DefaultScheduler = KunminghuScheduler

import argparse
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath, fatal, warn
from m5.util.fdthelper import *

addToPath('../')

from ruby import Ruby

from common.FSConfig import *
from common.SysPaths import *
from common.Benchmarks import *
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import MemConfig
from common import ObjectList
from common import XSConfig
from common.Caches import *
from common import Options
from common.xiangshan import *

def autoCalibrateParams(args, system):
    for cpu in system.cpu:
        cpu.fetchWidth=8
        cpu.fetchQueueSize=64
        cpu.decodeWidth=6
        cpu.renameWidth=6
        cpu.commitWidth=8
        cpu.LQEntries=72
        cpu.SQEntries=56
        cpu.SbufferEntries=16
        cpu.SbufferEvictThreshold=7
        cpu.RARQEntries=72
        cpu.RAWQEntries=32
        cpu.numROBEntries=160
        cpu.numPhysIntRegs=224
        cpu.numPhysFloatRegs=192
        cpu.numPhysVecRegs=128







if __name__ == '__m5_main__':
    args = xiangshan_system_init()

    args.enable_difftest = False
    args.raw_cpt = True
    args.no_pf = True

    # l1cache prefetcher use stream, stride
    # l2cache prefetcher use pht, bop, cmc
    # disable l1prefetcher store pf train
    # disable l1 berti, l2 cdp
    args.l2_hwp_type = "L2CompositeWithWorkerPrefetcher"
    #args.pht_pf_level = 2
    #args.kmh_align = True

    assert not args.external_memory_system

    test_mem_mode = 'timing'

    # override cpu class and clock
    if args.xiangshan_ecore:
        FutureClass = None
        args.cpu_clock = '2.4GHz'
    else:
        FutureClass = None

    test_sys = build_xiangshan_system(args)
    autoCalibrateParams(args, test_sys)

    root = Root(full_system=True, system=test_sys)

    Simulation.run_vanilla(args, root, test_sys, FutureClass)
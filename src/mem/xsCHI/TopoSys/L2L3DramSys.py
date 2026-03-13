from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.DDRWrapper import DDRWrapper
from m5.objects.CHI_L2 import CHI_L2
from m5.objects.CHI_L3 import CHI_L3


class L2L3DramSys(ClockedObject):
    type = 'L2L3DramSys'
    cxx_header = 'mem/xsCHI/TopoSys/L2L3DramSys.hh'
    cxx_class = 'gem5::xsCHI::L2L3DramSys'

    cpu_side_port = ResponsePort("port for receiving requests from the CPU or other requestor")
    mem_side_port = RequestPort("This port is only used for redirecting uncached accesses")
    dramsim3 = Param.DDRWrapper('DDR Wrapper')
    L2Wrapper = Param.CHI_L2('L2 Wrapper')
    L3 = Param.CHI_L3('CHI L3 Wrapper')

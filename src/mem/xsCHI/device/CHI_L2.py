from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIBridge import *
class CHI_L2(ClockedObject):
    type = 'CHI_L2'
    cxx_header = "mem/xsCHI/device/CHI_L2.hh"
    cxx_class = 'gem5::xsCHI::CHI_L2'

    cpu_side_port = ResponsePort("Port for receiving requests from upper-level caches")
    mem_side_port = RequestPort("Port for sending uncached requests downstream")

    RNBridge = Param.CHIBridge("CHI CHIBridge for L2")
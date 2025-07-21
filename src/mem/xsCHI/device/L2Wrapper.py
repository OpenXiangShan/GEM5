from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIBridge import *
class L2Wrapper(ClockedObject):
    type = 'L2Wrapper'
    cxx_header = "mem/xsCHI/device/L2Wrapper.hh"
    cxx_class = 'gem5::xsCHI::L2Wrapper'

    RNBridge = Param.CHIBridge("CHI CHIBridge for L2")
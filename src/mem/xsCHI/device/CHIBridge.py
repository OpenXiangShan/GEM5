from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIPort import *
class CHIBridge(ClockedObject):
    type = 'CHIBridge'
    cxx_header = "mem/xsCHI/device/CHIBridge.hh"
    cxx_class = 'gem5::xsCHI::CHIBridge'

    networkPort = Param.CHIPort("networkPort pointer")
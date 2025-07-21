from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIPort import *
class FakeL3(ClockedObject):
    type = 'FakeL3'
    cxx_header = "mem/xsCHI/device/HNF.hh"
    cxx_class = 'gem5::xsCHI::FakeL3'
    L2side = Param.CHIPort("L2 port pointer")
    Dramside = Param.CHIPort("Dramside port pointer")
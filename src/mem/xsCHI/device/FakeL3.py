from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIPort import *
class FakeL3(ClockedObject):
    type = 'FakeL3'
    cxx_header = "mem/xsCHI/device/fakeL3.hh"
    cxx_class = 'gem5::xsCHI::FakeL3'
    networkPort = Param.CHIPort("networkPort pointer")

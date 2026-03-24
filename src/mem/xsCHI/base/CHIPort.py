from m5.params import *
from m5.objects.ClockedObject import ClockedObject

class CHIPort(ClockedObject):
    type = 'CHIPort'
    cxx_header = "mem/xsCHI/base/CHIPort.hh"
    cxx_class = 'gem5::xsCHI::CHIPort'

    recv_buffer_size = Param.Unsigned(4, "DDRWrapperBufferSize")
    transfer_latency_cycles = Param.Unsigned(
        1, "Port receive-to-handle latency in cycles")

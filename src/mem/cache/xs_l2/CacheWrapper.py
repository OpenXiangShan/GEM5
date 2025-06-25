from m5.params import *
from m5.proxy import *

from m5.objects.ClockedObject import ClockedObject

class CacheWrapper(ClockedObject):
    type = 'CacheWrapper'
    cxx_header = "mem/cache/xs_l2/CacheWrapper.hh"
    cxx_class = 'gem5::CacheWrapper'

    cpu_side = ResponsePort("CPU side port, receives requests")
    mem_side = RequestPort("Memory side port, sends requests")

    inner_cpu_port = RequestPort("Port to connect to inner cache's CPU side")
    inner_mem_port = ResponsePort("Port to connect to inner cache's mem side")

class L2CacheWrapper(CacheWrapper):
    type = 'L2CacheWrapper'
    cxx_header = "mem/cache/xs_l2/L2CacheWrapper.hh"
    cxx_class = 'gem5::L2CacheWrapper'
    buffer_size = Param.Unsigned(4, "Size of the request buffer")
    pipeline_depth = Param.Unsigned(5, "Depth of the response pipeline")

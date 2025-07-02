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

class L2CacheSlice(CacheWrapper):
    type = 'L2CacheSlice'
    cxx_header = "mem/cache/xs_l2/L2CacheSlice.hh"
    cxx_class = 'gem5::L2CacheSlice'
    buffer_size = Param.Unsigned(4, "Size of the request buffer")
    pipeline_depth = Param.Unsigned(5, "Depth of the response pipeline")

class L2CacheWrapper(ClockedObject):
    type = 'L2CacheWrapper'
    cxx_header = "mem/cache/xs_l2/L2CacheWrapper.hh"
    cxx_class = 'gem5::L2CacheWrapper'

    cpu_side = ResponsePort("CPU side port, receives requests from L1/CPU")

    # Ports to connect to the slices' CPU side
    slice_cpuside_ports = VectorRequestPort(
        "Ports to connect to the slices' CPU-side")

    num_slices = Param.Unsigned("Number of slices")
    block_bits = Param.Unsigned(6, "Log2 of cache block size in bytes")

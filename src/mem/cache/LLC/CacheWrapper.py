from m5.params import *
from m5.proxy import *
from m5.SimObject import *

from m5.objects.ClockedObject import ClockedObject
from m5.objects.Prefetcher import *

class CacheWrapper(ClockedObject):
    type = 'CacheWrapper'
    cxx_header = "mem/cache/LLC/CacheWrapper.hh"
    cxx_class = 'gem5::CacheWrapper'

    cpu_side = ResponsePort("CPU side port, receives requests")
    mem_side = RequestPort("Memory side port, sends requests")

    inner_cpu_port = RequestPort("Port to connect to inner cache's CPU side")
    inner_mem_port = ResponsePort("Port to connect to inner cache's mem side")

class L3CacheSlice(CacheWrapper):
    type = 'L3CacheSlice'
    cxx_header = "mem/cache/LLC/L3CacheSlice.hh"
    cxx_class = 'gem5::L3CacheSlice'
    cxx_exports = [
        PyBindMethod("setCacheAccessor"),
    ]

    buffer_size = Param.Unsigned(4, "Size of the request buffer")
    pipeline_depth = Param.Unsigned(5, "Depth of the response pipeline")

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._cache_accessor = None

    # Override the normal SimObject::regProbeListeners method and
    # add the cache accessors to the L2CacheSlice.
    def regProbeListeners(self):
        if self._cache_accessor is not None:
            print("Registering inner cache accessor for L3CacheSlice {}".format(self))
            self.getCCObject().setCacheAccessor(self._cache_accessor.getCCObject())
        self.getCCObject().regProbeListeners()

    def setCacheAccessor(self, accessor):
        self._cache_accessor = accessor

class L3CacheWrapper(ClockedObject):
    type = 'L3CacheWrapper'
    cxx_header = "mem/cache/LLC/L3CacheWrapper.hh"
    cxx_class = 'gem5::L3CacheWrapper'
    cxx_exports = [
        PyBindMethod("addCacheAccessor"),
        PyBindMethod("addSliceAccessor"),
    ]

    cpu_side = ResponsePort("CPU side port, receives requests from L2(toL3bus)")
    mem_side = RequestPort("Memory side port, sends requests")

    # Ports to connect to the slices' CPU side
    slice_cpuside_ports = VectorRequestPort(
        "Ports to connect to the slices' CPU-side")

    num_slices = Param.Unsigned("Number of slices")
    cache_size = Param.MemorySize("Size of the cache in bytes")
    cache_assoc = Param.Unsigned("Associativity of the cache")
    block_bits = Param.Unsigned(6, "Log2 of cache block size in bytes")

    pip_data_write_stage = Param.Unsigned(3, "Pipeline data write stage")
    dir_read_bypass = Param.Bool(False, "Whether to bypass directory read when set address is same")

    prefetcher = Param.BasePrefetcher(L3CompositeWithWorkerPrefetcher(), "Prefetcher attached to L3CacheWrapper")
    system = Param.System(Parent.any, "System we belong to")

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._cache_accessors = []
        self._slice_accessors = []

    # Override the normal SimObject::regProbeListeners method and
    # add the slice accessors to the L3CacheWrapper.
    def regProbeListeners(self):
        print("Registering inner cache accessors for L3CacheWrapper {}".format(self))
        for accessor in self._cache_accessors:
            self.getCCObject().addCacheAccessor(accessor.getCCObject())
        for _slice in self._slice_accessors:
            self.getCCObject().addSliceAccessor(_slice.getCCObject())
        self.getCCObject().regProbeListeners()

    def addCacheAccessor(self, accessor):
        print("Adding cache accessor to L3CacheWrapper {}".format(self))
        self._cache_accessors.append(accessor)

    def addSliceAccessor(self, slice):
        print("Adding slice accessor to L3CacheWrapper {}".format(self))
        self._slice_accessors.append(slice)

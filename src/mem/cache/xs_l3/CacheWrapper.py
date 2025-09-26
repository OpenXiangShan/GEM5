from m5.params import *
from m5.proxy import *
from m5.SimObject import *

from m5.objects.ClockedObject import ClockedObject
from m5.objects.Prefetcher import *

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
            print("Registering inner cache accessor for L2CacheSlice {}".format(self))
            self.getCCObject().setCacheAccessor(self._cache_accessor.getCCObject())
        self.getCCObject().regProbeListeners()

    def setCacheAccessor(self, accessor):
        self._cache_accessor = accessor

class L2CacheWrapper(ClockedObject):
    type = 'L2CacheWrapper'
    cxx_header = "mem/cache/xs_l2/L2CacheWrapper.hh"
    cxx_class = 'gem5::L2CacheWrapper'
    cxx_exports = [
        PyBindMethod("addCacheAccessor"),
        PyBindMethod("addSliceAccessor"),
    ]

    cpu_side = ResponsePort("CPU side port, receives requests from L1/CPU")

    # Ports to connect to the slices' CPU side
    slice_cpuside_ports = VectorRequestPort(
        "Ports to connect to the slices' CPU-side")

    num_slices = Param.Unsigned("Number of slices")
    cache_size = Param.MemorySize("Cache size")
    cache_assoc = Param.Unsigned("Cache associativity")
    block_bits = Param.Unsigned(6, "Log2 of cache block size in bytes")

    pipe_dir_write_stage = Param.Unsigned(3, "the stage of directory write in L2MainPipe")
    dir_read_bypass = Param.Bool(False, "whether to bypass the directory read when set address is the same")

    # Number of DataSram banks (divides the Data Sets into banks)
    # Must be power of 2 (1, 2, 4, 8, etc.), default is 1 (single bank)
    data_sram_banks = Param.Unsigned(1, "Number of DataSram banks for dividing Data Sets")

    # Number of DirSram banks (divides the Dir Sets into banks)
    # Must be power of 2 (1, 2, 4, 8, etc.), default is 1 (single bank)
    dir_sram_banks = Param.Unsigned(1, "Number of DirSram banks for dividing Dir Sets")

    prefetcher = Param.BasePrefetcher(L2CompositeWithWorkerPrefetcher(), "Prefetcher attached to L2CacheWrapper")
    system = Param.System(Parent.any, "System we belong to")

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._cache_accessors = []
        self._slice_accessors = []

    # Override the normal SimObject::regProbeListeners method and
    # add the slice accessors to the L2CacheWrapper.
    def regProbeListeners(self):
        print("Registering inner cache accessors for L2CacheWrapper {}".format(self))
        for accessor in self._cache_accessors:
            self.getCCObject().addCacheAccessor(accessor.getCCObject())
        for _slice in self._slice_accessors:
            self.getCCObject().addSliceAccessor(_slice.getCCObject())
        self.getCCObject().regProbeListeners()

    def addCacheAccessor(self, accessor):
        self._cache_accessors.append(accessor)

    def addSliceAccessor(self, slice):
        self._slice_accessors.append(slice)


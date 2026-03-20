from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.DDRWrapper import DDRWrapper
from m5.objects.CHI_L2 import CHI_L2
from m5.objects.CHI_L3 import CHI_L3
from m5.objects.MeshNode import MeshNode


class L2L3DramSys3x3(ClockedObject):
    type = 'L2L3DramSys3x3'
    cxx_header = 'mem/xsCHI/TopoSys/L2L3DramSys3x3.hh'
    cxx_class = 'gem5::xsCHI::L2L3DramSys3x3'

    cpu_side_port = ResponsePort("port for receiving requests from the CPU or other requestor")
    mem_side_port = RequestPort("This port is only used for redirecting uncached accesses")
    dramsim3 = Param.DDRWrapper('DDR Wrapper')
    L2Wrapper = Param.CHI_L2('L2 Wrapper')
    ShadowRNBridges = VectorParam.CHIBridge([], "Shadow RN bridges")
    shadow_attach_points = VectorParam.String(
        [], "Per-shadow mesh attach point, e.g. mesh8.local0")
    L3 = Param.CHI_L3('CHI L3 Wrapper')

    # 3x3 mesh topology (row-major):
    # M6(0,2) -- M7(1,2) -- M8(2,2)
    #   |          |          |
    # M3(0,1) -- M4(1,1) -- M5(2,1)
    #   |          |          |
    # M0(0,0) -- M1(1,0) -- M2(2,0)
    # Endpoint placement:
    # RN@M0.local0, HN@M4.local0, DRAM@M4.local1
    # Shadow default attach point is configured at CacheConfig level.
    MeshNode0 = Param.MeshNode('M0 (0,0) RN-side mesh node')
    MeshNode1 = Param.MeshNode('M1 (1,0) transit mesh node')
    MeshNode2 = Param.MeshNode('M2 (2,0) transit mesh node')
    MeshNode3 = Param.MeshNode('M3 (0,1) transit mesh node')
    MeshNode4 = Param.MeshNode('M4 (1,1) HN/DRAM-side mesh node')
    MeshNode5 = Param.MeshNode('M5 (2,1) transit mesh node')
    MeshNode6 = Param.MeshNode('M6 (0,2) transit mesh node')
    MeshNode7 = Param.MeshNode('M7 (1,2) transit mesh node')
    MeshNode8 = Param.MeshNode('M8 (2,2) shadow-default-side mesh node')

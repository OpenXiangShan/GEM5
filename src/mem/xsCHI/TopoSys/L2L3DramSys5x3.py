from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.DDRWrapper import DDRWrapper
from m5.objects.CHI_L2 import CHI_L2
from m5.objects.CHI_L3 import CHI_L3
from m5.objects.MeshNode import MeshNode


class L2L3DramSys5x3(ClockedObject):
    type = 'L2L3DramSys5x3'
    cxx_header = 'mem/xsCHI/TopoSys/L2L3DramSys5x3.hh'
    cxx_class = 'gem5::xsCHI::L2L3DramSys5x3'

    cpu_side_port = ResponsePort("port for receiving requests from the CPU or other requestor")
    mem_side_port = RequestPort("This port is only used for redirecting uncached accesses")
    L2Wrapper = Param.CHI_L2('L2 Wrapper')
    rn_attach_point = Param.String(
        "mesh0.local0", "Main RN/L2 mesh attach point")
    ShadowRNBridges = VectorParam.CHIBridge([], "Shadow RN bridges")
    shadow_attach_points = VectorParam.String(
        [], "Per-shadow mesh attach point, e.g. mesh14.local0")

    HNs = VectorParam.CHI_L3([], "CHI L3/HN wrappers")
    hn_attach_points = VectorParam.String(
        ["mesh6.local0"], "Per-HN mesh attach point")
    dramsim3s = VectorParam.DDRWrapper([], "DDR wrappers")
    dram_attach_points = VectorParam.String(
        ["mesh6.local1"], "Per-DRAM mesh attach point")

    # 5x3 mesh topology (row-major):
    # M10(0,2) -- M11(1,2) -- M12(2,2) -- M13(3,2) -- M14(4,2)
    #   |           |           |           |           |
    # M5 (0,1) -- M6 (1,1) -- M7 (2,1) -- M8 (3,1) -- M9 (4,1)
    #   |           |           |           |           |
    # M0 (0,0) -- M1 (1,0) -- M2 (2,0) -- M3 (3,0) -- M4 (4,0)
    # Default endpoint placement:
    # RN@M0.local0, HN@M6.local0, DRAM@M6.local1
    MeshNode0 = Param.MeshNode('M0 (0,0) RN-side mesh node')
    MeshNode1 = Param.MeshNode('M1 (1,0) transit mesh node')
    MeshNode2 = Param.MeshNode('M2 (2,0) transit mesh node')
    MeshNode3 = Param.MeshNode('M3 (3,0) transit mesh node')
    MeshNode4 = Param.MeshNode('M4 (4,0) transit mesh node')
    MeshNode5 = Param.MeshNode('M5 (0,1) transit mesh node')
    MeshNode6 = Param.MeshNode('M6 (1,1) default HN/DRAM-side mesh node')
    MeshNode7 = Param.MeshNode('M7 (2,1) transit mesh node')
    MeshNode8 = Param.MeshNode('M8 (3,1) transit mesh node')
    MeshNode9 = Param.MeshNode('M9 (4,1) transit mesh node')
    MeshNode10 = Param.MeshNode('M10 (0,2) transit mesh node')
    MeshNode11 = Param.MeshNode('M11 (1,2) transit mesh node')
    MeshNode12 = Param.MeshNode('M12 (2,2) transit mesh node')
    MeshNode13 = Param.MeshNode('M13 (3,2) transit mesh node')
    MeshNode14 = Param.MeshNode('M14 (4,2) shadow-default-side mesh node')

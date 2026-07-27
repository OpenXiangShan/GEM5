from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.DDRWrapper import DDRWrapper
from m5.objects.CHI_L2 import CHI_L2
from m5.objects.CHI_L3 import CHI_L3
from m5.objects.MeshNode import MeshNode


class L2L3DramSys6x6(ClockedObject):
    type = 'L2L3DramSys6x6'
    cxx_header = 'mem/xsCHI/TopoSys/L2L3DramSys6x6.hh'
    cxx_class = 'gem5::xsCHI::L2L3DramSys6x6'

    cpu_side_port = ResponsePort("port for receiving requests from the CPU or other requestor")
    mem_side_port = RequestPort("This port is only used for redirecting uncached accesses")
    L2Wrapper = Param.CHI_L2('L2 Wrapper')
    rn_attach_point = Param.String(
        "mesh7.local0", "Main RN/L2 mesh attach point")
    ShadowRNBridges = VectorParam.CHIBridge([], "Shadow RN bridges")
    shadow_attach_points = VectorParam.String(
        [], "Per-shadow mesh attach point, e.g. mesh10.local0")

    HNs = VectorParam.CHI_L3([], "CHI L3/HN wrappers")
    hn_attach_points = VectorParam.String(
        [
            "mesh7.local1", "mesh8.local1", "mesh9.local1", "mesh10.local1",
            "mesh13.local1", "mesh14.local1", "mesh15.local1", "mesh16.local1",
            "mesh19.local1", "mesh20.local1", "mesh21.local1", "mesh22.local1",
            "mesh25.local1", "mesh26.local1", "mesh27.local1", "mesh28.local1",
        ],
        "Per-HN mesh attach point")
    dramsim3s = VectorParam.DDRWrapper([], "DDR wrappers")
    dram_attach_points = VectorParam.String(
        ["mesh1.local0", "mesh4.local0", "mesh31.local0", "mesh34.local0"],
        "Per-DRAM mesh attach point")

    # 6x6 mesh topology (row-major):
    # y=5: M30 M31 M32 M33 M34 M35
    # y=4: M24 M25 M26 M27 M28 M29
    # y=3: M18 M19 M20 M21 M22 M23
    # y=2: M12 M13 M14 M15 M16 M17
    # y=1: M6  M7  M8  M9  M10 M11
    # y=0: M0  M1  M2  M3  M4  M5
    # Default endpoint placement:
    # RN0@M7.local0, HN0@M7.local1, SN0@M1.local0.
    MeshNode0 = Param.MeshNode('M0 (0,0) SYS12 mesh node')
    MeshNode1 = Param.MeshNode('M1 (1,0) SN0 mesh node')
    MeshNode2 = Param.MeshNode('M2 (2,0) SYS13 mesh node')
    MeshNode3 = Param.MeshNode('M3 (3,0) SYS14 mesh node')
    MeshNode4 = Param.MeshNode('M4 (4,0) SN1 mesh node')
    MeshNode5 = Param.MeshNode('M5 (5,0) SYS15 mesh node')
    MeshNode6 = Param.MeshNode('M6 (0,1) SYS0 mesh node')
    MeshNode7 = Param.MeshNode('M7 (1,1) RN0/HN0 mesh node')
    MeshNode8 = Param.MeshNode('M8 (2,1) RN1/HN1 mesh node')
    MeshNode9 = Param.MeshNode('M9 (3,1) RN2/HN2 mesh node')
    MeshNode10 = Param.MeshNode('M10 (4,1) RN3/HN3 mesh node')
    MeshNode11 = Param.MeshNode('M11 (5,1) SYS1 mesh node')
    MeshNode12 = Param.MeshNode('M12 (0,2) SYS10 mesh node')
    MeshNode13 = Param.MeshNode('M13 (1,2) RN4/HN4 mesh node')
    MeshNode14 = Param.MeshNode('M14 (2,2) RN5/HN5 mesh node')
    MeshNode15 = Param.MeshNode('M15 (3,2) RN6/HN6 mesh node')
    MeshNode16 = Param.MeshNode('M16 (4,2) RN7/HN7 mesh node')
    MeshNode17 = Param.MeshNode('M17 (5,2) SYS11 mesh node')
    MeshNode18 = Param.MeshNode('M18 (0,3) SYS8 mesh node')
    MeshNode19 = Param.MeshNode('M19 (1,3) RN8/HN8 mesh node')
    MeshNode20 = Param.MeshNode('M20 (2,3) RN9/HN9 mesh node')
    MeshNode21 = Param.MeshNode('M21 (3,3) RN10/HN10 mesh node')
    MeshNode22 = Param.MeshNode('M22 (4,3) RN11/HN11 mesh node')
    MeshNode23 = Param.MeshNode('M23 (5,3) SYS9 mesh node')
    MeshNode24 = Param.MeshNode('M24 (0,4) SYS3 mesh node')
    MeshNode25 = Param.MeshNode('M25 (1,4) RN12/HN12 mesh node')
    MeshNode26 = Param.MeshNode('M26 (2,4) RN13/HN13 mesh node')
    MeshNode27 = Param.MeshNode('M27 (3,4) RN14/HN14 mesh node')
    MeshNode28 = Param.MeshNode('M28 (4,4) RN15/HN15 mesh node')
    MeshNode29 = Param.MeshNode('M29 (5,4) SYS2 mesh node')
    MeshNode30 = Param.MeshNode('M30 (0,5) SYS4 mesh node')
    MeshNode31 = Param.MeshNode('M31 (1,5) SN2 mesh node')
    MeshNode32 = Param.MeshNode('M32 (2,5) SYS5 mesh node')
    MeshNode33 = Param.MeshNode('M33 (3,5) SYS6 mesh node')
    MeshNode34 = Param.MeshNode('M34 (4,5) SN3 mesh node')
    MeshNode35 = Param.MeshNode('M35 (5,5) SYS7 mesh node')

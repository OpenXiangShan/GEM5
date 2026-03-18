from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.DDRWrapper import DDRWrapper
from m5.objects.CHI_L2 import CHI_L2
from m5.objects.CHI_L3 import CHI_L3
from m5.objects.MeshNode import MeshNode


class L2L3DramSys(ClockedObject):
    type = 'L2L3DramSys'
    cxx_header = 'mem/xsCHI/TopoSys/L2L3DramSys.hh'
    cxx_class = 'gem5::xsCHI::L2L3DramSys'

    cpu_side_port = ResponsePort("port for receiving requests from the CPU or other requestor")
    mem_side_port = RequestPort("This port is only used for redirecting uncached accesses")
    dramsim3 = Param.DDRWrapper('DDR Wrapper')
    L2Wrapper = Param.CHI_L2('L2 Wrapper')
    L3 = Param.CHI_L3('CHI L3 Wrapper')

    # 2x2 mesh topology (clockwise):
    # Mesh0(0,0) --east--> Mesh1(1,0)
    #   ^                         |
    #   |                         north
    #  south                      v
    # Mesh3(0,1) <--west-- Mesh2(1,1)
    # Endpoint placement:
    # RN@Mesh0.local0, HN@Mesh1.local0, DRAM@Mesh2.local0
    MeshNode0 = Param.MeshNode('RN-side mesh node')
    MeshNode1 = Param.MeshNode('HN-side mesh node')
    MeshNode2 = Param.MeshNode('DRAM-side mesh node')
    MeshNode3 = Param.MeshNode('Transit-only mesh node')

from m5.params import *
from m5.objects.ClockedObject import ClockedObject


class TwoMeshStressSys(ClockedObject):
    type = "TwoMeshStressSys"
    cxx_header = "mem/xsCHI/test/TwoMeshStressSys.hh"
    cxx_class = "gem5::xsCHI::TwoMeshStressSys"

    sender = Param.CHIStressEndpoint("Traffic source endpoint")
    receiver = Param.CHIStressEndpoint("Traffic sink endpoint")
    mesh0 = Param.MeshNode("Left mesh node")
    mesh1 = Param.MeshNode("Right mesh node")

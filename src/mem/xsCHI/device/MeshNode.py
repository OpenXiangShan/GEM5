from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIPort import *


class MeshNode(ClockedObject):
    type = "MeshNode"
    cxx_header = "mem/xsCHI/device/MeshNode.hh"
    cxx_class = "gem5::xsCHI::MeshNode"

    node_x = Param.Unsigned(0, "Mesh X coordinate")
    node_y = Param.Unsigned(0, "Mesh Y coordinate")
    # VOQ depth threshold used by MeshNode backpressure.
    voq_depth = Param.Unsigned(2, "MeshNode VOQ depth threshold")
    # True: per-(egress,channel,ingress) depth limit.
    # False: aggregate per-(egress,channel) depth limit.
    voq_depth_per_ingress = Param.Bool(
        True, "Use per-ingress VOQ depth threshold instead of aggregate")
    router_latency_cycles = Param.Unsigned(
        1, "Router pipeline latency from enqueue to send scheduling")

    # local0 is mandatory in current v1 topology.
    port_local0 = Param.CHIPort("Local device port 0")
    port_local1 = Param.CHIPort(NULL, "Optional local device port 1")
    port_east = Param.CHIPort(NULL, "Mesh east port")
    port_west = Param.CHIPort(NULL, "Mesh west port")
    port_north = Param.CHIPort(NULL, "Mesh north port")
    port_south = Param.CHIPort(NULL, "Mesh south port")

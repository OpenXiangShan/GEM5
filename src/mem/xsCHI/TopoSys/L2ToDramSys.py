from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.DDRWrapper import *
from m5.objects.CHI_L2 import *
from m5.objects.FakeL3 import *
from m5.objects.MeshNode import *
class L2ToDramSys(ClockedObject):
    type = 'L2ToDramSys'
    cxx_header = "mem/xsCHI/TopoSys/L2todram.hh"
    cxx_class = 'gem5::xsCHI::L2ToDramSys'

    # # A default memory size of 128 MiB (starting at 0) is used to
    # # simplify the regressions
    # range = Param.AddrRange('128MiB',
    #                         "Address range (potentially interleaved)")
    # null = Param.Bool(False, "Do not store data, always return zero")

    # # All memories are passed to the global physical memory, and
    # # certain memories may be excluded from the global address map,
    # # e.g. by the testers that use shadow memories as a reference
    # in_addr_map = Param.Bool(True, "Memory part of the global address map")

    # # When KVM acceleration is used, memory is mapped into the guest process
    # # address space and accessed directly. Some memories may need to be
    # # excluded from this mapping if they overlap with other memory ranges or
    # # are not accessible by the CPU.
    # kvm_map = Param.Bool(True, "Should KVM map this memory for the guest")

    # # Should the bootloader include this memory when passing
    # # configuration information about the physical memory layout to
    # # the kernel, e.g. using ATAG or ACPI
    # conf_table_reported = Param.Bool(True, "Report to configuration table")

    # # Image file to load into this memory as its initial contents. This is
    # # particularly useful for ROMs.
    # image_file = Param.String('',
    #         "Image to load into memory as its initial contents")

    # A single port for now
    cpu_side_port = ResponsePort("port for receiving requests from"
                        "the CPU or other requestor")
    mem_side_port = RequestPort("This port sends requests and "
                            "receives responses")
    configFile = Param.String("ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini",
                              "The configuration file to use with DRAMSim3")
    filePath = Param.String("ext/dramsim3/DRAMsim3/",
                            "Directory to prepend to file names")
    write_buffers = Param.Unsigned(8, "Number of write buffers")

    dramsim3 = Param.DDRWrapper("DDR Wrapper")

    L2Wrapper = Param.CHI_L2("L2 Wrapper")
    # 来自配置层的影子桥列表；TopoSys 在构造时负责将其接到指定 Mesh local 口。
    ShadowRNBridges = VectorParam.CHIBridge([], "Shadow RN bridges")
    # 每个影子对应一个挂点，格式固定为 meshX.localY（例如 mesh3.local0）。
    shadow_attach_points = VectorParam.String([], "Per-shadow mesh attach point, e.g. mesh3.local0")

    L3 = Param.FakeL3("L3 mux")

    # 2x2 mesh topology (clockwise):
    # Mesh0(0,0) --east--> Mesh1(1,0)
    #   ^                         |
    #   |                         north
    #  south                      v
    # Mesh3(0,1) <--west-- Mesh2(1,1)
    # Endpoint placement:
    # RN@Mesh0.local0, HN@Mesh1.local0, DRAM@Mesh2.local0
    MeshNode0 = Param.MeshNode("RN-side mesh node")
    MeshNode1 = Param.MeshNode("HN-side mesh node")
    MeshNode2 = Param.MeshNode("DRAM-side mesh node")
    MeshNode3 = Param.MeshNode("Transit-only mesh node")

    L2BridgeBufferSize = Param.Unsigned(4, "Number of L2Bridgebuffers")

    FakeL3CpuSideBufferSize = Param.Unsigned(4, "FakeL3CpuSideBufferSize")

    FakeL3MemSideBufferSize = Param.Unsigned(4, "FakeL3MemSideBufferSize")

    DDRWrapperBufferSize = Param.Unsigned(4, "DDRWrapperBufferSize")

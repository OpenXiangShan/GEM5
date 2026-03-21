from m5.params import *
from m5.objects.ClockedObject import ClockedObject


class CHIStressEndpoint(ClockedObject):
    type = "CHIStressEndpoint"
    cxx_header = "mem/xsCHI/test/CHIStressEndpoint.hh"
    cxx_class = "gem5::xsCHI::CHIStressEndpoint"

    networkPort = Param.CHIPort("Attached CHI network port")

    enable_sender = Param.Bool(False, "Enable synthetic traffic generation")
    total_flits = Param.Unsigned(0, "Total flits to inject when sender is enabled")
    inject_per_cycle = Param.Unsigned(1, "Maximum sends attempted per cycle")

    src_id = Param.Unsigned(0, "Source node ID in generated flits")
    tgt_id = Param.Unsigned(0, "Target node ID in generated flits")

    base_addr = Param.Unsigned(0, "Base physical address for generated flits")
    addr_stride = Param.Unsigned(64, "Address stride between generated flits")
    payload_size = Param.Unsigned(64, "Flit payload size in bytes")

    receiver_block_period = Param.Unsigned(
        0, "Receiver periodic blocking window; 0 disables blocking")
    receiver_block_cycles = Param.Unsigned(
        0, "Number of cycles blocked in each receiver_block_period")

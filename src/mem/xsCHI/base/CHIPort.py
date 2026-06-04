from m5.params import *
from m5.objects.ClockedObject import ClockedObject

class CHIPort(ClockedObject):
    type = 'CHIPort'
    cxx_header = "mem/xsCHI/base/CHIPort.hh"
    cxx_class = 'gem5::xsCHI::CHIPort'

    # Legacy alias. 0 means "auto": legacy credit model uses 4 entries,
    # cmn700/cmn700_rtl credit models use rxbuf_num's CMN default of 3 entries.
    recv_buffer_size = Param.Unsigned(0, "Legacy receive buffer size alias")
    rxbuf_num = Param.Unsigned(
        0, "CMN-style receive flit buffer entries; 0 selects model default")
    skid_depth = Param.Unsigned(
        0, "CMN RTL-style skid/staging entries; 0 selects model default")
    initial_credit_count = Param.Unsigned(
        0, "Initial advertised credits per channel; 0 selects rxbuf_num")

    credit_model = Param.String(
        "legacy", "Credit model: legacy, cmn700, or cmn700_rtl")
    credit_return_direction = Param.String(
        "internal", "Credit return direction: up, down, or internal")
    credit_release_policy = Param.String(
        "on_accept", "Credit release policy: on_accept or on_downstream_release")

    up_crd_lat_int = Param.Cycles(1, "CMN upload internal credit latency")
    up_crd_lat_ext = Param.Cycles(2, "CMN upload external credit latency")
    dn_crd_lat_int = Param.Cycles(2, "CMN download internal credit latency")
    dn_crd_lat_ext = Param.Cycles(1, "CMN download external credit latency")
    internal_crd_lat = Param.Cycles(1, "Internal mesh-link credit latency")

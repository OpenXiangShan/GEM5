from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIPort import CHIPort
from m5.objects.XBar import CoherentXBar
from m5.objects.CacheWrapper import L3CacheWrapper

class CHI_L3(ClockedObject):
    type = 'CHI_L3'
    cxx_header = "mem/xsCHI/device/CHI_L3.hh"
    cxx_class = 'gem5::xsCHI::CHI_L3'

    networkPort = Param.CHIPort("CHI network port")
    coherent_xbar = Param.CoherentXBar("coherent xbar instance")
    cache_wrapper = Param.L3CacheWrapper("CacheWrapper instance to wrap")
    extra_req_cycles = Param.Unsigned(
        0, "Extra cycles before emitting CHI REQ flits to DDR path")
    extra_rsp_cycles = Param.Unsigned(
        0, "Extra cycles before emitting CHI RSP/DAT flits to RN path")

    inner_req_port = RequestPort("Pseudo request port bound to coherent_xbar cpu_side_ports")
    inner_resp_port = ResponsePort("Pseudo response port bound to coherent_xbar mem_side_ports")

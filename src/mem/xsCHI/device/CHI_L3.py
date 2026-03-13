from m5.params import *
from m5.objects.ClockedObject import ClockedObject
from m5.objects.CHIPort import CHIPort
from m5.objects.XBar import CoherentXBar
from m5.objects.CacheWrapper import L3CacheWrapper

class CHI_L3(ClockedObject):
    type = 'CHI_L3'
    cxx_header = "mem/xsCHI/device/CHI_L3.hh"
    cxx_class = 'gem5::xsCHI::CHI_L3'

    cpuSidePort = Param.CHIPort("L2/RN side CHI port")
    memSidePort = Param.CHIPort("DDR side CHI port")
    coherent_xbar = Param.CoherentXBar("coherent xbar instance")
    cache_wrapper = Param.L3CacheWrapper("CacheWrapper instance to wrap")

    inner_req_port = RequestPort("Pseudo request port bound to coherent_xbar cpu_side_ports")
    inner_resp_port = ResponsePort("Pseudo response port bound to coherent_xbar mem_side_ports")

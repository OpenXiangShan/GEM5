from m5.params import *
from m5.objects.AbstractMemory import *
from m5.objects.CHIPort import *
class DDRWrapper(AbstractMemory):
    type = 'DDRWrapper'
    cxx_header = "mem/xsCHI/device/DDRWrapper.hh"
    cxx_class = 'gem5::xsCHI::DDRWrapper'

    # A single port for now
    port = ResponsePort("port for receiving requests from"
                        "the CPU or other requestor")

    configFile = Param.String("ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini",
                              "The configuration file to use with DRAMSim3")
    filePath = Param.String("ext/dramsim3/DRAMsim3/",
                            "Directory to prepend to file names")
    networkPort = Param.CHIPort("networkPort pointer")
    use_dmt = Param.Bool(True, "Whether to use DMT return (ReturnNid/ReturnTxnid) if use FakeL3,it has to be True")
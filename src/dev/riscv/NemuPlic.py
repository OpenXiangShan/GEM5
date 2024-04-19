from m5.params import *
from m5.proxy import *

from m5.objects.Device import BasicPioDevice, PioDevice, IsaFake, BadAddr

class NemuPlic(BasicPioDevice):
    type = 'NemuPlic'
    cxx_header = "dev/riscv/nemu_plic.hh"
    cxx_class = 'gem5::NemuPlic'
    pio_addr = 0x3c000000
    pio_size = Param.Addr(0x4000000, "NemuPlic space size")
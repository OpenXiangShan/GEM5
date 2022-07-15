from m5.params import *
from m5.proxy import *

from m5.objects.Device import BasicPioDevice, PioDevice, IsaFake, BadAddr

class NemuMMC(BasicPioDevice):
    type = 'NemuMMC'
    cxx_header = "dev/riscv/nemu_mmc.hh"
    cxx_class = 'gem5::NemuMMC'
    pio_addr = 0x40002000
    pio_size = Param.Addr(0x80, "Nemu MMC space size")
    img_path = Param.String("/the/mid/of/nowhere.xhit","Nemu MMC img path")
    cpt_bin_path = Param.String("/the/mid/of/nowhere.xhit",
                                "Nemu MMC cpt bin path")

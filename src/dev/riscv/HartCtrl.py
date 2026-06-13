from m5.params import *
from m5.proxy import *

from m5.objects.Device import BasicPioDevice


class HartCtrl(BasicPioDevice):
    type = 'HartCtrl'
    cxx_header = "dev/riscv/hart_ctrl.hh"
    cxx_class = 'gem5::HartCtrl'
    pio_addr = 0x39001000
    pio_size = Param.Addr(0x1000, "Hart control register space size")
    num_threads = Param.Int("Number of threads in the system.")

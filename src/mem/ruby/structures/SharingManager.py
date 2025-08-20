from m5.objects.System import System
from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject

class SharingManager(SimObject):
    type = "SharingManager"
    cxx_class = "gem5::ruby::SharingManager"
    cxx_header = "mem/ruby/structures/SharingManager.hh"


    downstream_hnfs = VectorParam.RubyController(
        [], "HNFs downstream of this SNF"
    )
    downstream_snfs = VectorParam.RubyController(
        [], "SNFs downstream of this HNF"
    )

    xid = Param.Int(-1, "X coordinate of this SNF")
    yid = Param.Int(-1, "Y coordinate of this SNF")

    row_size = Param.Int(4, "Mesh number of rows")
    col_size = Param.Int(4, "Mesh number of columns")

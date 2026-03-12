from m5.params import *
from m5.proxy import *
from m5.SimObject import *


class AddressPredType(ScopedEnum):
    vals = ["IdealConstantAP", "EStrideAP"]


class AddressPredictor(SimObject):
    type = "AddressPredictor"
    cxx_class = "gem5::addresspred::APUnit"
    cxx_header = "cpu/addresspred/addresspred_unit.hh"
    abstract = True


class IdealConstantAP(AddressPredictor):
    type = "IdealConstantAP"
    cxx_class = "gem5::addresspred::IdealConstantAP"
    cxx_header = "cpu/addresspred/ideal_constant_ap.hh"
    abstract = False

    satCounterBits = Param.Unsigned(
        9, "bits of saturating counter, initial value is 0"
    )
    resetConfidence = Param.Bool(True, "reset confidence to 0 when mispredict")


class EStrideAP(AddressPredictor):
    type = "EStrideAP"
    cxx_class = "gem5::addresspred::EStrideAP"
    cxx_header = "cpu/addresspred/enhanced_stride_ap.hh"
    abstract = False

    ways = Param.Int(3, "ways of the EStrideAP")
    strideWidth = Param.Int(20, "bits used for stride, must <= 64")
    tagWidth = Param.Int(16, "tag-width")
    logESTBEntrys = Param.Int(7, "log2 of EStrideAP table entry counts")
    logMaxConfidence = Param.Int(10, "log2 of max confidence number")
    thresholdPercent = Param.Float(0.25, "threshold percent of confidence")
    dcacheCounterBits = Param.Unsigned(
        3, "bits of fromDcache confidence saturating counter"
    )
    dcacheThresholdPercent = Param.Float(
        0.5, "minimum fromDcache confidence to allow prediction"
    )

    idealWindow = Param.Bool(
        True, "key in ideal window is full pc, not hashed"
    )
    inflightWindowTagLength = Param.Int(64, "inflight window tag length")

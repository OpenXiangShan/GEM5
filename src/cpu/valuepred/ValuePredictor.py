from m5.params import *
from m5.proxy import *
from m5.SimObject import *

class ValuePredType(ScopedEnum):
    # vals will contains value predictor type
    vals = ["NullPredictor", "EStride", "IdealConstantLVP", "MultiValuePredictor"]

class ValuePredictor(SimObject):
    type = "ValuePredictor"
    cxx_class = "gem5::valuepred::VPUnit"
    cxx_header = "cpu/valuepred/valuepred_unit.hh"
    abstract = True

class GatedVPUnit(ValuePredictor):
    type = "GatedVPUnit"
    cxx_class = "gem5::valuepred::GatedVPUnit"
    cxx_header = "cpu/valuepred/gated_vp_unit.hh"
    abstract = True

    shadowThresholdPercent = Param.Float(
        0.99, "minimum shadow prediction accuracy required for real value prediction")

class EStride(GatedVPUnit):
    type = "EStride"
    cxx_class = "gem5::valuepred::EStride"
    cxx_header = "cpu/valuepred/enhanced_stride.hh"
    abstract = False

    # default params reference 1st cvp paper
    ways = Param.Int(3, "ways of the EStride")
    strideWidth = Param.Int(20, "Indicates the number of bits used for stride"
                            "must <= 32")
    tagWidth = Param.Int(16, "tag-width")
    logESTBEntrys = Param.Int(7, "log 2 of ES table entry counts")
    logMaxConfidence = Param.Int(5, "log 2 of max confidence number")
    thresholdPercent = Param.Float(0.25, "threshold percent of confidence")
    disableZeroStridePredict = Param.Bool(
        False, "if true, EStride will not generate prediction for zero stride entries")
    # inflight window configuration
    idealWindow = Param.Bool(True, "The key in the ideal window is a 64-bit pc, not hashed")
    inflightWindowTagLength = Param.Int(64, "inflight window tag length")

    # about update strategy
    # still not use
    enableTimeMsgInUpdate = Param.Bool(True, "enable use instruction"
                                       "inflight time in update")

class IdealConstantLVP(GatedVPUnit):
    type = "IdealConstantLVP"
    cxx_class = "gem5::valuepred::IdealConstantLVP"
    cxx_header = "cpu/valuepred/ideal_constant_lvp.hh"
    abstract = False

    satCounterBits = Param.Unsigned(9, "bits of saturating counter, initial value is 0")
    resetConfidence = Param.Bool(True, "reset confidence to 0 when mispredict")

class MultiValuePredictor(ValuePredictor):
    type = "MultiValuePredictor"
    cxx_class = "gem5::valuepred::MultiValuePredictor"
    cxx_header = "cpu/valuepred/multi_value_predictor.hh"
    abstract = False

    predictors = VectorParam.ValuePredictor([], "Sub value predictors in priority order")
    dynamicArb = Param.Bool(True, "Enable dynamic arbitration among valid predictors")
    arbCounterBits = Param.Unsigned(8, "bits of arbitration confidence counter")

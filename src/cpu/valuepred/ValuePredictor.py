from m5.params import *
from m5.proxy import *
from m5.SimObject import *

class ValuePredType(ScopedEnum):
    # vals will contains value predictor type
    vals = ["EStride", "MemoryRenaming", "IdealConstantLVP"]

class ValuePredictor(SimObject):
    type = "ValuePredictor"
    cxx_class = "gem5::valuepred::VPUnit"
    cxx_header = "cpu/valuepred/valuepred_unit.hh"
    abstract = True
    numThreads = Param.Unsigned(Parent.numThreads, "Number of threads")

class EStride(ValuePredictor):
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

    # inflight window configuration
    idealWindow = Param.Bool(True, "The key in the ideal window is a 64-bit pc, "
                             "not hashed")
    inflightWindowTagLength = Param.Int(64, "inflight window tag length")

    # about update strategy
    # still not use
    enableTimeMsgInUpdate = Param.Bool(True, "enable use instruction"
                                       "inflight time in update")

class MemoryRenaming(ValuePredictor):
    type = "MemoryRenaming"
    cxx_class = "gem5::valuepred::MemoryRenaming"
    cxx_header = "cpu/valuepred/memory_renaming.hh"
    abstract = False

    ways = Param.Int(3, "ways of the store-load cache")
    tagWidth = Param.Int(16, "tag-width")
    logESTBEntrys = Param.Int(7, "log 2 of store-load cache entry counts")
    logMaxConfidence = Param.Int(5, "log 2 of max confidence number")
    thresholdPercent = Param.Float(0.25, "threshold percent of confidence")
    logStoreLoadValueFileEntries = Param.Int(32,
                                             "log 2 of value-file index space "
                                             "for store-load cache entries")

    idealWindow = Param.Bool(True, "The key in the ideal window is a 64-bit pc, "
                             "not hashed")
    inflightWindowTagLength = Param.Int(64, "inflight window tag length")

class IdealConstantLVP(ValuePredictor):
    type = "IdealConstantLVP"
    cxx_class = "gem5::valuepred::IdealConstantLVP"
    cxx_header = "cpu/valuepred/ideal_constant_lvp.hh"
    abstract = False

    satCounterBits = Param.Unsigned(9, "bits of saturating counter, initial value is 0")
    resetConfidence = Param.Bool(True, "reset confidence to 0 when mispredict")

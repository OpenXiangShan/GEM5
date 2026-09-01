from m5.params import *
from m5.params import isNullPointer
from m5.proxy import *
from m5.SimObject import *

class ValuePredType(ScopedEnum):
    # vals will contains value predictor type
    vals = ["EStride", "MemoryRenaming", "IdealConstantLVP",
            "ExampleValuePredictor", "CompositeValuePredictor",
            "ConstantLVP"]

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

class ExampleValuePredictor(ValuePredictor):
    type = "ExampleValuePredictor"
    cxx_class = "gem5::valuepred::ExampleValuePredictor"
    cxx_header = "cpu/valuepred/example_value_predictor.hh"
    abstract = False

class CVPArb(SimObject):
    type = "CVPArb"
    cxx_class = "gem5::valuepred::CompositeValuePredictorArb"
    cxx_header = "cpu/valuepred/composite_value_predictor_arb.hh"
    abstract = True

class CVPFixedPriorityArb(CVPArb):
    type = "CVPFixedPriorityArb"
    cxx_class = "gem5::valuepred::CompositeValuePredictorFixedPriorityArb"
    cxx_header = "cpu/valuepred/composite_value_predictor_arb.hh"

class CVPRandomArb(CVPArb):
    type = "CVPRandomArb"
    cxx_class = "gem5::valuepred::CompositeValuePredictorRandomArb"
    cxx_header = "cpu/valuepred/composite_value_predictor_arb.hh"

class CVPRRArb(CVPArb):
    type = "CVPRRArb"
    cxx_class = "gem5::valuepred::CompositeValuePredictorRRArb"
    cxx_header = "cpu/valuepred/composite_value_predictor_arb.hh"

class CVPConfidenceArb(CVPArb):
    type = "CVPConfidenceArb"
    cxx_class = "gem5::valuepred::CompositeValuePredictorConfidenceArb"
    cxx_header = "cpu/valuepred/composite_value_predictor_arb.hh"

    counterBits = Param.Unsigned(2,
        "Bits of per-child learned confidence counter used by the arbiter")

class CompositeValuePredictor(ValuePredictor):
    type = "CompositeValuePredictor"
    cxx_class = "gem5::valuepred::CompositeValuePredictor"
    cxx_header = "cpu/valuepred/composite_value_predictor.hh"
    abstract = False

    predictors = VectorParam.ValuePredictor([], "Ordered child value predictors")
    arb = Param.CVPArb(
        CVPFixedPriorityArb(),
        "Arbitration policy for selecting among speculative child candidates")

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._rename_predictor_children()

    def __setattr__(self, attr, value):
        super().__setattr__(attr, value)
        if attr == "predictors":
            self._rename_predictor_children()

    def _rename_predictor_children(self):
        # rename the child predictors so that they have a meaningful name in stats.txt
        predictors = self._values.get("predictors")
        if not predictors:
            return

        type_counts = {}
        for predictor in predictors:
            if isNullPointer(predictor):
                continue
            type_name = predictor.__class__.__name__
            type_counts[type_name] = type_counts.get(type_name, 0) + 1

        type_seen = {}
        for predictor in predictors:
            if isNullPointer(predictor):
                continue

            type_name = predictor.__class__.__name__
            if type_counts[type_name] == 1:
                child_name = type_name
            else:
                child_idx = type_seen.get(type_name, 0)
                child_name = f"{type_name}_{child_idx}"
                type_seen[type_name] = child_idx + 1

            predictor.set_parent(self, child_name)

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

class ConstantLVP(ValuePredictor):
    type = "ConstantLVP"
    cxx_class = "gem5::valuepred::ConstantLVP"
    cxx_header = "cpu/valuepred/constant_lvp.hh"
    abstract = False

    numWays = Param.Unsigned(3, "Number of skewed-associative table ways")
    numSets = Param.Unsigned(128, "Number of entries in each way")
    tagBits = Param.Unsigned(16, "Hashed PC tag width")
    confidenceBits = Param.Unsigned(
        9, "Constant confidence counter width; allocations start at one")
    usefulBits = Param.Unsigned(2, "Replacement useful counter width")
    resetConfidence = Param.Bool(
        False, "Reset confidence and useful counters on a value mismatch")
    thresholdPercent = Param.Percent(
        100, "Minimum confidence percentage required to predict")

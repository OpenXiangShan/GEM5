from m5.params import *
from m5.params import isNullPointer
from m5.proxy import *
from m5.SimObject import *

class ValuePredType(ScopedEnum):
    # vals will contains value predictor type
    vals = ["EStride", "VTAGE", "MemoryRenaming", "IdealConstantLVP",
            "IdealEgDiff", "ExampleValuePredictor", "CompositeValuePredictor"]

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

class VTAGE(ValuePredictor):
    type = "VTAGE"
    cxx_class = "gem5::valuepred::VTAGE"
    cxx_header = "cpu/valuepred/vtage.hh"
    abstract = False

    numHistories = Param.Unsigned(8,
        "Number of VTAGE history banks excluding the base bank")
    numBanks = Param.Unsigned(9,
        "Total VTAGE banks including the base bank")
    histLengths = VectorParam.Unsigned(
        [0, 0, 3, 7, 15, 31, 63, 90, 127],
        "History length per VTAGE bank, including the base bank")
    requireHistoryExt = Param.Bool(
        False,
        "Fatal if the predict request does not provide FetchTarget history")
    logBankSize = Param.Unsigned(9, "Log2 entries per VTAGE bank")
    tagBits = Param.Unsigned(11, "VTAGE tag width")
    confBits = Param.Unsigned(11, "VTAGE confidence-counter width")
    usefulBits = Param.Unsigned(2, "VTAGE usefulness-counter width")
    logValueArrayEntries = Param.Unsigned(9,
        "Log2 entries per VTAGE value-array way")

    predictConfThreshold = Param.Unsigned(1024,
        "Minimum confidence needed to issue a VTAGE prediction")
    hashOnlyUpgradeThreshold = Param.Unsigned(1023,
        "Minimum confidence before trying to upgrade hash-only entries")
    mispredBackoffDistance = Param.Unsigned(128,
        "Number of dynamic instructions to suppress VTAGE after a selected misprediction")
    agingTickMax = Param.Unsigned(1024,
        "Threshold for triggering global usefulness aging")
    agingPenaltyOnAlloc = Param.Unsigned(1,
        "Aging-tick increment after a successful VTAGE allocation")
    agingPenaltyOnNoAlloc = Param.Unsigned(5,
        "Aging-tick increment after a non-allocating VTAGE update")

    l1HitMaxCycles = Param.Unsigned(4,
        "Observed latency upper bound for L1-like loads")
    l2HitMaxCycles = Param.Unsigned(20,
        "Observed latency upper bound for L2-like loads")
    llcHitMaxCycles = Param.Unsigned(50,
        "Observed latency upper bound for LLC-like loads")
    fastInstCycles = Param.Unsigned(1,
        "Observed latency upper bound for fast loads")
    mfastInstCycles = Param.Unsigned(14,
        "Observed latency upper bound for medium-fast loads")

    enableStochasticTraining = Param.Bool(
        True,
        "Use probabilistic VTAGE training helpers instead of deterministic always-on updates")
    rngSeed = Param.Unsigned(1, "VTAGE RNG seed")
    allocProbLoadL1Hit = Param.Float(
        0.25, "Allocation probability for fast/L1-like loads")
    allocProbLoadMiss = Param.Float(
        1.0, "Allocation probability for slower loads")
    confIncProbLowValue = Param.Float(
        1.0, "Confidence increment probability when the actual value is small or sign-extended")
    confIncProbFastLoad = Param.Float(
        1.0, "Confidence increment probability for fast loads")
    uIncProbFastLoad = Param.Float(
        0.5, "Usefulness increment probability for medium-fast loads")
    valueArrayUpgradeProb = Param.Float(
        0.25, "Probability of promoting a hash-only entry into the value array")
    shortHistoryAllocBias = Param.Float(
        0.125, "Bias toward allocating from shorter-history banks")
    deepAllocExtraHopProb = Param.Float(
        0.125, "Probability of skipping one extra bank toward deeper history on allocation")

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

class IdealEgDiff(ValuePredictor):
    type = "IdealEgDiff"
    cxx_class = "gem5::valuepred::IdealEgDiff"
    cxx_header = "cpu/valuepred/ideal_egdiff.hh"
    abstract = False

    order = Param.Unsigned(32, "Maximum dynamic load distance to poll")
    confidenceBits = Param.Unsigned(
        3, "Bits in each deterministic saturating confidence counter")

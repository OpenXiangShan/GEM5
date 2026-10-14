#include "cpu/o3/mlp_predictor.hh"

#include "base/logging.hh"
#include "debug/Fetch.hh"

namespace gem5 {
namespace o3 {

MLPredictorPerThread::MLPredictorPerThread(
    unsigned missPatSize, unsigned mlpDistSize, unsigned shiftRegSize)
    : missPatternTableSize(missPatSize),
      mlpDistanceTableSize(mlpDistSize)
{
    // Validate public CPU parameters before allocation, including in builds
    // without assertions and configurations that bypass XiangShan's CLI.
    fatal_if(missPatSize < 2 || (missPatSize & (missPatSize - 1)) != 0,
             "MLP miss-pattern table size (%u) must be a power of two >= 2",
             missPatSize);
    fatal_if(mlpDistSize < 2 || (mlpDistSize & (mlpDistSize - 1)) != 0,
             "MLP distance table size (%u) must be a power of two >= 2",
             mlpDistSize);
    fatal_if(shiftRegSize == 0, "MLP shift register size must be positive");
    missPatternTable.resize(missPatSize);
    mlpDistanceTable.resize(mlpDistSize);
    shiftRegister.resize(shiftRegSize);
}

unsigned
MLPredictorPerThread::hashIndex(Addr pc, unsigned tableSize) const
{
    // RISC-V C extension: instructions are 2-byte aligned
    Addr shifted = pc >> 1;

    // The constructor enforces a power-of-two table size >= 2 even without
    // assertions. Keep this invariant check before computing nonzero indexBits.
    assert(tableSize >= 2 && (tableSize & (tableSize - 1)) == 0);
    unsigned indexBits = __builtin_ctz(tableSize);

    // XOR-fold the full address into indexBits-wide chunks
    constexpr unsigned addrBits = sizeof(Addr) * 8;
    unsigned mask = (1u << indexBits) - 1;
    unsigned hash = 0;
    for (unsigned offset = 0; offset < addrBits; offset += indexBits) {
        hash ^= (unsigned)((shifted >> offset) & mask);
    }

    // For power-of-2 tableSize: hash is at most mask == tableSize - 1
    assert(hash < tableSize);
    return hash;
}

// ========== Frontend read-only queries ==========

bool
MLPredictorPerThread::predictLongLatency(Addr loadPC) const
{
    unsigned idx = hashIndex(loadPC, missPatternTableSize);
    const auto &entry = missPatternTable[idx];
    if (!entry.valid || !entry.hasObservedInterval) return false;
    return (entry.currentHits == entry.lastIntervalHits);
}

uint32_t
MLPredictorPerThread::predictMLPDistance(Addr loadPC, bool *outValid) const
{
    unsigned idx = hashIndex(loadPC, mlpDistanceTableSize);
    const auto &entry = mlpDistanceTable[idx];
    if (outValid) {
        *outValid = entry.valid;
    }
    if (!entry.valid) return 0;
    return entry.predictedDistance;
}

// ========== Backend updates ==========

int
MLPredictorPerThread::updateOnLoadComplete(
    Addr loadPC, bool actualLongLatency,
    bool predictedLongLatency, uint32_t predictedDistance)
{
    // Determine prediction result
    int result = 0;
    if (actualLongLatency && predictedLongLatency) {
        result = 0; // LONG_LATENCY_PRED_TRUE_POSITIVE;
    } else if (!actualLongLatency && !predictedLongLatency) {
        result = 1; // LONG_LATENCY_PRED_TRUE_NEGATIVE;
    } else if (!actualLongLatency && predictedLongLatency) {
        result = 2; //LONG_LATENCY_PRED_FALSE_POSITIVE;
    } else {
        result = 3; //LONG_LATENCY_PRED_FALSE_NEGATIVE;
    }

    // Update miss-pattern table
    unsigned patIdx = hashIndex(loadPC, missPatternTableSize);
    auto &patEntry = missPatternTable[patIdx];

    if (actualLongLatency) {
        if (!patEntry.valid) {
            patEntry.valid = true;
            patEntry.lastIntervalHits = 0;
            patEntry.currentHits = 0;
            patEntry.hasObservedInterval = false;  // First miss, no interval yet
        } else {
            patEntry.lastIntervalHits = patEntry.currentHits;
            patEntry.hasObservedInterval = true;  // Second miss establishes interval
            patEntry.currentHits = 0;
        }
    } else {
        patEntry.currentHits++;
    }

    return result;
}

int64_t
MLPredictorPerThread::pushLLSR(bool isLongLatencyLoad, Addr loadPC,
                               uint32_t predictedDistance, bool predictionValid,
                               bool isFusionPlaceholder)
{
    auto evicted = shiftRegister.push(isLongLatencyLoad, loadPC,
                                     predictedDistance, predictionValid,
                                     isFusionPlaceholder);
    if (evicted.isLongLatencyLoad && !evicted.isFusionPlaceholder) {
        return updateMLPDistanceTable(evicted);
    }
    return INT64_MAX;
}

int64_t
MLPredictorPerThread::updateMLPDistanceTable(
    const LongLatencyShiftRegister::Entry &evicted)
{
    unsigned actualDistance = shiftRegister.findLastLongLatencyDistance();
    unsigned idx = hashIndex(evicted.pc, mlpDistanceTableSize);

    // Compare the fetch-time snapshot only if it was valid at prediction time.
    // Cast before subtraction to avoid unsigned underflow and sentinel overlap.
    int64_t result = INT64_MAX;
    if (evicted.predictionValid) {
        result = int64_t(evicted.predictedDistance) - int64_t(actualDistance);
    }

    // Train every evicted LL load, even when its prediction snapshot was invalid.
    mlpDistanceTable[idx].valid = true;
    mlpDistanceTable[idx].predictedDistance = actualDistance;

    return result;
}

// ========== MLPredictor ==========

MLPredictor::MLPredictor(unsigned numThreads, unsigned missPatternTableSize,
                         unsigned mlpDistanceTableSize,
                         unsigned shiftRegisterSize)
{
    for (unsigned i = 0; i < numThreads; i++) {
        predictors.emplace_back(missPatternTableSize,
                                mlpDistanceTableSize,
                                shiftRegisterSize);
    }
}

MLPredictorPerThread&
MLPredictor::getPredictor(ThreadID tid)
{
    return predictors[tid];
}

} // namespace o3
} // namespace gem5

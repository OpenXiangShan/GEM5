#include "cpu/o3/mlp_predictor.hh"

#include "base/logging.hh"
#include "debug/Fetch.hh"

namespace gem5 {
namespace o3 {

MLPredictorPerThread::MLPredictorPerThread(
    unsigned missPatSize, unsigned mlpDistSize, unsigned shiftRegSize)
    : missPatternTableSize(missPatSize),
      mlpDistanceTableSize(mlpDistSize),
      missPatternTable(missPatSize),
      mlpDistanceTable(mlpDistSize)
{
    shiftRegister.resize(shiftRegSize);
}

unsigned
MLPredictorPerThread::hashIndex(Addr pc, unsigned tableSize) const
{
    // RISC-V C extension: instructions are 2-byte aligned
    Addr shifted = pc >> 1;

    // Table size must be a power of 2 (and >= 2) so that XOR-fold produces
    // indices in [0, tableSize) without modulo. A size of 1 is also a power
    // of two but yields indexBits = __builtin_ctz(1) = 0, making the loop
    // below increment offset by 0 and spin forever, so reject it here.
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
    if (!entry.valid) return false;
    return (entry.currentHits == entry.lastIntervalHits);
}

uint32_t
MLPredictorPerThread::predictMLPDistance(Addr loadPC) const
{
    unsigned idx = hashIndex(loadPC, mlpDistanceTableSize);
    const auto &entry = mlpDistanceTable[idx];
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
        } else {
            patEntry.lastIntervalHits = patEntry.currentHits;
            patEntry.currentHits = 0;
        }
    } else {
        patEntry.currentHits++;
    }

    return result;
}

int
MLPredictorPerThread::pushLLSR(bool isLongLatencyLoad, Addr pc)
{
    auto evicted = shiftRegister.push(isLongLatencyLoad, pc);
    if (evicted.isLongLatencyLoad) {
        return updateMLPDistanceTable(evicted.pc);
    }
    return INT32_MAX;
}

int
MLPredictorPerThread::updateMLPDistanceTable(Addr headLoadPC)
{
    unsigned actualDistance = shiftRegister.findLastLongLatencyDistance();
    unsigned idx = hashIndex(headLoadPC, mlpDistanceTableSize);

    // Compare old prediction with actual distance
    int result = INT32_MAX;
    if (mlpDistanceTable[idx].valid) {
        result = mlpDistanceTable[idx].predictedDistance - actualDistance;
    }

    // Update to last-value prediction
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

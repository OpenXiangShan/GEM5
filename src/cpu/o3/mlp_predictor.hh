#ifndef __CPU_O3_MLP_PREDICTOR_HH__
#define __CPU_O3_MLP_PREDICTOR_HH__

// MLP-aware fetch predictor for SMT processors.
//
// Based on "A Memory-Level Parallelism Aware Fetch Policy for SMT
// Processors" (Eyerman & Eeckhout, HPCA 2007).
//
// The predictor has three components:
//   1. Miss-pattern table: predicts whether a load will be long-latency
//      by tracking the number of short-latency loads between consecutive
//      long-latency loads. If the current interval matches the last
//      observed interval, the load is predicted as long-latency.
//   2. MLP distance table: predicts how many instructions ahead must be
//      fetched to expose the maximum available MLP for a given load PC.
//   3. Long-latency shift register (LLSR): a shift register updated at
//      commit time that tracks which committed instructions are long-latency
//      loads. When a long-latency load exits the register, the actual MLP
//      distance is measured and written back into the MLP distance table.
//
// Tables are updated exclusively by the backend (IEW writeback and commit).
// The frontend only reads predictions at fetch time.
//
// Adaptation for this codebase:
//   - Long-latency = L2 miss (depth >= 2). Value-prediction hits, L1 hits,
//     and L2 hits are all treated as short-latency.
//   - Hash uses (pc >> 1) for RISC-V C-extension compatibility.
//   - Table sizes are configurable, default 2048 entries per thread.

#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/o3/limits.hh"

namespace gem5 {
namespace o3 {

struct MissPatternEntry
{
    uint32_t lastIntervalHits = 0;
    uint32_t currentHits = 0;
    bool valid = false;
    bool hasObservedInterval = false;  // True after second miss establishes an interval
};

struct MLPDistanceEntry
{
    uint32_t predictedDistance = 0;
    bool valid = false;
};

class LongLatencyShiftRegister
{
  public:
    struct Entry
    {
        bool isLongLatencyLoad = false;
        Addr pc = 0;
        // Prediction and table validity captured when this load was fetched.
        uint32_t predictedDistance = 0;
        bool predictionValid = false;
        // Second architectural slot of a fusion; never trains on eviction.
        bool isFusionPlaceholder = false;
    };

    LongLatencyShiftRegister() : entries(), head(0) {}

    void resize(unsigned size) {
        entries.resize(size);
        head = 0;
    }

    unsigned size() const { return entries.size(); }

    // Overwrite all fields, including the fetch-time prediction snapshot.
    Entry push(bool isLL, Addr pc, uint32_t predictedDistance,
               bool predictionValid, bool isFusionPlaceholder) {
        Entry evicted = entries[head];
        entries[head].isLongLatencyLoad = isLL;
        entries[head].pc = pc;
        entries[head].predictedDistance = predictedDistance;
        entries[head].predictionValid = predictionValid;
        entries[head].isFusionPlaceholder = isFusionPlaceholder;
        head = (head + 1) % entries.size();
        return evicted;
    }

    unsigned findLastLongLatencyDistance() const {
        unsigned sz = entries.size();
        unsigned dist = 0;
        // Include other fusions' LL placeholders: their second slot is the
        // conservative endpoint, avoiding compression of pre-fusion distance.
        for (unsigned i = 0; i < sz; i++) {
            unsigned pos = (head + i) % sz;
            if (entries[pos].isLongLatencyLoad) {
                dist = i + 1;
            }
        }
        // Fusion slots are pushed consecutively. An LL placeholder at distance
        // one is the evicted first slot's own second slot, not additional MLP.
        // If it is the last LL slot, train a valid zero distance, even when both
        // original components were LL loads: intra-fusion MLP is not modeled.
        // The observed window is anchored at the first slot, not at the actual
        // LL component; a second-slot load's extra lookahead slot is omitted.
        if (dist == 1 && entries[head].isFusionPlaceholder) {
            return 0;
        }
        return dist;
    }

  private:
    std::vector<Entry> entries;
    unsigned head;
};

class MLPredictorPerThread
{
  public:
    MLPredictorPerThread(unsigned missPatternTableSize,
                         unsigned mlpDistanceTableSize,
                         unsigned shiftRegisterSize);

    // Frontend read-only queries
    bool predictLongLatency(Addr loadPC) const;
    // Return distance (0 if invalid) and optionally validity from the same lookup.
    uint32_t predictMLPDistance(Addr loadPC, bool *outValid = nullptr) const;

    // Backend updates
    int updateOnLoadComplete(
        Addr loadPC, bool actualLongLatency,
        bool predictedLongLatency, uint32_t predictedDistance);
    // Push one architectural slot and its fetch-time prediction snapshot.
    // Non-placeholder LL evictions train regardless of snapshot validity; a
    // valid snapshot yields distance error, otherwise return INT64_MAX.
    // Placeholder evictions never train or produce an error statistic.
    int64_t pushLLSR(bool isLongLatencyLoad, Addr loadPC,
                     uint32_t predictedDistance, bool predictionValid,
                     bool isFusionPlaceholder);

  private:
    unsigned missPatternTableSize;
    unsigned mlpDistanceTableSize;

    std::vector<MissPatternEntry> missPatternTable;
    std::vector<MLPDistanceEntry> mlpDistanceTable;
    LongLatencyShiftRegister shiftRegister;

    unsigned hashIndex(Addr pc, unsigned tableSize) const;
    // Train from the evicted LL load; only a valid fetch-time snapshot yields
    // an error statistic. An invalid snapshot returns INT64_MAX but still trains.
    int64_t updateMLPDistanceTable(
        const LongLatencyShiftRegister::Entry &evicted);
};

class MLPredictor
{
  public:
    MLPredictor(unsigned numThreads, unsigned missPatternTableSize,
                unsigned mlpDistanceTableSize, unsigned shiftRegisterSize);

    MLPredictorPerThread& getPredictor(ThreadID tid);

  private:
    std::vector<MLPredictorPerThread> predictors;
};

} // namespace o3
} // namespace gem5
#endif

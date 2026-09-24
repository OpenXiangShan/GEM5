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
    };

    LongLatencyShiftRegister() : entries(), head(0) {}

    void resize(unsigned size) {
        entries.resize(size);
        head = 0;
    }

    unsigned size() const { return entries.size(); }

    Entry push(bool isLongLatency, Addr pc) {
        Entry evicted = entries[head];
        entries[head].isLongLatencyLoad = isLongLatency;
        entries[head].pc = pc;
        head = (head + 1) % entries.size();
        return evicted;
    }

    unsigned findLastLongLatencyDistance() const {
        unsigned sz = entries.size();
        unsigned dist = 0;
        for (unsigned i = 0; i < sz; i++) {
            unsigned pos = (head + i) % sz;
            if (entries[pos].isLongLatencyLoad) {
                dist = i;
            }
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
    uint32_t predictMLPDistance(Addr loadPC) const;

    // Backend updates
    int updateOnLoadComplete(
        Addr loadPC, bool actualLongLatency,
        bool predictedLongLatency, uint32_t predictedDistance);
    // Returns (predictedDistance - actualDistance) if table was valid, INT32_MAX otherwise
    int pushLLSR(bool isLongLatencyLoad, Addr loadPC);

  private:
    unsigned missPatternTableSize;
    unsigned mlpDistanceTableSize;

    std::vector<MissPatternEntry> missPatternTable;
    std::vector<MLPDistanceEntry> mlpDistanceTable;
    LongLatencyShiftRegister shiftRegister;

    unsigned hashIndex(Addr pc, unsigned tableSize) const;
    int updateMLPDistanceTable(Addr headLoadPC);
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

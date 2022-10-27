#ifndef __CPU_PRED_MODIFY_TAGE_HH__
#define __CPU_PRED_MODIFY_TAGE_HH__

#include <deque>
#include <map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "base/sat_counter.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/timed_pred.hh"
#include "cpu/pred/stream_loop_predictor.hh"
#include "params/StreamTAGE.hh"
#include "debug/DecoupleBP.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

  enum PredSource {
    LoopButInvalid,
    LoopAndValid,
    TAGE
  };

class StreamTAGE : public TimedPredictor
{
    using defer = std::shared_ptr<void>;
    using bitset = boost::dynamic_bitset<>;
  public:
    typedef StreamTAGEParams Params;

    struct TickedStreamStorage : public StreamStorage
    {
        uint64_t tick;
        bool valid;

      public:
        TickedStreamStorage() : tick(0)
        {
            this->bbStart = 0;
            this->controlAddr = 0;
            this->nextStream = 0;
            this->controlSize = 0;
            this->hysteresis = 0;
            this->endType = END_NONE;
        }

        void set(Tick tick_, Addr stream_start_addr, Addr control_addr,
                 Addr next_stream, uint8_t control_size, uint8_t hysteresis_,
                 int end_type, bool valid_, bool end_not_taken)
        {
            this->tick = tick_;
            this->bbStart = stream_start_addr;
            this->controlAddr = control_addr;
            this->nextStream = next_stream;
            this->controlSize = control_size;
            this->hysteresis = hysteresis_;
            this->endType = end_type;
            this->valid = valid_;
        }
    };

  private:
    const unsigned delay{1};

    bool lookupHelper(bool flag, Addr last_chunk_start, Addr stream_start,
                      const bitset& history, TickedStreamStorage* &target,
                      TickedStreamStorage* &alt_target, int& predictor,
                      int& predictor_index, int& alt_predictor,
                      int& alt_predictor_index, int& pred_count,
                      bool& use_alt_pred);

  public:
    StreamTAGE(const Params& p);

    void tickStart() override;

    void tick() override;

    void putPCHistory(Addr cur_chunk_start, Addr stream_start,
                      const bitset& history) override;

    unsigned getDelay() override { return delay; }

    StreamPrediction getStream();

    void update(Addr last_chunk_start_pc, Addr stream_start_pc,
                Addr control_pc, Addr target,
                unsigned control_size, bool actually_taken,
                const bitset &history, EndType endType);

    void commit(Addr, Addr, Addr, bitset&);

  private:
    Addr getTageIndex(Addr pc, const bitset& ghr, int table);

    Addr getTageTag(Addr pc, const bitset& ghr, int table);

    uint64_t getTableGhrLen(int table);

    StreamPrediction prediction;

    const unsigned numPredictors;

    std::vector<unsigned> tableSizes;
    std::vector<unsigned> tableIndexBits;
    std::vector<bitset> tableIndexMasks;
    std::vector<uint64_t> tablePcMasks;
    // std::vector<bitset> indexCalcBuffer;
    std::vector<unsigned> tagSegments;

    std::vector<unsigned> tableTagBits;
    std::vector<bitset> tableTagMasks;
    // std::vector<bitset> tagCalcBuffer;
    std::vector<unsigned> indexSegments;

    std::vector<unsigned> tablePcShifts;
    std::vector<unsigned> histLengths;
    std::vector<bool> hasTag;

    unsigned maxHistLen;

    const unsigned altSelectorSize{128};
    std::vector<int> useAlt; // min:0 max: 15
    const int useAltMin{-7};
    const int useAltMax{8};
    unsigned computeAltSelHash(Addr pc, const bitset& ghr);

    int usefulResetCounter;

    struct DBPStats : public statistics::Group {
        statistics::Scalar coldMisses;
        statistics::Scalar capacityMisses;
        statistics::Scalar compulsoryMisses;
        statistics::Distribution providerTableDist;
        DBPStats(statistics::Group* parent);
    }dbpstats;

    struct PredEntry {
        bool valid = false;
        Addr tag = 0;
        TickedStreamStorage target;
        int useful = 0;
        bool isLoop = false;
    };

    std::vector<std::vector<PredEntry>> tageTable;

    bool equals(const TickedStreamStorage& entry, Addr stream_start_pc,
                Addr control_pc, Addr target);
    
    bool matchTag(Addr expected, Addr found, int table);

    void setTag(Addr &dest, Addr src, int table);

    bool debugFlagOn{false};

    unsigned numTablesToAlloc{1};

    bool satIncrement(int max, int &counter);

    bool satIncrement(TickedStreamStorage &target);

    bool satDecrement(int min, int &counter);

    bool satDecrement(TickedStreamStorage &target);

    void maintainUsefulCounters(int allocated, int new_allocated);

    std::pair<bool, std::pair<bool, Addr>> makeLoopPrediction(bool use_alt_pred, int pred_count, TickedStreamStorage *target, TickedStreamStorage *alt_target);

    StreamLoopPredictor *loopPredictor{};

public:

    void setStreamLoopPredictor(StreamLoopPredictor *slp) {
      loopPredictor = slp;
    }
};

}

}

#endif  // __CPU_PRED_MODIFY_TAGE_HH__

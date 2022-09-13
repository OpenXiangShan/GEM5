#ifndef __CPU_PRED_MODIFY_TAGE_HH__
#define __CPU_PRED_MODIFY_TAGE_HH__

#include <vector>
#include <deque>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/timed_pred.hh"
#include "params/StreamTAGE.hh"
#include "debug/DecoupleBP.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class StreamTAGE : public TimedPredictor
{
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
                 int end_type, bool valid_)
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
                      const bitset& history, TickedStreamStorage& target,
                      TickedStreamStorage& alt_target, int& predictor,
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
    Addr getBaseIndex(Addr pc);

    Addr getTageIndex(Addr pc, const bitset& ghr, int table);

    Addr getTageTag(Addr pc, const bitset& ghr, int table);

    uint64_t getTableGhrLen(int table);

    StreamPrediction prediction;

    const unsigned numPredictors;
    const unsigned baseTableSize;

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

    unsigned maxHistLen;

    int use_alt; // min:0 max: 15
    int reset_counter;

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
        int counter = 0;
        int useful = 0;
    };

    bitset ghr;
    std::vector<std::vector<PredEntry>> tageTable;
    std::vector<TickedStreamStorage> baseTable;

    bool equals(const TickedStreamStorage& entry, Addr stream_start_pc,
                Addr control_pc, Addr target);

    bool debugFlagOn{false};

    unsigned numTablesToAlloc{1};
};

}

}

#endif  // __CPU_PRED_MODIFY_TAGE_HH__

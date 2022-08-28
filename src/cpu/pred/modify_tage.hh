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

        public:
         TickedStreamStorage()
             : tick(0) {
            this->bbStart = 0;
            this->controlAddr = 0;
            this->nextStream = 0;
            this->controlSize = 0;
            this->hysteresis = 0;
            this->endIsRet = 0;
        }
    };

  private:
    const unsigned delay{1};

    bool lookup_helper(bool, Addr,const bitset&, TickedStreamStorage&, TickedStreamStorage&, int&, int&, int&, int&, int&, bool&);

  public:
    StreamTAGE(const Params &p);

    void tickStart() override;

    void tick() override;

    void putPCHistory(Addr pc,
                      const bitset &history) override;

    unsigned getDelay() override { return delay; }

    StreamPrediction getStream();

    void update(Addr stream_start_pc,
                Addr control_pc, Addr target,
                unsigned control_size, bool actually_taken,
                const bitset &history);

    void commit(Addr, Addr, Addr, bitset&);

    uint64_t makePCHistTag(Addr pc, const bitset &history);

    uint64_t getAddrFold(uint64_t address, int table);

    uint64_t getTag(Addr pc, const bitset& ghr, int table);

    uint64_t getTableGhrLen(int table);

    StreamPrediction prediction;

    const unsigned numPredictors;
    const unsigned simpleBTBSize;
    std::vector<int> tableSizes;
    std::vector<int> TTagBitSizes;
    std::vector<int> TTagPcShifts;
    std::vector<int> histLengths;

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
    std::vector<std::vector<PredEntry>  >targetCache;
    TickedStreamStorage previous_target;
    std::vector<bool> base_predictor_valid;
    std::vector<TickedStreamStorage> base_predictor;

  private:
    bool equals(TickedStreamStorage, Addr, Addr, Addr);
};

}

}

#endif  // __CPU_PRED_MODIFY_TAGE_HH__

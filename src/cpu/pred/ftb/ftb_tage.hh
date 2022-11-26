#ifndef __CPU_PRED_FTB_TAGE_HH__
#define __CPU_PRED_FTB_TAGE_HH__

#include <deque>
#include <map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "cpu/pred/ftb/timed_base_pred.hh"
#include "debug/DecoupleBP.hh"
#include "params/FTBTAGE.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

class FTBTAGE : public TimedBaseFTBPredictor
{
    using defer = std::shared_ptr<void>;
    using bitset = boost::dynamic_bitset<>;
  public:
    typedef FTBTAGEParams Params;

    struct TageEntry
    {
        public:
            bool valid;
            Addr startAddr;
            Addr tag;
            short counter;
            bool useful;

            TageEntry() : valid(false), pc(0), tag(0), counter(0), useful(false) {}

            TageEntry(Addr startAddr, Addr tag, short counter) :
                      valid(true), startAddr(startAddr),
                      tag(tag), counter(counter), useful(false) {}

    };

    struct TagePrediction
    {
        public:
            bool mainFound;
            short mainCounter;
            short altCounter;
            unsigned table;
            Addr index;
            Addr tag;
            unsigned counter;
            bool useAlt;
            bitset usefulMask;

            TagePrediction() : mainFound(false), mainCounter(0), altTaken(false),
                                table(0), index(0), tag(0), counter(0), useAlt(false) {}

            TagePrediction(bool mainFound, short mainCounter, short altCounter, unsigned table,
                            Addr index, Addr tag, unsigned counter, bool useAlt) :
                            mainFound(mainFound), mainCounter(mainCounter), altCounter(altCounter),
                            table(table), index(index), tag(tag), counter(counter), useAlt(useAlt) {}


    };

  public:
    FTBTAGE(const Params& p);

    void tickStart() override;

    void tick() override;
    // make predictions, record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::array<FullFTBPrediction> &stagePreds) override;

    void update(const boost::dynamic_bitset<> &history, Addr startAddr, const TagePrediction pred, bool actualTaken);

    void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

    unsigned getDelay() override { return 1; }

  private:
    void lookupHelper(Addr stream_start,
                      TageEntry &main_entry, TageEntry &alt_entry,
                      int& main_table, int& main_table_index,
                      int& alt_table, int& alt_table_index,
                      int& provider_counts, bool& use_alt_pred,
                      bitset &usefulMask);

    Addr getTageIndex(Addr pc, int table);

    Addr getTageTag(Addr pc, int table);

    const unsigned numPredictors;

    std::vector<unsigned> tableSizes;
    std::vector<unsigned> tableIndexBits;
    std::vector<bitset> tableIndexMasks;
    std::vector<uint64_t> tablePcMasks;
    std::vector<unsigned> tableTagBits;
    std::vector<bitset> tableTagMasks;
    std::vector<unsigned> tablePcShifts;
    std::vector<unsigned> histLengths;
    std::vector<bitset> tagFoldedHist;
    std::vector<bitset> indexFoldedHist;

    unsigned maxHistLen;

    std::vector<std::vector<TageEntry>> tageTable;

    std::vector<short> baseTable;

    std::vector<int> useAlt;

    bool matchTag(Addr expected, Addr found);

    void setTag(Addr &dest, Addr src, int table);

    bool debugFlagOn{false};

    unsigned numTablesToAlloc{1};

    void updateCounter(bool actualTaken, bool predTaken, short &counter);

    bool satIncrement(int max, short &counter);

    bool satDecrement(int min, short &counter);

    Addr getUseAltIdx(Addr pc);

    int usefulResetCnt;

public:

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);
};
}

}

}

#endif  // __CPU_PRED_FTB_TAGE_HH__

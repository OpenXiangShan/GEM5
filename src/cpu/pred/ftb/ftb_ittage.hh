#ifndef __CPU_PRED_FTB_ITTAGE_HH__
#define __CPU_PRED_FTB_ITTAGE_HH__

#include <deque>
#include <map>
#include <vector>
#include <utility>

#include "base/statistics.hh"
#include "base/types.hh"
#include "base/sat_counter.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/folded_hist.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "cpu/pred/ftb/timed_base_pred.hh"
#include "params/FTBITTAGE.hh"
#include "debug/DecoupleBP.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

class FTBITTAGE : public TimedBaseFTBPredictor
{
    using defer = std::shared_ptr<void>;
    using bitset = boost::dynamic_bitset<>;
  public:
    typedef FTBITTAGEParams Params;

    struct TageEntry
    {
        public:
            bool valid;
            Addr tag;
            Addr target;
            short counter;
            bool useful;

            TageEntry() : valid(false), tag(0), target(0), counter(0), useful(false) {}

            TageEntry(Addr tag, Addr target, short counter) :
                        valid(true), tag(tag), target(target), counter(counter), useful(false) {}

    };

    struct TagePrediction
    {
        public:
            bool mainFound;
            bool altFound;
            TageEntry mainEntry;
            TageEntry altEntry;
            unsigned main_table;
            unsigned alt_table;
            Addr main_index;
            Addr alt_index;
            bool useAlt;
            bool useBase;
            bitset usefulMask;

            TagePrediction() : mainFound(false), altFound(false), mainEntry(TageEntry()), altEntry(TageEntry()),
                                 main_table(0), alt_table(0), main_index(0), alt_index(0), useAlt(false), useBase(false) {}

            TagePrediction(bool mainFound, bool altFound, TageEntry mainEntry, TageEntry altEntry,
                            unsigned main_table, unsigned alt_table, Addr main_index, Addr alt_index,
                            bool useAlt, bool useBase, bitset usefulMask) :
                            mainFound(mainFound), altFound(altFound), mainEntry(mainEntry), altEntry(altEntry),
                            main_table(main_table), alt_table(alt_table), main_index(main_index), alt_index(alt_index),
                            useAlt(useAlt), useBase(useBase), usefulMask(usefulMask) {}

    };

  public:
    FTBITTAGE(const Params& p);

    void tickStart() override;

    void tick() override;
    // make predictions, record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::vector<FullFTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

    void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;

    void update(const FetchStream &entry) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

  private:


    
    // return provided
    std::pair<bool, bool> lookupHelper(Addr stream_start, TageEntry &main_entry, int &main_table, int &main_table_index,
                                       TageEntry &alt_entry, int &alt_table, int &alt_table_index, bool &use_alt_pred,
                                       bool &use_base_table, bitset &usefulMask);

    Addr getTageIndex(Addr pc, int table);

    Addr getTageIndex(Addr pc, int table, bitset &foldedHist);

    Addr getTageTag(Addr pc, int table);

    Addr getTageTag(Addr pc, int table, bitset &foldedHist, bitset &altFoldedHist);

    void doUpdateHist(const bitset &history, int shamt, bool taken);

    const unsigned numPredictors;

    std::vector<unsigned> tableSizes;
    std::vector<unsigned> tableIndexBits;
    std::vector<bitset> tableIndexMasks;
    // std::vector<uint64_t> tablePcMasks;
    std::vector<unsigned> tableTagBits;
    std::vector<bitset> tableTagMasks;
    std::vector<unsigned> tablePcShifts;
    std::vector<unsigned> histLengths;
    std::vector<FoldedHist> tagFoldedHist;
    std::vector<FoldedHist> altTagFoldedHist;
    std::vector<FoldedHist> indexFoldedHist;

    LFSR64 allocLFSR;

    unsigned maxHistLen;

    std::vector<std::vector<TageEntry>> tageTable;

    bool matchTag(Addr expected, Addr found);

    void setTag(Addr &dest, Addr src, int table);

    bool debugFlagOn{false};

    unsigned numTablesToAlloc;

    unsigned numBr;

    unsigned instShiftAmt {1};

    void updateCounter(bool taken, unsigned width, short &counter);

    bool satIncrement(int max, short &counter);

    bool satDecrement(int min, short &counter);

    int usefulResetCnt;

    typedef struct TageMeta {
        TagePrediction pred;
        std::vector<FoldedHist> tagFoldedHist;
        std::vector<FoldedHist> altTagFoldedHist;
        std::vector<FoldedHist> indexFoldedHist;
        TageMeta(TagePrediction pred, std::vector<FoldedHist> tagFoldedHist,
            std::vector<FoldedHist> altTagFoldedHist, std::vector<FoldedHist> indexFoldedHist) :
            pred(pred), tagFoldedHist(tagFoldedHist), altTagFoldedHist(altTagFoldedHist), indexFoldedHist(indexFoldedHist) {}
        TageMeta() {}
        TageMeta(const TageMeta &other) {
            pred = other.pred;
            tagFoldedHist = other.tagFoldedHist;
            altTagFoldedHist = other.altTagFoldedHist;
            indexFoldedHist = other.indexFoldedHist;
        }
    } TageMeta;

    TageMeta meta;

public:

    Addr debugPC = 0;
    Addr debugPC2 = 0;
    bool debugFlag = false;

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);
};
}

}

}

#endif  // __CPU_PRED_FTB_ITTAGE_HH__
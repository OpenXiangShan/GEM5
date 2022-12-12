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
            Addr mainTarget;
            Addr altTarget;
            unsigned table;
            Addr index;
            Addr tag;
            unsigned counter;
            bool useAlt;
            bitset usefulMask;

            TagePrediction() : mainFound(false), mainTarget(0), altTarget(0),
                                 table(0), index(0), tag(0), counter(0), useAlt(false) {}

            TagePrediction(bool mainFound, Addr mainTarget, Addr altTarget,
                            unsigned table, Addr index, Addr tag, unsigned counter,
                            bool useAlt, bitset usefulMask) :
                            mainFound(mainFound), mainTarget(mainTarget), altTarget(altTarget),
                            table(table), index(index), tag(tag), counter(counter), useAlt(useAlt),
                            usefulMask(usefulMask) {}

    };

  public:
    FTBITTAGE(const Params& p);

    void tickStart() override;

    void tick() override;
    // make predictions, record in stage preds
    void putPCHistory(Addr startAddr,
                      const boost::dynamic_bitset<> &history,
                      std::array<FullFTBPrediction, 3> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

    void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;

    void update(const FetchStream &entry) override;

    unsigned getDelay() override { return 1; }

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

  private:


    
    // return provided
    std::vector<bool> lookupHelper(Addr stream_start,
                        std::vector<TageEntry> &main_entries,
                        std::vector<int> &main_tables, std::vector<int> &main_table_indices,
                        std::vector<bool> &use_alt_preds, std::vector<bitset> &usefulMasks);

    Addr getTageIndex(Addr pc, int table);

    Addr getTageIndex(Addr pc, int table, bitset &foldedHist);

    Addr getTageTag(Addr pc, int table);

    Addr getTageTag(Addr pc, int table, bitset &foldedHist);

    unsigned getBaseTableIndex(Addr pc);

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
    std::vector<FoldedHist> indexFoldedHist;

    unsigned maxHistLen;

    std::vector<std::vector<std::vector<TageEntry>>> tageTable;

    std::vector<std::vector<std::pair<Addr, short>>> baseTable;

    std::vector<std::vector<short>> useAlt;

    bool matchTag(Addr expected, Addr found);

    void setTag(Addr &dest, Addr src, int table);

    bool debugFlagOn{false};

    unsigned numTablesToAlloc;

    unsigned numBr;

    unsigned instShiftAmt {1};

    void updateCounter(bool taken, unsigned width, short &counter);

    bool satIncrement(int max, short &counter);

    bool satDecrement(int min, short &counter);

    Addr getUseAltIdx(Addr pc);

    std::vector<int> usefulResetCnt;

    typedef struct TageMeta {
        std::vector<TagePrediction> preds;
        std::vector<FoldedHist> tagFoldedHist;
        std::vector<FoldedHist> indexFoldedHist;
        TageMeta(std::vector<TagePrediction> preds, std::vector<FoldedHist> tagFoldedHist, std::vector<FoldedHist> indexFoldedHist) :
            preds(preds), tagFoldedHist(tagFoldedHist), indexFoldedHist(indexFoldedHist) {}
        TageMeta() {}
        TageMeta(const TageMeta &other) {
            preds = other.preds;
            tagFoldedHist = other.tagFoldedHist;
            indexFoldedHist = other.indexFoldedHist;
        }
    } TageMeta;

    TageMeta meta;

public:

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);
};
}

}

}

#endif  // __CPU_PRED_FTB_ITTAGE_HH__
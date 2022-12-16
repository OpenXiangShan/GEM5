#ifndef __CPU_PRED_FTB_TAGE_HH__
#define __CPU_PRED_FTB_TAGE_HH__

#include <deque>
#include <map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/folded_hist.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "cpu/pred/ftb/timed_base_pred.hh"
#include "debug/DecoupleBP.hh"
#include "debug/FTBTAGEUseful.hh"
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
            Addr tag;
            short counter;
            bool useful;

            TageEntry() : valid(false), tag(0), counter(0), useful(false) {}

            TageEntry(Addr tag, short counter) :
                      valid(true), tag(tag), counter(counter), useful(false) {}

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
            bool useAlt;
            bitset usefulMask;

            TagePrediction() : mainFound(false), mainCounter(0), altCounter(0),
                                table(0), index(0), tag(0), useAlt(false) {}

            TagePrediction(bool mainFound, short mainCounter, short altCounter, unsigned table,
                            Addr index, Addr tag, bool useAlt, bitset usefulMask) :
                            mainFound(mainFound), mainCounter(mainCounter), altCounter(altCounter),
                            table(table), index(index), tag(tag), useAlt(useAlt), usefulMask(usefulMask) {}


    };

  public:
    FTBTAGE(const Params& p);
    ~FTBTAGE();

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

    LFSR64 allocLFSR;

    unsigned maxHistLen;

    std::vector<std::vector<std::vector<TageEntry>>> tageTable;

    std::vector<std::vector<short>> baseTable;

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

    struct TageBankStats : public statistics::Group {
        statistics::Distribution predTableHits;
        statistics::Scalar predNoHitUseBim;
        statistics::Scalar predUseAlt;
        statistics::Distribution updateTableHits;
        statistics::Scalar updateNoHitUseBim;
        statistics::Scalar updateUseAlt;
    
        statistics::Scalar updateUseAltCorrect;
        statistics::Scalar updateUseAltWrong;
        statistics::Scalar updateAltDiffers;
        statistics::Scalar updateUseAltOnNaUpdated;
        statistics::Scalar updateUseAltOnNaInc;
        statistics::Scalar updateUseAltOnNaDec;
        statistics::Scalar updateProviderNa;
        statistics::Scalar updateUseNaCorrect;
        statistics::Scalar updateUseNaWrong;
        statistics::Scalar updateUseAltOnNa;
        statistics::Scalar updateUseAltOnNaCorrect;
        statistics::Scalar updateUseAltOnNaWrong;
        statistics::Scalar updateAllocFailure;
        statistics::Scalar updateAllocSuccess;
        statistics::Scalar updateMispred;
        statistics::Scalar updateResetU;
        statistics::Distribution updateResetUCtrInc;
        statistics::Distribution updateResetUCtrDec;

        int bankIdx;
        int numPredictors;

        TageBankStats(statistics::Group* parent, const char *name, int numPredictors);
        void updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred);
    } ;
    
    TageBankStats **tageBankStats;

public:

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);
};
}

}

}

#endif  // __CPU_PRED_FTB_TAGE_HH__

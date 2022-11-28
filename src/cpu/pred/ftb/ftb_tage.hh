#ifndef __CPU_PRED_FTB_TAGE_HH__
#define __CPU_PRED_FTB_TAGE_HH__

#include <deque>
#include <map>
#include <vector>
#include <utility>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/folded_hist.hh"
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
            unsigned counter;
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

public:

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);
public:
    class StatisticalCorrector {
      public:
        StatisticalCorrector() {
          scCntTable.resize(8);
          tagVec.resize(2048);
          for (int i = 0; i < 8; i++) {
            scCntTable[i].resize(2048);
          }
        };

      public:
        Addr getIndex(Addr pc, int table);

        void updateGEHL(bool actualTaken, std::pair<bool, int> prediction,
                        Addr pc);

        std::pair<bool, float> getPrediction(Addr pc);

        void updateThreshold(bool actualTaken, std::pair<bool, int> prediction);

      private:
        float threshold = 0;

        int TC = 0;

        std::vector<bool> tagVec;

        std::vector<std::vector<int>> scCntTable;

        std::vector<unsigned> cntSizeTable {5, 5, 4, ,4 ,4 ,4 ,4 ,4};

        std::vector<unsigned> shortHistLen {0, 3, 5, 8, 12, 19, 31, 49};

        std::vector<unsigned> longHistLen {0, 0, 79, 0, 125, 0, 200, 0};
    }
};
}

}

}

#endif  // __CPU_PRED_FTB_TAGE_HH__

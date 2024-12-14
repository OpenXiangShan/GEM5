#ifndef __CPU_PRED_FTB_TAGE_HH__
#define __CPU_PRED_FTB_TAGE_HH__

#include <deque>
#include <map>
#include <vector>
#include <utility>

#include "base/sat_counter.hh"
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
            bool mainUseful;
            short altCounter;
            unsigned table;
            Addr index;
            Addr tag;
            bool useAlt;
            bitset usefulMask;
            bool taken;

            TagePrediction() : mainFound(false), mainCounter(0), mainUseful(false), altCounter(0),
                                table(0), index(0), tag(0), useAlt(false), taken(false) {}

            TagePrediction(bool mainFound, short mainCounter, bool mainUseful, short altCounter, unsigned table,
                            Addr index, Addr tag, bool useAlt, bitset usefulMask, bool taken) :
                            mainFound(mainFound), mainCounter(mainCounter), mainUseful(mainUseful), altCounter(altCounter),
                            table(table), index(index), tag(tag), useAlt(useAlt), usefulMask(usefulMask),
                            taken(taken) {}


    };

  public:
    FTBTAGE(const Params& p);
    ~FTBTAGE();

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

    void setTrace() override;

    // check folded hists after speculative update and recover
    void checkFoldedHist(const bitset &history, const char *when);

    // we hash between numBr br slots, depending on lower bits of pc
    // br slot 0 may be tage entry 0 or 1
    Addr getBrIndexUnshuffleBits(Addr pc);

    Addr getShuffledBrIndex(Addr pc, int brIdxToShuffle);

  private:


    
    // return provided
    std::vector<bool> lookupHelper(Addr stream_start,
                        std::vector<TageEntry> &main_entries,
                        std::vector<int> &main_tables, std::vector<int> &main_table_indices,
                        std::vector<bool> &use_alt_preds, std::vector<bitset> &usefulMasks);

    Addr getTageIndex(Addr pc, int table);

    Addr getTageIndex(Addr pc, int table, bitset &foldedHist);

    Addr getTageTag(Addr pc, int table);

    Addr getTageTag(Addr pc, int table, bitset &foldedHist, bitset &altFoldedHist);

    unsigned getBaseTableIndex(Addr pc);

    void doUpdateHist(const bitset &history, int shamt, bool taken);

    const unsigned numPredictors;

    unsigned baseTableSize;

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



    std::vector<Addr> tageIndex;

    std::vector<Addr> tageTag;

    bool enableSC;

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

        statistics::Vector updateTableMispreds;

        statistics::Scalar scAgreeAtPred;
        statistics::Scalar scAgreeAtCommit;
        statistics::Scalar scDisagreeAtPred;
        statistics::Scalar scDisagreeAtCommit;
        statistics::Scalar scConfAtPred;
        statistics::Scalar scConfAtCommit;
        statistics::Scalar scUnconfAtPred;
        statistics::Scalar scUnconfAtCommit;
        statistics::Scalar scUpdateOnMispred;
        statistics::Scalar scUpdateOnUnconf;
        statistics::Scalar scUsedAtPred;
        statistics::Scalar scUsedAtCommit;
        statistics::Scalar scCorrectTageWrong;
        statistics::Scalar scWrongTageCorrect;

        int bankIdx;
        int numPredictors;

        TageBankStats(statistics::Group* parent, const char *name, int numPredictors);
        void updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred);
    } ;
    
    TageBankStats **tageBankStats;

    TraceManager *tageMissTrace;

public:

    void recoverFoldedHist(const bitset& history);

    // void checkFoldedHist(const bitset& history);


    struct TageMissTrace : public Record {
        void set(uint64_t startPC, uint64_t branchPC, uint64_t lgcBank, uint64_t phyBank, uint64_t mainFound, uint64_t mainCounter, uint64_t mainUseful,
            uint64_t altCounter, uint64_t mainTable, uint64_t mainIndex, uint64_t altIndex, uint64_t tag,
            uint64_t useAlt, uint64_t predTaken, uint64_t actualTaken, uint64_t allocSuccess, uint64_t allocFailure,
            uint64_t predUseSC, uint64_t predSCDisagree, uint64_t predSCCorrect)
        {
            _tick = curTick();
            _uint64_data["startPC"] = startPC;
            _uint64_data["branchPC"] = branchPC;
            _uint64_data["lgcBank"] = lgcBank;
            _uint64_data["phyBank"] = phyBank;
            _uint64_data["mainFound"] = mainFound;
            _uint64_data["mainCounter"] = mainCounter;
            _uint64_data["mainUseful"] = mainUseful;
            _uint64_data["altCounter"] = altCounter;
            _uint64_data["mainTable"] = mainTable;
            _uint64_data["mainIndex"] = mainIndex;
            _uint64_data["altIndex"] = altIndex;
            _uint64_data["tag"] = tag;
            _uint64_data["useAlt"] = useAlt;
            _uint64_data["predTaken"] = predTaken;
            _uint64_data["actualTaken"] = actualTaken;
            _uint64_data["allocSuccess"] = allocSuccess;
            _uint64_data["allocFailure"] = allocFailure;
            _uint64_data["predUseSC"] = predUseSC;
            _uint64_data["predSCDisagree"] = predSCDisagree;
            _uint64_data["predSCCorrect"] = predSCCorrect;
        }
    };
public:
    class StatisticalCorrector {
      public:
        // TODO: parameterize
        StatisticalCorrector(int numBr, FTBTAGE *tage) : numBr(numBr), tage(tage) {
          scCntTable.resize(numPredictors);
          tableIndexBits.resize(numPredictors);
          for (int i = 0; i < numPredictors; i++) {
            tableIndexBits[i] = ceilLog2(tableSizes[i]);
            foldedHist.push_back(FoldedHist(histLens[i], tableIndexBits[i], numBr));
            scCntTable[i].resize(tableSizes[i]);
            for (auto &br_counters : scCntTable[i]) {
              br_counters.resize(numBr);
              for (auto &tOrNt : br_counters) {
                tOrNt.resize(2, 0);
              }
            }
          }
          // initial theshold
          thresholds.resize(numBr, 6);
          TCs.resize(numBr, neutralVal);
        };

        typedef struct SCPrediction {
            bool tageTaken;
            bool scUsed;
            bool scPred;
            int scSum;
            SCPrediction() : tageTaken(false), scUsed(false), scPred(false), scSum(0) {}
        } SCPrediction;

        typedef struct SCMeta {
            std::vector<FoldedHist> indexFoldedHist;
            std::vector<SCPrediction> scPreds;
        } SCMeta;

      public:
        Addr getIndex(Addr pc, int t);

        Addr getIndex(Addr pc, int t, bitset &foldedHist);

        std::vector<FoldedHist> getFoldedHist();

        std::vector<SCPrediction> getPredictions(Addr pc, std::vector<TagePrediction> &tagePreds);

        void update(Addr pc, SCMeta meta, std::vector<bool> needToUpdates, std::vector<bool> actualTakens);

        void recoverHist(std::vector<FoldedHist> &fh);

        void doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool cond_taken);

        void setStats(std::vector<TageBankStats *> stats) {
          this->stats = stats;
        }

      private:
        int numBr;

        FTBTAGE *tage;

        int numPredictors = 4;

        int scCounterWidth = 6;

        std::vector<int> thresholds;

        int minThres = 5;

        int maxThres = 31;

        std::vector<int> TCs;

        int TCWidth = 6;

        int neutralVal = 0;

        std::vector<FoldedHist> foldedHist;

        std::vector<int> tableIndexBits;

        // std::vector<bool> tagVec;

        // table - table index - numBr - taken/not taken
        std::vector<std::vector<std::vector<std::vector<int>>>> scCntTable;

        std::vector<unsigned> histLens {0, 4, 10, 16};

        std::vector<int> tableSizes {256, 256, 256, 256};

        std::vector<int> tablePcShifts {1, 1, 1, 1};

        bool satPos(int &counter, int counterBits);

        bool satNeg(int &counter, int counterBits);

        void counterUpdate(int &ctr, int nbits, bool taken);

        std::vector<TageBankStats*> stats;

    };

    StatisticalCorrector sc;

private:
    using SCMeta = StatisticalCorrector::SCMeta;
    typedef struct TageMeta {
        std::vector<TagePrediction> preds;
        std::vector<FoldedHist> tagFoldedHist;
        std::vector<FoldedHist> altTagFoldedHist;
        std::vector<FoldedHist> indexFoldedHist;
        SCMeta scMeta;
        TageMeta(std::vector<TagePrediction> preds, std::vector<FoldedHist> tagFoldedHist,
            std::vector<FoldedHist> altTagFoldedHist, std::vector<FoldedHist> indexFoldedHist, SCMeta scMeta) :
            preds(preds), tagFoldedHist(tagFoldedHist), altTagFoldedHist(altTagFoldedHist), indexFoldedHist(indexFoldedHist),
            scMeta(scMeta) {}
        TageMeta() {}
        TageMeta(const TageMeta &other) {
            preds = other.preds;
            tagFoldedHist = other.tagFoldedHist;
            altTagFoldedHist = other.altTagFoldedHist;
            indexFoldedHist = other.indexFoldedHist;
            scMeta = other.scMeta;
        }
    } TageMeta;

    TageMeta meta;
};
}

}

}

#endif  // __CPU_PRED_FTB_TAGE_HH__

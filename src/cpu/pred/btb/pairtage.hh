#ifndef __CPU_PRED_BTB_PAIRTAGE_HH__
#define __CPU_PRED_BTB_PAIRTAGE_HH__

#include <array>
#include <memory>
#include <queue>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "params/PairTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class PairTAGE : public TimedBaseBTBPredictor
{
    using bitset = boost::dynamic_bitset<>;

  public:
    typedef PairTAGEParams Params;

    struct PTAGEEntryModelInfo
    {
        bool dummy;

        PTAGEEntryModelInfo() : dummy(0) {}
    };

    struct PTAGEBlockInfo
    {
        Addr directionCounter;
        Addr branchPC;
        Addr targetPC;

        PTAGEBlockInfo(): directionCounter(0), branchPC(0), targetPC(0) {}
    };

    struct PTAGEEntry
    {
        bool valid;
        bool b2Valid;
        Addr tag;
        bool useful;

        PTAGEBlockInfo block;
        PTAGEBlockInfo block2;

        PTAGEEntryModelInfo modelInfo;

        PTAGEEntry() : valid(false), b2Valid(false), tag(0), useful(false) {}
    };

    PairTAGE(const Params &p);

    // The predictor is wired into the BPU but does not predict yet: it
    // leaves every stage prediction untouched.
    void putPCHistory(Addr startAddr, const bitset &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;

    void specUpdatePHist(const bitset &history, FullBTBPrediction &pred,
                         const PathHistoryUpdate &update) override;

    void recoverPHist(const bitset &history,
                      const HistoryRecoveryContext &context,
                      const PathHistoryUpdate &update) override;

    // Fast training hook, driven by the final-stage prediction of every
    // predicted block, like the other s1 predictors.
    void updateUsingS3Pred(FullBTBPrediction &s3Pred);

  private:
    // Number of PairTAGE tables
    const unsigned numPredictors;

    // Number of entries in each table, summed over all banks and ways
    std::vector<unsigned> tableSizes;

    // Number of bits used for indexing each table, bank bits included
    std::vector<unsigned> tableIndexBits;

    // Number of bits used for tags in each table
    std::vector<unsigned> tableTagBits;

    // History lengths for each table
    std::vector<unsigned> histLengths;

    // Number of ways per set; the RTL tables are direct mapped
    const unsigned numWays;

    // ========== Bank Configuration ==========
    // Each table is split into banks selected by low PC bits, like the RTL
    // SRAM banks. A bank owns a contiguous range of indices, so an index is
    // laid out as {bank, set}.
    const unsigned numBanks;         // Number of banks (e.g., 4)
    const unsigned bankIdWidth;      // log2(numBanks), computed in constructor

    // The PairTAGE prediction tables (table x index x way)
    std::vector<std::vector<std::vector<PTAGEEntry>>> table;

    struct PairTAGEStats : public statistics::Group
    {
        statistics::Scalar lookups;
        statistics::Scalar s3TrainEvents;

        PairTAGEStats(statistics::Group *parent);
    } pairTageStats;
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#endif

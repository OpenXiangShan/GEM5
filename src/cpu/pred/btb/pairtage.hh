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
    };

    struct PTAGEBlockInfo
    {
        Addr directionCounter;
        Addr branchPC;
        Addr targetPC;
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
    const unsigned numPredictors;
    const std::vector<unsigned> tableSizes;
    const std::vector<unsigned> tableTagBits;
    const std::vector<unsigned> histLengths;

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

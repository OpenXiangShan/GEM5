#include "cpu/pred/btb/pairtage.hh"

#include "base/logging.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

PairTAGE::PairTAGE(const Params &p)
    : TimedBaseBTBPredictor(p),
      numPredictors(p.numPredictors),
      tableSizes(p.tableSizes),
      tableTagBits(p.TTagBitSizes),
      histLengths(p.histLengths),
      pairTageStats(this)
{
    fatal_if(tableSizes.size() < numPredictors,
             "PairTAGE: tableSizes has %zu entries, need %u",
             tableSizes.size(), numPredictors);
    fatal_if(tableTagBits.size() < numPredictors,
             "PairTAGE: TTagBitSizes has %zu entries, need %u",
             tableTagBits.size(), numPredictors);
    fatal_if(histLengths.size() < numPredictors,
             "PairTAGE: histLengths has %zu entries, need %u",
             histLengths.size(), numPredictors);
}

PairTAGE::PairTAGEStats::PairTAGEStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "prediction lookups issued to PairTAGE"),
      ADD_STAT(s3TrainEvents, statistics::units::Count::get(),
               "final-stage predictions delivered to PairTAGE for training")
{
}

void
PairTAGE::putPCHistory(Addr startAddr, const bitset &history,
                       std::vector<FullBTBPrediction> &stagePreds)
{
    (void)startAddr;
    (void)history;
    (void)stagePreds;
    pairTageStats.lookups++;
}

std::shared_ptr<void>
PairTAGE::getPredictionMeta(ThreadID tid)
{
    (void)tid;
    return nullptr;
}

void
PairTAGE::specUpdatePHist(const bitset &history, FullBTBPrediction &pred,
                          const PathHistoryUpdate &update)
{
    (void)history;
    (void)pred;
    (void)update;
}

void
PairTAGE::recoverPHist(const bitset &history,
                       const HistoryRecoveryContext &context,
                       const PathHistoryUpdate &update)
{
    (void)history;
    (void)context;
    (void)update;
}

void
PairTAGE::updateUsingS3Pred(FullBTBPrediction &s3Pred)
{
    (void)s3Pred;
    pairTageStats.s3TrainEvents++;
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

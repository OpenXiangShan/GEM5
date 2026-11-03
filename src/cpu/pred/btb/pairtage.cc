#include "cpu/pred/btb/pairtage.hh"

#include "base/bitfield.hh"
#include "base/intmath.hh"
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
      numWays(p.numWays),
      numBanks(p.numBanks),
      bankIdWidth(ceilLog2(p.numBanks)),
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
    fatal_if(numWays == 0, "PairTAGE: numWays must be > 0");
    fatal_if(!isPowerOf2(numBanks),
             "PairTAGE: numBanks (%u) must be a power of two", numBanks);

    table.resize(numPredictors);
    tableIndexBits.resize(numPredictors);

    for (unsigned int i = 0; i < numPredictors; ++i) {
        // Every bank of a table holds the same number of sets, and both are
        // selected by bit fields, so the size has to split evenly.
        fatal_if(!isPowerOf2(tableSizes[i]) || tableSizes[i] < numBanks,
                 "PairTAGE: tableSizes[%u] (%u) must be a power of two and "
                 "at least numBanks (%u)", i, tableSizes[i], numBanks);

        table[i].resize(tableSizes[i]);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            table[i][j].resize(numWays);
        }

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
    }
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

unsigned
PairTAGE::getBankId(Addr pc) const
{
    return (pc >> instShiftAmt) & mask(bankIdWidth);
}

Addr
PairTAGE::getSetIndex(Addr pc, unsigned t, uint64_t foldedHist) const
{
    const unsigned setBits = tableIndexBits[t] - bankIdWidth;
    const Addr pcBits = pc >> (instShiftAmt + bankIdWidth);
    return (pcBits ^ foldedHist) & mask(setBits);
}

Addr
PairTAGE::getIndex(unsigned t, unsigned bank, Addr set) const
{
    const unsigned setBits = tableIndexBits[t] - bankIdWidth;
    return (static_cast<Addr>(bank) << setBits) | set;
}

Addr
PairTAGE::getTag(Addr pc, unsigned t, uint64_t foldedHist,
                 uint64_t altFoldedHist) const
{
    const Addr pcBits = pc >> (instShiftAmt + tableIndexBits[t]);
    return (pcBits ^ foldedHist ^ (altFoldedHist << 1)) &
           mask(tableTagBits[t]);
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

#include "cpu/pred/btb/pairtage.hh"

#include <cassert>

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
      pairTageStats(this, p.numPredictors)
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
    for (unsigned i = 1; i < numPredictors; ++i) {
        fatal_if(histLengths[i] <= histLengths[i - 1],
                 "PairTAGE: histLengths must be strictly increasing, "
                 "histLengths[%u] (%u) <= histLengths[%u] (%u)",
                 i, histLengths[i], i - 1, histLengths[i - 1]);
    }
    fatal_if(numWays == 0, "PairTAGE: numWays must be > 0");
    fatal_if(!isPowerOf2(numBanks),
             "PairTAGE: numBanks (%u) must be a power of two", numBanks);

    table.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    threadHistory.resize(MaxThreads);
    threadMeta.resize(MaxThreads);

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

        const int setBits = tableIndexBits[i] - bankIdWidth;
        for (auto &state : threadHistory) {
            state.indexFoldedHist.emplace_back(histLengths[i], setBits, 16);
            state.tagFoldedHist.emplace_back(
                histLengths[i], tableTagBits[i], 16);
            state.altTagFoldedHist.emplace_back(
                histLengths[i], tableTagBits[i] - 1, 16);
        }
    }
}

PairTAGE::PairTAGEStats::PairTAGEStats(statistics::Group *parent,
                                       unsigned numPredictors)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "prediction lookups issued to PairTAGE"),
      ADD_STAT(providerTable, statistics::units::Count::get(),
               "lookups provided by each table"),
      ADD_STAT(s3TrainEvents, statistics::units::Count::get(),
               "final-stage predictions delivered to PairTAGE for training")
{
    providerTable.init(numPredictors);
}

void
PairTAGE::putPCHistory(Addr startAddr, const bitset &history,
                       std::vector<FullBTBPrediction> &stagePreds)
{
    (void)history;
    assert(!stagePreds.empty());
    const ThreadID tid = stagePreds.front().tid;

    auto meta = std::make_shared<PairTAGEMeta>();
    meta->history = historyState(tid);
    meta->lookup = lookup(startAddr, meta->history);
    threadMeta[tid] = meta;

    pairTageStats.lookups++;
    if (meta->lookup.provider >= 0) {
        pairTageStats.providerTable[meta->lookup.provider]++;
    }
}

std::shared_ptr<void>
PairTAGE::getPredictionMeta(ThreadID tid)
{
    if (tid >= threadMeta.size()) {
        return nullptr;
    }
    return threadMeta[tid];
}

void
PairTAGE::specUpdatePHist(const bitset &history, FullBTBPrediction &pred,
                          const PathHistoryUpdate &update)
{
    doUpdateHist(history, update, pred.tid);
}

void
PairTAGE::recoverPHist(const bitset &history,
                       const HistoryRecoveryContext &context,
                       const PathHistoryUpdate &update)
{
    auto predMeta = std::static_pointer_cast<PairTAGEMeta>(
        context.predMetas[getComponentIdx()]);
    if (!predMeta) {
        return;
    }

    // Restore the prediction-time state, then apply the actual outcome
    auto &state = historyState(context.tid);
    const auto &saved = predMeta->history;
    for (unsigned t = 0; t < numPredictors; ++t) {
        state.indexFoldedHist[t].recover(saved.indexFoldedHist[t]);
        state.tagFoldedHist[t].recover(saved.tagFoldedHist[t]);
        state.altTagFoldedHist[t].recover(saved.altTagFoldedHist[t]);
    }
    doUpdateHist(history, update, context.tid);
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

PairTAGE::LookupResult
PairTAGE::lookup(Addr pc, const ThreadHistoryState &history) const
{
    LookupResult result;
    result.tableIndexes.resize(numPredictors);

    const unsigned bank = getBankId(pc);
    for (unsigned t = 0; t < numPredictors; ++t) {
        auto &ti = result.tableIndexes[t];
        const Addr set =
            getSetIndex(pc, t, history.indexFoldedHist[t].get());
        ti.index = getIndex(t, bank, set);
        ti.tag = getTag(pc, t, history.tagFoldedHist[t].get(),
                        history.altTagFoldedHist[t].get());

        const auto &ways = table[t][ti.index];
        for (unsigned way = 0; way < numWays; ++way) {
            if (ways[way].valid && ways[way].tag == ti.tag) {
                // A later, longer-history table overrides an earlier one
                result.provider = t;
                result.providerWay = way;
                result.entry = ways[way];
                break;
            }
        }
    }
    return result;
}

void
PairTAGE::writeEntry(unsigned t, Addr index, unsigned way,
                     const PTAGEEntry &entry)
{
    assert(t < numPredictors && index < table[t].size() && way < numWays);

    table[t][index][way] = entry;
}

PairTAGE::ThreadHistoryState &
PairTAGE::historyState(ThreadID tid)
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

const PairTAGE::ThreadHistoryState &
PairTAGE::historyState(ThreadID tid) const
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

void
PairTAGE::doUpdateHist(const bitset &history, const PathHistoryUpdate &update,
                       ThreadID tid)
{
    auto &state = historyState(tid);
    for (unsigned t = 0; t < numPredictors; ++t) {
        state.indexFoldedHist[t].update(history, update.shamt, update.taken,
                                        update.pc, update.target);
        state.tagFoldedHist[t].update(history, update.shamt, update.taken,
                                      update.pc, update.target);
        state.altTagFoldedHist[t].update(history, update.shamt, update.taken,
                                         update.pc, update.target);
    }
}

void
PairTAGE::checkFoldedHist(const bitset &history, ThreadID tid,
                          const char *when)
{
    (void)when;
    const auto &state = historyState(tid);
    for (unsigned t = 0; t < numPredictors; ++t) {
        state.indexFoldedHist[t].check(history);
        state.tagFoldedHist[t].check(history);
        state.altTagFoldedHist[t].check(history);
    }
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

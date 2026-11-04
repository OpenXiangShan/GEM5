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
    // Follows the RTL, which has direct-mapped tables.
    // TODO: Maybe support set-associative tables in the future with a
    // more complex replacement & allocation policy.
    fatal_if(numWays != 1, "PairTAGE: only direct-mapped tables (numWays "
             "= 1) are supported, got %u", numWays);
    fatal_if(!isPowerOf2(numBanks),
             "PairTAGE: numBanks (%u) must be a power of two", numBanks);

    table.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    threadHistory.resize(MaxThreads);
    threadMeta.resize(MaxThreads);
    threadAhead.resize(MaxThreads);
    threadPhase.resize(MaxThreads, StatePhase::First);
    threadPending.resize(MaxThreads);

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
               "first block phase lookups issued to PairTAGE"),
      ADD_STAT(lookupNoAnchor, statistics::units::Count::get(),
               "lookups skipped because no ahead key exists after a squash"),
      ADD_STAT(providerTable, statistics::units::Count::get(),
               "lookups provided by each table"),
      ADD_STAT(s3TrainEvents, statistics::units::Count::get(),
               "final-stage predictions delivered to PairTAGE for training"),
      ADD_STAT(trainStrengthen, statistics::units::Count::get(),
               "trainings that strengthened a provider naming the right exit"),
      ADD_STAT(trainCorrect, statistics::units::Count::get(),
               "trainings that rewrote a wrong provider in place"),
      ADD_STAT(trainAllocate, statistics::units::Count::get(),
               "trainings that allocated an entry in a longer history"),
      ADD_STAT(trainAllocateEvicted, statistics::units::Count::get(),
               "allocations made while useful entries could be evicted"),
      ADD_STAT(trainNoTableFree, statistics::units::Count::get(),
               "trainings that found no table to allocate in"),
      ADD_STAT(secondPhasePreds, statistics::units::Count::get(),
               "predictions made in the second block phase"),
      ADD_STAT(secondPhaseProvided, statistics::units::Count::get(),
               "second block phase predictions given by a valid second block"),
      ADD_STAT(trainSecondStrengthen, statistics::units::Count::get(),
               "trainings that strengthened a second block naming the right "
               "exit"),
      ADD_STAT(trainSecondReplace, statistics::units::Count::get(),
               "trainings that replaced or installed a second block"),
      ADD_STAT(trainSecondBroken, statistics::units::Count::get(),
               "pending pairs dropped because the next block did not "
               "continue them")
{
    providerTable.init(numPredictors);
}

void
PairTAGE::putPCHistory(Addr startAddr, const bitset &history,
                       std::vector<FullBTBPrediction> &stagePreds)
{
    // Lookups use the ahead key maintained by `specUpdatePHist`, so the
    // history argument is not used here.
    (void)history;

    assert(!stagePreds.empty());
    const ThreadID tid = stagePreds.front().tid;

    auto meta = std::make_shared<PairTAGEMeta>();
    meta->history = historyState(tid);
    meta->phase = threadPhase[tid];
    threadMeta[tid] = meta;

    if (meta->phase == StatePhase::Second) {
        // No lookup: predict the second block of the pending pair, as long
        // as this block starts where its first block went
        pairTageStats.secondPhasePreds++;
        const auto &pending = threadPending[tid];
        if (pending.valid && pending.nextPC == startAddr &&
            pending.entry.b2Valid) {
            pairTageStats.secondPhaseProvided++;
            fillStagePredictions(pending.entry.block2,
                                 pending.entry.modelInfo.block2BranchSize,
                                 stagePreds);
        }
        return;
    }

    const auto &ahead = threadAhead[tid];
    meta->noAnchor = !ahead.valid;
    if (!meta->noAnchor) {
        meta->lookup = lookup(ahead.pc, ahead.history);
    }

    pairTageStats.lookups++;
    if (meta->noAnchor) {
        pairTageStats.lookupNoAnchor++;
    } else if (meta->lookup.provider >= 0) {
        pairTageStats.providerTable[meta->lookup.provider]++;
        fillStagePredictions(meta->lookup.entry.block,
                             meta->lookup.entry.modelInfo.branchSize,
                             stagePreds);
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
    // A first block entering the FTQ becomes the key of the next pair,
    // and the phase moves on to its second block
    auto &phase = threadPhase[pred.tid];
    if (phase == StatePhase::First) {
        auto &ahead = threadAhead[pred.tid];
        ahead.valid = true;
        ahead.pc = pred.bbStart;
        ahead.history = historyState(pred.tid);
        phase = StatePhase::Second;
    } else {
        phase = StatePhase::First;
    }

    doUpdateHist(history, update, pred.tid);
}

void
PairTAGE::recoverPHist(const bitset &history,
                       const HistoryRecoveryContext &context,
                       const PathHistoryUpdate &update)
{
    // The block after a squash starts a new pair without a key
    threadAhead[context.tid].valid = false;
    threadPhase[context.tid] = StatePhase::First;
    threadPending[context.tid].valid = false;

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
    pairTageStats.s3TrainEvents++;

    const auto &meta = threadMeta[s3Pred.tid];
    assert(meta);

    // The final prediction information
    // TODO: Recogonize fallthrough with attibutes rather than mark branch PC to zero.
    const bool taken = s3Pred.isTaken();
    const BTBEntry exit = taken ? s3Pred.getTakenEntry() : BTBEntry();
    const Addr targetPC = s3Pred.getTarget(predictWidth);

    // The block right after a trained first block is its second block,
    // whatever phase it was predicted in, as long as it continues it.
    // Otherwise a first block phase block trains its own first block.
    auto &pending = threadPending[s3Pred.tid];
    if (pending.valid) {
        if (s3Pred.bbStart == pending.nextPC) {
            trainSecondBlock(pending, exit, targetPC);
        } else {
            pairTageStats.trainSecondBroken++;
        }
        pending.valid = false;
    } else if (meta->phase == StatePhase::First && !meta->noAnchor) {
        pending = trainFirstBlock(meta->lookup, exit, targetPC);
    }
}

PairTAGE::PendingTrainPair
PairTAGE::trainFirstBlock(const LookupResult &lookup, const BTBEntry &exit,
                          Addr targetPC)
{
    const bool taken = exit.valid;
    const Addr branchPC = exit.valid ? exit.pc : 0;
    PendingTrainPair pending;
    pending.nextPC = targetPC;

    // Hit & Pred Correct: Update counters and targetPC
    // TODO: Maybe the target PC incorrect also counts as a misprediction.
    const bool hit = lookup.provider >= 0;
    if (hit && lookup.entry.block.branchPC == branchPC) {
        PTAGEEntry entry = lookup.entry;
        entry.block.updateCounter(taken);
        // The second block starts at the first block's target, so it no
        // longer holds once that target moves
        if (entry.block.targetPC != targetPC) {
            entry.b2Valid = false;
        }
        entry.block.targetPC = targetPC;
        entry.useful = true;
        const auto &ti = lookup.tableIndexes[lookup.provider];
        writeEntry(lookup.provider, ti.index, lookup.providerWay, entry);
        pairTageStats.trainStrengthen++;
        pending.valid = true;
        pending.table = lookup.provider;
        pending.index = ti.index;
        pending.way = lookup.providerWay;
        pending.entry = entry;
        return pending;
    }

    // Miss or Mispred: Find alloc unused entry or force allocate when refusals saturate.
    // TODO: Does global refusal count cause frequent evictions of useful entries?
    const bool mayEvict = allocRefusals == mask(AllocRefusalWidth);
    int allocTable = -1;
    for (int t = lookup.provider + 1; t < (int)numPredictors; ++t) {
        if (!lookup.tableIndexes[t].useful || mayEvict) {
            allocTable = t;
            break;
        }
    }

    // Mispred but hit entry weak: rewrite the entry with correct information.
    // Mispred but hit entry not weak:
    //   - Try to find a free entry
    //   - Force alloc when refusals saturate, which may evict a useful entry
    if (hit && (lookup.entry.block.isWeak() ||
                allocTable < 0)) {
        const auto &ti = lookup.tableIndexes[lookup.provider];
        pending.valid = true;
        pending.table = lookup.provider;
        pending.index = ti.index;
        pending.way = lookup.providerWay;
        pending.entry = PTAGEEntry(ti.tag, exit, targetPC);
        writeEntry(pending.table, pending.index, pending.way, pending.entry);
        pairTageStats.trainCorrect++;
    } else if (allocTable >= 0) {
        const auto &ti = lookup.tableIndexes[allocTable];
        pending.valid = true;
        pending.table = allocTable;
        pending.index = ti.index;
        pending.way = 0;
        pending.entry = PTAGEEntry(ti.tag, exit, targetPC);
        writeEntry(pending.table, pending.index, pending.way, pending.entry);
        pairTageStats.trainAllocate++;
        if (mayEvict) {
            allocRefusals = 0;
            pairTageStats.trainAllocateEvicted++;
        }
    }

    // Count refusals until one eviction is allowed. Successes **do not** clear
    // the count: they interleave with refusals.
    // TODO: Does this strategy only work with frequent alloc failure case / blocks?
    if (allocTable < 0) {
        allocRefusals = (allocRefusals + 1) & mask(AllocRefusalWidth);
        pairTageStats.trainNoTableFree++;
    }

    return pending;
}

void
PairTAGE::trainSecondBlock(const PendingTrainPair &pending, const BTBEntry &exit,
                           Addr targetPC)
{
    const Addr branchPC = exit.valid ? exit.pc : 0;
    PTAGEEntry entry = pending.entry;

    // A second block naming the right exit is strengthened; anything else
    // is replaced by the block that actually followed the first one
    if (entry.b2Valid && entry.block2.branchPC == branchPC) {
        entry.block2.updateCounter(exit.valid);
        entry.block2.targetPC = targetPC;
        pairTageStats.trainSecondStrengthen++;
    } else {
        entry.b2Valid = true;
        entry.block2 = PTAGEBlockInfo(exit, targetPC);
        entry.modelInfo.block2BranchSize = exit.valid ? exit.size : 0;
        pairTageStats.trainSecondReplace++;
    }

    writeEntry(pending.table, pending.index, pending.way, entry);
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

        // Direct mapped: the one entry of the set is what an allocation
        // into this table would replace
        const auto &ways = table[t][ti.index];
        ti.useful = ways[0].valid && ways[0].useful;
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
PairTAGE::fillStagePredictions(const PTAGEBlockInfo &block, uint8_t branchSize,
                               std::vector<FullBTBPrediction> &stagePreds)
{
    FillStageLoop(s) {
        stagePreds[s].btbEntries.clear();
        stagePreds[s].condTakens.clear();
        stagePreds[s].indirectTargets.clear();
        stagePreds[s].returnTarget = 0;
        stagePreds[s].predTick = curTick();
    }

    // A block that falls through has no branch to predict
    if (!block.taken()) {
        return;
    }

    BTBEntry btbEntry;
    btbEntry.valid = true;
    btbEntry.pc = block.branchPC;
    btbEntry.target = block.targetPC;
    btbEntry.isCond = block.isCond;
    btbEntry.isDirect = block.isDirect;
    btbEntry.isIndirect = block.isIndirect;
    btbEntry.isCall = block.isCall;
    btbEntry.isReturn = block.isReturn;
    btbEntry.size = branchSize;
    btbEntry.ctr = block.directionCounter;
    btbEntry.source = getComponentIdx();

    FillStageLoop(s) stagePreds[s].btbEntries.push_back(btbEntry);
    if (block.isCond) {
        FillStageLoop(s) stagePreds[s].condTakens.push_back(
            {block.branchPC, true});
    } else if (block.isIndirect) {
        FillStageLoop(s) stagePreds[s].indirectTargets.push_back(
            {block.branchPC, block.targetPC});
        if (block.isReturn) {
            FillStageLoop(s) stagePreds[s].returnTarget = block.targetPC;
        }
    }
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

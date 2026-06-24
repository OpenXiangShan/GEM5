#include "cpu/pred/btb/btb_llbpx.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "sim/cur_tick.hh"

#ifndef UNIT_TEST
#include "base/trace.hh"

#endif

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

#ifndef UNIT_TEST
BTBLLBPX::LLBPXStats::LLBPXStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookup, statistics::units::Count::get(),
               "LLBP-X branch lookups"),
      ADD_STAT(contextHit, statistics::units::Count::get(),
               "LLBP-X context hits"),
      ADD_STAT(patternHit, statistics::units::Count::get(),
               "LLBP-X pattern hits"),
      ADD_STAT(providerUse, statistics::units::Count::get(),
               "LLBP-X provider predictions accepted"),
      ADD_STAT(override, statistics::units::Count::get(),
               "LLBP-X direction writes"),
      ADD_STAT(directionChange, statistics::units::Count::get(),
               "LLBP-X direction writes that differ from the base prediction"),
      ADD_STAT(allocContext, statistics::units::Count::get(),
               "LLBP-X context allocations"),
      ADD_STAT(allocPattern, statistics::units::Count::get(),
               "LLBP-X pattern allocations"),
      ADD_STAT(updatePattern, statistics::units::Count::get(),
               "LLBP-X pattern counter updates"),
      ADD_STAT(rcrRecover, statistics::units::Count::get(),
               "LLBP-X RCR recoveries"),
      ADD_STAT(providerFallback, statistics::units::Count::get(),
               "LLBP-X provider depth fallback uses"),
      ADD_STAT(providerRejectDepth, statistics::units::Count::get(),
               "LLBP-X pattern hits rejected by TAGE provider depth"),
      ADD_STAT(providerRejectTiming, statistics::units::Count::get(),
               "LLBP-X pattern hits rejected by Pattern Buffer timing"),
      ADD_STAT(patternBufferHit, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer ready hits"),
      ADD_STAT(patternBufferMiss, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer misses"),
      ADD_STAT(patternBufferNotReady, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer hits that are not ready yet"),
      ADD_STAT(patternBufferInstall, statistics::units::Count::get(),
               "LLBP-X Pattern Buffer installs")
{
}

BTBLLBPX::BTBLLBPX(const Params &p)
    : TimedBaseBTBPredictor(p),
      numThreads(p.numThreads),
      tagBits(p.tagBits),
      keyBits(p.keyBits),
      rcrEntries(p.rcrEntries),
      counterBits(p.counterBits),
      enableTiming(p.enableTiming),
      adaptCtxDepth(p.adaptCtxDepth),
      overrideOnlyOnDiff(p.overrideOnlyOnDiff),
      patternBufferLatency(p.patternBufferLatency),
      contexts(p.contextSets, p.contextWays),
      patterns(p.patternSets, p.patternWays),
      patternBufferSize(p.patternBufferSize),
      threadState(std::max<unsigned>(p.numThreads, MaxThreads)),
      llbpxStats(this)
{
    panic_if(tagBits == 0 || tagBits >= 64, "BTBLLBPX tagBits must be in [1, 63]");
    panic_if(keyBits == 0 || keyBits >= 64, "BTBLLBPX keyBits must be in [1, 63]");
    panic_if(enableTiming && patternBufferSize == 0,
             "BTBLLBPX timing mode requires a non-zero patternBufferSize");
    warn_if(adaptCtxDepth, "BTBLLBPX Phase 1 ignores adaptive context depth");
}
#endif

ThreadID
BTBLLBPX::predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const
{
    assert(!stagePreds.empty());
    return stagePreds.front().tid;
}

std::shared_ptr<void>
BTBLLBPX::getPredictionMeta(ThreadID tid)
{
    if (tid >= threadState.size()) {
        return nullptr;
    }
    return threadState[tid].meta;
}

void
BTBLLBPX::putPCHistory(Addr startAddr,
                       const boost::dynamic_bitset<> &history,
                       std::vector<FullBTBPrediction> &stagePreds)
{
    if (stagePreds.empty()) {
        return;
    }

    const ThreadID tid = predictorTid(stagePreds);
    const unsigned firstStage = std::min<unsigned>(getDelay(), stagePreds.size());
    if (tid >= threadState.size() || firstStage >= stagePreds.size()) {
        return;
    }

    auto meta = std::make_shared<LLBPXMeta>();
    meta->tid = tid;
    meta->startPC = startAddr;
    meta->asidHash = stagePreds[firstStage].asidHash;
    meta->rcrSnapshot = threadState[tid].rcr;

    for (unsigned stage = firstStage; stage < stagePreds.size(); ++stage) {
        auto &stagePred = stagePreds[stage];
        for (const auto &entry : stagePred.btbEntries) {
            if (!entry.valid || !entry.isCond || entry.alwaysTaken) {
                continue;
            }

            const auto branchPC = entry.pc;
            auto it = CondTakens_find(stagePred.condTakens, branchPC);
            bool basePred = entry.ctr >= 0;
            if (it != stagePred.condTakens.end()) {
                basePred = it->second;
            }

            int baseProviderHistIdx = -2;
            auto tageInfoIt = stagePred.tageInfoForMgscs.find(branchPC);
            if (tageInfoIt != stagePred.tageInfoForMgscs.end()) {
                baseProviderHistIdx = tageInfoIt->second.tage_provider_table;
            }

            auto branchMeta = lookup(tid, startAddr, entry, basePred,
                                     history, stagePred.asidHash,
                                     baseProviderHistIdx);
            if (!branchMeta.patternHit) {
                meta->branches[entry.pc] = branchMeta;
                continue;
            }

            if (!branchMeta.providerDepthEligible) {
#ifndef UNIT_TEST
                llbpxStats.providerRejectDepth++;
#endif
                meta->branches[entry.pc] = branchMeta;
                DPRINTF(LLBPX,
                        "reject by depth start %#lx branch %#lx llbpx depth %d tage depth %d\n",
                        startAddr, entry.pc, branchMeta.hitHistIdx,
                        branchMeta.baseProviderHistIdx);
                continue;
            }

            if (!branchMeta.providerTimingReady) {
#ifndef UNIT_TEST
                llbpxStats.providerRejectTiming++;
#endif
                meta->branches[entry.pc] = branchMeta;
                DPRINTF(LLBPX,
                        "reject by timing start %#lx branch %#lx key %#lx tag %#lx\n",
                        startAddr, entry.pc, branchMeta.key,
                        branchMeta.patternTag);
                continue;
            }

            branchMeta.providerUsed = true;
            branchMeta.directionChanged = branchMeta.llbpxPred != basePred;
#ifndef UNIT_TEST
            llbpxStats.providerUse++;
            if (branchMeta.directionChanged) {
                llbpxStats.directionChange++;
            }
#endif

            if (overrideOnlyOnDiff && !branchMeta.directionChanged) {
                meta->branches[entry.pc] = branchMeta;
                continue;
            }

            if (it != stagePred.condTakens.end()) {
                it->second = branchMeta.llbpxPred;
            } else {
                stagePred.condTakens.emplace_back(entry.pc, branchMeta.llbpxPred);
            }
            branchMeta.overridden = true;
            meta->branches[entry.pc] = branchMeta;
#ifndef UNIT_TEST
            llbpxStats.override++;
#endif
            DPRINTF(LLBPX,
                    "override start %#lx branch %#lx base %d llbpx %d diff %d stage %u\n",
                    startAddr, entry.pc, basePred, branchMeta.llbpxPred,
                    branchMeta.directionChanged, stage);
        }
    }

    threadState[tid].meta = meta;
}

BTBLLBPX::BranchMeta
BTBLLBPX::lookup(ThreadID tid, Addr startPC, const BTBEntry &entry,
                 bool basePred, const boost::dynamic_bitset<> &history,
                 uint8_t asidHash, int baseProviderHistIdx)
{
#ifndef UNIT_TEST
    llbpxStats.lookup++;
#endif

    BranchMeta meta;
    meta.startPC = startPC;
    meta.branchPC = entry.pc;
    meta.tid = tid;
    meta.asidHash = asidHash;
    meta.basePred = basePred;
    meta.llbpxPred = basePred;
    meta.baseProviderHistIdx = baseProviderHistIdx;
    meta.providerInfoMissing = baseProviderHistIdx < -1;
    meta.hitHistIdx = -1;

    meta.cid = contextKey(tid, startPC, entry.pc, history, asidHash);
    meta.contextTag = tagFromKey(meta.cid ^ (entry.pc << 1));
    auto *ctx = contexts.find(meta.cid, meta.contextTag);
    if (!ctx) {
        DPRINTF(LLBPX, "context miss start %#lx branch %#lx cid %#lx\n",
                startPC, entry.pc, meta.cid);
        return meta;
    }

    meta.contextHit = true;
#ifndef UNIT_TEST
    llbpxStats.contextHit++;
#endif
    meta.key = ctx->patternKey ? ctx->patternKey : patternKey(meta.cid, entry.pc, asidHash);
    meta.bcid = meta.key;
    meta.patternTag = tagFromKey(meta.key ^ entry.pc ^ asidHash);
    auto *pattern = patterns.find(meta.key, meta.patternTag);
    if (!pattern) {
        DPRINTF(LLBPX, "pattern miss start %#lx branch %#lx key %#lx\n",
                startPC, entry.pc, meta.key);
        return meta;
    }

    meta.patternHit = true;
    meta.hitHistIdx = pattern->providerDepth;
    const int compareProviderDepth = meta.providerInfoMissing ?
                                     -1 : meta.baseProviderHistIdx;
    meta.providerDepthEligible = compareProviderDepth < 0 ||
                                 meta.hitHistIdx >= compareProviderDepth;
    meta.llbpxPred = pattern->taken();
#ifndef UNIT_TEST
    llbpxStats.patternHit++;
    if (meta.providerInfoMissing) {
        llbpxStats.providerFallback++;
    }
#endif
    if (enableTiming) {
        auto *bufferEntry = findPatternBuffer(tid, meta.key, meta.patternTag);
        if (!bufferEntry) {
#ifndef UNIT_TEST
            llbpxStats.patternBufferMiss++;
#endif
            rememberPattern(tid, meta.key, meta.patternTag, false,
                            curTick() + patternBufferLatency);
            meta.providerTimingReady = false;
        } else if (bufferEntry->readyTick > curTick()) {
#ifndef UNIT_TEST
            llbpxStats.patternBufferNotReady++;
#endif
            meta.providerTimingReady = false;
        } else {
#ifndef UNIT_TEST
            llbpxStats.patternBufferHit++;
#endif
            meta.providerTimingReady = true;
        }
    } else {
        meta.providerTimingReady = true;
    }
    DPRINTF(LLBPX,
            "pattern hit start %#lx branch %#lx key %#lx ctr %d pred %d "
            "llbpx depth %d tage depth %d depth-ok %d timing-ok %d\n",
            startPC, entry.pc, meta.key, pattern->counter, meta.llbpxPred,
            meta.hitHistIdx, meta.baseProviderHistIdx,
            meta.providerDepthEligible, meta.providerTimingReady);
    return meta;
}

void
BTBLLBPX::specUpdateGHist(const boost::dynamic_bitset<> &history,
                          FullBTBPrediction &pred,
                          const DirectionHistoryUpdate &update)
{
    if (pred.tid >= threadState.size()) {
        return;
    }

    auto meta = threadState[pred.tid].meta;
    if (!meta) {
        return;
    }

    const auto takenEntry = pred.getTakenEntry();
    if (!takenEntry.valid) {
        meta->rcrUpdated = true;
        return;
    }

    pushRCR(pred.tid, takenEntry, true);
    meta->rcrUpdated = true;
}

void
BTBLLBPX::recoverHist(const boost::dynamic_bitset<> &history,
                      const FetchTarget &entry, int shamt,
                      bool cond_taken)
{
    auto meta = std::static_pointer_cast<LLBPXMeta>(entry.predMetas[getComponentIdx()]);
    if (!meta || entry.tid >= threadState.size()) {
        return;
    }
    restoreRCR(entry.tid, *meta);
    if (entry.exeTaken) {
        pushRCR(entry.tid, entry.exeBranchInfo, true);
        DPRINTF(LLBPX,
                "recover RCR with actual taken branch pc %#lx target %#lx\n",
                entry.exeBranchInfo.pc, entry.exeBranchInfo.target);
    }
#ifndef UNIT_TEST
    llbpxStats.rcrRecover++;
#endif
}

void
BTBLLBPX::recoverPHist(const boost::dynamic_bitset<> &history,
                       const FetchTarget &entry,
                       const PathHistoryUpdate &update)
{
}

void
BTBLLBPX::update(const FetchTarget &entry)
{
    auto meta = std::static_pointer_cast<LLBPXMeta>(entry.predMetas[getComponentIdx()]);
    if (!meta) {
        DPRINTF(LLBPX, "update skip: no meta for start %#lx\n", entry.startPC);
        return;
    }

    for (const auto &btbEntry : entry.updateBTBEntries) {
        if (!btbEntry.valid || !btbEntry.isCond) {
            continue;
        }
        auto it = meta->branches.find(btbEntry.pc);
        if (it == meta->branches.end()) {
            continue;
        }

        const bool actualTaken = entry.exeTaken && entry.exeBranchInfo == btbEntry;
        auto &branchMeta = it->second;

        if (branchMeta.providerUsed) {
            updatePattern(branchMeta, actualTaken);
        }

        const bool baseWrong = branchMeta.basePred != actualTaken;
        const bool llbpxWrong = branchMeta.providerUsed &&
                                branchMeta.llbpxPred != actualTaken;
        if (baseWrong || llbpxWrong || !branchMeta.contextHit || !branchMeta.patternHit) {
            allocateFor(branchMeta, actualTaken);
        }
    }
}

void
BTBLLBPX::allocateFor(const BranchMeta &meta, bool actualTaken)
{
    auto *ctx = contexts.find(meta.cid, meta.contextTag);
    if (!ctx) {
        ctx = &contexts.allocate(meta.cid, meta.contextTag);
        ctx->patternKey = patternKey(meta.cid, meta.branchPC, meta.asidHash);
#ifndef UNIT_TEST
        llbpxStats.allocContext++;
#endif
        DPRINTF(LLBPX, "alloc context branch %#lx cid %#lx tag %#lx\n",
                meta.branchPC, meta.cid, meta.contextTag);
    }

    const Addr key = ctx->patternKey ? ctx->patternKey : meta.key;
    const Addr ptag = tagFromKey(key ^ meta.branchPC ^ meta.asidHash);
    auto *pattern = patterns.find(key, ptag);
    const int allocDepth = meta.baseProviderHistIdx >= 0 ?
                           meta.baseProviderHistIdx + 1 : 0;
    if (!pattern) {
        pattern = &patterns.allocate(key, ptag);
        pattern->counter = actualTaken ? 0 : -1;
        pattern->providerDepth = allocDepth;
#ifndef UNIT_TEST
        llbpxStats.allocPattern++;
#endif
        DPRINTF(LLBPX, "alloc pattern branch %#lx key %#lx tag %#lx ctr %d depth %d\n",
                meta.branchPC, key, ptag, pattern->counter,
                pattern->providerDepth);
    } else if (allocDepth > pattern->providerDepth) {
        pattern->providerDepth = allocDepth;
    }
    rememberPattern(meta.tid, key, ptag, true, curTick());
}

void
BTBLLBPX::updatePattern(const BranchMeta &meta, bool actualTaken)
{
    auto *pattern = patterns.find(meta.key, meta.patternTag);
    if (!pattern) {
        return;
    }
    updateCounter(actualTaken, pattern->counter);
    if (pattern->taken() == actualTaken && pattern->confidence < 15) {
        pattern->confidence++;
    } else if (pattern->confidence > 0) {
        pattern->confidence--;
    }
#ifndef UNIT_TEST
    llbpxStats.updatePattern++;
#endif
    rememberPattern(meta.tid, meta.key, meta.patternTag, true, curTick());
    DPRINTF(LLBPX, "update pattern branch %#lx key %#lx actual %d ctr %d depth %d\n",
            meta.branchPC, meta.key, actualTaken, pattern->counter,
            pattern->providerDepth);
}

BTBLLBPX::PatternBufferEntry *
BTBLLBPX::findPatternBuffer(ThreadID tid, Addr key, Addr tag)
{
    if (tid >= threadState.size()) {
        return nullptr;
    }
    auto &buffer = threadState[tid].patternBuffer;
    auto it = std::find_if(buffer.begin(), buffer.end(),
        [key, tag](const PatternBufferEntry &entry) {
            return entry.key == key && entry.tag == tag;
        });
    return it == buffer.end() ? nullptr : &(*it);
}

void
BTBLLBPX::rememberPattern(ThreadID tid, Addr key, Addr tag, bool dirty,
                          Tick readyTick)
{
    if (tid >= threadState.size() || patternBufferSize == 0) {
        return;
    }
    auto &buffer = threadState[tid].patternBuffer;
    auto *entry = findPatternBuffer(tid, key, tag);
    if (entry) {
        entry->dirty = entry->dirty || dirty;
        entry->readyTick = readyTick;
        return;
    }
    if (buffer.size() >= patternBufferSize) {
        buffer.pop_front();
    }
    buffer.push_back(PatternBufferEntry{key, tag, dirty, readyTick});
#ifndef UNIT_TEST
    llbpxStats.patternBufferInstall++;
#endif
}

void
BTBLLBPX::restoreRCR(ThreadID tid, const LLBPXMeta &meta)
{
    threadState[tid].rcr = meta.rcrSnapshot;
}

void
BTBLLBPX::pushRCR(ThreadID tid, const BranchInfo &branch, bool taken)
{
    if (tid >= threadState.size() || rcrEntries == 0) {
        return;
    }
    auto &rcr = threadState[tid].rcr;
    if (rcr.size() >= rcrEntries) {
        rcr.pop_front();
    }
    rcr.push_back(RCRRecord{branch.pc, branch.target, taken, branch.isCond});
}

Addr
BTBLLBPX::hashBits(const boost::dynamic_bitset<> &history, unsigned bits) const
{
    Addr value = 0;
    const unsigned limit = std::min<unsigned>(history.size(), bits);
    for (unsigned i = 0; i < limit; ++i) {
        if (history[i]) {
            value ^= mix(static_cast<Addr>(i + 1));
        }
    }
    return value;
}

Addr
BTBLLBPX::mix(Addr value) const
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

Addr
BTBLLBPX::contextKey(ThreadID tid, Addr startPC, Addr branchPC,
                     const boost::dynamic_bitset<> &history,
                     uint8_t asidHash) const
{
    Addr key = mix(startPC >> 1) ^ mix(branchPC << 1) ^
               mix(static_cast<Addr>(asidHash) << 48) ^ hashBits(history, keyBits);

    const auto &rcr = threadState[tid].rcr;
    unsigned idx = 0;
    for (const auto &record : rcr) {
        key ^= mix(record.pc + (record.target << 1) + (record.taken ? 0x9e37 : 0) + idx);
        idx++;
    }
    return key & mask(keyBits);
}

Addr
BTBLLBPX::patternKey(Addr contextKey, Addr branchPC, uint8_t asidHash) const
{
    return mix(contextKey ^ (branchPC >> 1) ^ (static_cast<Addr>(asidHash) << 7)) &
           mask(keyBits);
}

Addr
BTBLLBPX::tagFromKey(Addr key) const
{
    return mix(key) & mask(tagBits);
}

void
BTBLLBPX::updateCounter(bool taken, short &counter) const
{
    const short max = (1 << (counterBits - 1)) - 1;
    const short min = -(1 << (counterBits - 1));
    if (taken && counter < max) {
        counter++;
    } else if (!taken && counter > min) {
        counter--;
    }
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

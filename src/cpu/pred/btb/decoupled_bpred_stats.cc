#include <array>

#include "base/output.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/trace/TraceBranchTruth.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "debug/BTB.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace
{

constexpr int NumS1SourceBuckets = 4;
constexpr int NumOverrideReasonBuckets = 4;

constexpr const char *S1SourceLabels[NumS1SourceBuckets] = {
    "fallthrough",
    "ubtb",
    "abtb",
    "pairtage"
};

constexpr const char *OverrideReasonLabels[NumOverrideReasonBuckets] = {
    "no_override",
    "fall_thru",
    "control_addr",
    "target"
};

int
overrideReasonBucket(OverrideReason reason)
{
    switch (reason) {
      case OverrideReason::NO_OVERRIDE:
        return 0;
      case OverrideReason::FALL_THRU:
        return 1;
      case OverrideReason::CONTROL_ADDR:
        return 2;
      case OverrideReason::TARGET:
        return 3;
    }

    return 0;
}

} // namespace

void
DecoupledBPUWithBTB::initDB()
{

    bpdb.init_db();
    enableBranchTrace = checkGivenSwitch(bpDBSwitches, std::string("basic"));
    if (enableBranchTrace) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("fsqId", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("controlType", UINT64),
            std::make_pair("taken", UINT64),
            std::make_pair("mispred", UINT64),
            std::make_pair("fallThruPC", UINT64),
            std::make_pair("source", UINT64),
            std::make_pair("target", UINT64)
        };
        bptrace = bpdb.addAndGetTrace("BPTRACE", fields_vec);
        bptrace->init_table();
        removeGivenSwitch(bpDBSwitches, std::string("basic"));
        someDBenabled = true;
    }

    enablePredFSQTrace = checkGivenSwitch(bpDBSwitches, std::string("predfsq"));
    if (enablePredFSQTrace) {
        // Initialize prediction trace manager for recording predictions
        std::vector<std::pair<std::string, DataType>> pred_fields_vec = {
            std::make_pair("fsqId", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("predEndPC", UINT64),
            std::make_pair("controlPC", UINT64),
            std::make_pair("target", UINT64),
            std::make_pair("predSource", UINT64),
            std::make_pair("btbHit", UINT64)
        };
        predTraceManager = bpdb.addAndGetTrace("PREDTRACE", pred_fields_vec);
        predTraceManager->init_table();
        removeGivenSwitch(bpDBSwitches, std::string("predfsq"));
        someDBenabled = true;
    }
}

void
DecoupledBPUWithBTB::dumpStats()
{
    profiler.dump();
    if (someDBenabled) {
        bpdb.save_db(simout.resolve("bp.db").c_str());
    }
}

DecoupledBPUWithBTB::BpTrace::BpTrace(uint64_t fsqId, FetchTarget &target, const DynInstPtr &inst, bool mispred)
{
    _tick = curTick();
    Addr pc = inst->pcState().instAddr();
    const auto &rv_pc = inst->pcState().as<RiscvISA::PCState>();
    Addr targetpc = rv_pc.npc();
    Addr fallThru = rv_pc.getFallThruPC();
    BranchInfo info(pc, targetpc, inst->staticInst, fallThru-pc);
    bool taken = inst->branching();
    if (inst->hasTraceBranchInfo()) {
        // Anchored rule (TraceBranchTruth.hh; guards the b5fefebcef
        // port-gap): trace metadata wins over static-decode truth.
        const auto facts = o3::TraceBranchTruth::factsFrom(*inst);
        targetpc = o3::TraceBranchTruth::effectiveTarget(targetpc, facts);
        o3::TraceBranchTruth::applyTraceClassification(info, facts);
        taken = o3::TraceBranchTruth::effectiveTaken(taken, facts);
    }
    set(fsqId, target.startPC, pc, info.getType(), taken, mispred,
        fallThru, target.predSource, targetpc);
    // for (auto it = _uint64_data.begin(); it != _uint64_data.end(); it++) {
    //     printf("%s: %ld\n", it->first.c_str(), it->second);
    // }
}

namespace {

constexpr std::array<const char*, DecoupledBPUWithBTB::NumBranchClasses>
    BranchClassLabels = {
        "cond_branch",
        "direct_call",
        "indirect_call",
        "return",
        "direct_jump",
        "indirect_jump",
        "unknown"
    };

template <typename InstPtr>
DecoupledBPUWithBTB::BranchClass
classifyBranchImpl(const InstPtr &inst)
{
    using BranchClass = DecoupledBPUWithBTB::BranchClass;

    if (!inst || !inst->isControl()) {
        return BranchClass::Unknown;
    }

    if (inst->isReturn()) {
        return BranchClass::Return;
    }

    if (inst->isCall()) {
        return inst->isIndirectCtrl() ? BranchClass::IndirectCall
                                      : BranchClass::DirectCall;
    }

    if (inst->isCondCtrl()) {
        return BranchClass::CondBranch;
    }

    if (inst->isIndirectCtrl()) {
        return BranchClass::IndirectJump;
    }

    if (inst->isDirectCtrl() || inst->isUncondCtrl()) {
        return BranchClass::DirectJump;
    }

    return BranchClass::Unknown;
}

} // anonymous namespace

DecoupledBPUWithBTB::DBPBTBStats::DBPBTBStats(
    statistics::Group* parent, unsigned numStages, unsigned ftqSize,
    unsigned predictWidth, unsigned maxInstsNum, unsigned numThreads):
    statistics::Group(parent),
    ADD_STAT(condNum, statistics::units::Count::get(), "the number of cond branches"),
    ADD_STAT(uncondNum, statistics::units::Count::get(), "the number of uncond branches"),
    ADD_STAT(returnNum, statistics::units::Count::get(), "the number of return branches"),
    ADD_STAT(otherNum, statistics::units::Count::get(), "the number of other branches"),
    ADD_STAT(condMiss, statistics::units::Count::get(), "the number of cond branch misses"),
    ADD_STAT(uncondMiss, statistics::units::Count::get(), "the number of uncond branch misses"),
    ADD_STAT(returnMiss, statistics::units::Count::get(), "the number of return branch misses"),
    ADD_STAT(otherMiss, statistics::units::Count::get(), "the number of other branch misses"),
    ADD_STAT(branchClassCounts, statistics::units::Count::get(), "branch counts by fine-grained class"),
    ADD_STAT(branchClassMisses, statistics::units::Count::get(), "branch mispredictions by fine-grained class"),
    ADD_STAT(branchClassCountsTotal, statistics::units::Count::get(), "total number of classified branches"),
    ADD_STAT(controlSquashByClass, statistics::units::Count::get(), "commit/resolve-path squashes by branch class"),
    ADD_STAT(staticBranchNum, statistics::units::Count::get(), "the number of all (different) static branches"),
    ADD_STAT(staticBranchNumEverTaken, statistics::units::Count::get(), "the number of all (different) static branches that are once taken"),
    ADD_STAT(predsOfEachStage, statistics::units::Count::get(), "the number of preds of each stage that account for final pred"),
    ADD_STAT(overrideBubbleNum,  statistics::units::Count::get(), "the number of override bubbles"),
    ADD_STAT(overrideCount, statistics::units::Count::get(), "the number of overrides"),
    ADD_STAT(commitPredsFromEachStage, statistics::units::Count::get(),
    "the number of preds of each stage that account for a committed target"),
    ADD_STAT(commitOverrideBubbleNum, statistics::units::Count::get(),
    "the number of override bubbles, on the commit path"),
    ADD_STAT(commitOverrideCount, statistics::units::Count::get(), "the number of overrides, on the commit path"),
    ADD_STAT(overrideFallThruMismatch, statistics::units::Count::get(),
    "Number of overrides due to validity mismatches, on commit path"),
    ADD_STAT(overrideControlAddrMismatch, statistics::units::Count::get(),
    "Number of overrides due to control address mismatches, on commit path"),
    ADD_STAT(overrideTargetMismatch, statistics::units::Count::get(),
    "Number of overrides due to target mismatches, on commit path"),
    ADD_STAT(ftqEntryDist, statistics::units::Count::get(),
             "the distribution of number of entries in ftq"),
    ADD_STAT(ftqEntryEnqueued, statistics::units::Count::get(),
             "the number of ftq entries enqueued"),
    ADD_STAT(secondBlockEnqueued, statistics::units::Count::get(),
             "second blocks enqueued in the same cycle as their first block"),
    ADD_STAT(secondBlockFtqFull, statistics::units::Count::get(),
             "second blocks dropped because the FTQ had no room left"),
    ADD_STAT(ftqEntryCommitted, statistics::units::Count::get(),
             "the number of ftq entries committed"),
    ADD_STAT(controlSquashFromDecode, statistics::units::Count::get(),
             "the number of control squashes in bpu from decode"),
    ADD_STAT(controlSquashFromCommit, statistics::units::Count::get(),
             "the number of control squashes in bpu from commit"),
    ADD_STAT(controlSquashFromPredecode, statistics::units::Count::get(),
    "the number of control squashes in bpu from predecode"),
    ADD_STAT(nonControlSquash, statistics::units::Count::get(), "the number of non-control squashes in bpu"),
    ADD_STAT(trapSquash, statistics::units::Count::get(), "the number of trap squashes in bpu"),
    ADD_STAT(ftqNotValid, statistics::units::Count::get(), "fetch needs ftq req but ftq not valid"),
    ADD_STAT(ftqFullCannotEnq, statistics::units::Count::get(), "bpu has req but ftq full cannot enqueue"),
    ADD_STAT(commitFtqInstBytes, statistics::units::Byte::get(),
             "size of commit ftq entries"),
    ADD_STAT(commitFtqEntryHasInsts, statistics::units::Count::get(),
             "number of insts that commit ftq entries have"),
    ADD_STAT(commitFtqEntryFetchedInsts, statistics::units::Count::get(),
             "number of insts that commit ftq entries fetched"),
    ADD_STAT(commitFtqEntryOnlyHasOneJump, statistics::units::Count::get(),
             "number of ftq entries with only one instruction (jump)"),
    ADD_STAT(btbHit, statistics::units::Count::get(), "btb hits (in predict block)"),
    ADD_STAT(btbMiss, statistics::units::Count::get(), "btb misses (in predict block)"),
    ADD_STAT(btbEntriesWithDifferentStart, statistics::units::Count::get(), "number of btb entries with different start PC"),
    ADD_STAT(btbEntriesWithOnlyOneJump, statistics::units::Count::get(), "number of btb entries with different start PC starting with a jump"),
    ADD_STAT(predFalseHit, statistics::units::Count::get(), "false hit detected at pred"),
    ADD_STAT(commitFalseHit, statistics::units::Count::get(), "false hit detected at commit"),
    ADD_STAT(predictionBlockedForUpdate, statistics::units::Count::get(), "prediction blocked for update priority"),
    ADD_STAT(scheduleIneligibleThreadSkips, statistics::units::Count::get(),
    "round-robin BPU scheduler skipped an active thread that could not start a new prediction"),
    ADD_STAT(scheduleNoEligibleThread, statistics::units::Count::get(),
    "round-robin BPU scheduler found no thread eligible to start a new prediction"),
    ADD_STAT(redirectPendingPredictionSkips, statistics::units::Count::get(),
    "round-robin BPU scheduler skipped prediction because a redirect was pending"),
    ADD_STAT(predictionsStartedPerCycle, statistics::units::Count::get(),
    "number of distinct SMT thread predictions started in one cycle"),
    ADD_STAT(predictionsStartedByThread, statistics::units::Count::get(),
    "number of predictions started for each SMT thread"),
    ADD_STAT(s1PredWrongFallthrough, statistics::units::Count::get(), "S1pred wrong full throughs"),
    ADD_STAT(s1PredWrongUbtb, statistics::units::Count::get(),"S1pred wrong using ubtb "),
    ADD_STAT(s1PredWrongAbtb, statistics::units::Count::get(), "S1pred wrong using abtb "),
    ADD_STAT(s1PredWrongPairtage, statistics::units::Count::get(), "S1pred wrong using pairtage "),
    ADD_STAT(s1PredWrongBySourceAndReason, statistics::units::Count::get(),
             "committed S1 mispredictions bucketed by S1 source and override reason"),
    ADD_STAT(s3PredWrongMbtb, statistics::units::Count::get(), "S3pred wrong blame mbtb "),
    ADD_STAT(s3PredWrongTage, statistics::units::Count::get(), "S3pred wrong blame tage "),
    ADD_STAT(s3PredWrongIttage, statistics::units::Count::get(), "S3pred wrong blame ittage "),
    ADD_STAT(s3PredWrongRas, statistics::units::Count::get(), "S3pred wrong blame ras ")

{
    predsOfEachStage.init(numStages);
    commitPredsFromEachStage.init(numStages+1);
    commitOverrideBubbleNum = commitPredsFromEachStage[1] + 2 * commitPredsFromEachStage[2] ;
    commitOverrideCount = commitPredsFromEachStage[1] + commitPredsFromEachStage[2];
    ftqEntryDist.init(0, ftqSize, 20).flags(statistics::total);
    commitFtqInstBytes.init(0, predictWidth, 1);
    commitFtqEntryHasInsts.init(0, maxInstsNum >> 1, 1);
    commitFtqEntryFetchedInsts.init(0, maxInstsNum >> 1, 1);
    s1PredWrongBySourceAndReason.init(NumS1SourceBuckets, NumOverrideReasonBuckets);
    branchClassCounts.init(NumBranchClasses);
    predictionsStartedPerCycle.init(0, numThreads, 1);
    predictionsStartedByThread.init(numThreads);
    branchClassMisses.init(NumBranchClasses);
    controlSquashByClass.init(NumBranchClasses);
    for (int i = 0; i < NumS1SourceBuckets; ++i) {
        s1PredWrongBySourceAndReason.subname(i, S1SourceLabels[i]);
    }
    for (int i = 0; i < NumOverrideReasonBuckets; ++i) {
        s1PredWrongBySourceAndReason.ysubname(i, OverrideReasonLabels[i]);
    }
    for (size_t i = 0; i < NumBranchClasses; ++i) {
        branchClassCounts.subname(i, BranchClassLabels[i]);
        branchClassMisses.subname(i, BranchClassLabels[i]);
        controlSquashByClass.subname(i, BranchClassLabels[i]);
    }
}

void DecoupledBPUWithBTB::overrideStats(OverrideReason overrideReason)
{

        // Track specific override reasons for statistics
        switch (overrideReason) {
            case OverrideReason::FALL_THRU:
                dbpBtbStats.overrideFallThruMismatch++;
                break;
            case OverrideReason::CONTROL_ADDR:
                dbpBtbStats.overrideControlAddrMismatch++;
                break;
            case OverrideReason::TARGET:
                dbpBtbStats.overrideTargetMismatch++;
                break;
            default:
                break;
        }
}

DecoupledBPUWithBTB::BranchClass
DecoupledBPUWithBTB::classifyBranch(const DynInstPtr &inst) const
{
    return classifyBranchImpl(inst);
}

DecoupledBPUWithBTB::BranchClass
DecoupledBPUWithBTB::classifyBranch(const StaticInstPtr &inst) const
{
    return classifyBranchImpl(inst);
}

const char *
DecoupledBPUWithBTB::branchClassName(BranchClass cls)
{
    auto idx = static_cast<size_t>(cls);
    if (idx < BranchClassLabels.size()) {
        return BranchClassLabels[idx];
    }
    return "invalid";
}

void
DecoupledBPUWithBTB::addBranchClassStat(BranchClass cls, bool mispred)
{
    auto idx = static_cast<size_t>(cls);
    if (idx >= NumBranchClasses) {
        DPRINTF(DBPBTBStats, "Skip invalid branch class stats update %d\n",
                static_cast<int>(cls));
        return;
    }

    dbpBtbStats.branchClassCounts[idx]++;
    if (mispred) {
        dbpBtbStats.branchClassMisses[idx]++;
        dbpBtbStats.branchClassCountsTotal++;
    }

    DPRINTF(DBPBTBStats, "Branch classified as %s, mispred=%d\n",
            branchClassName(cls), mispred);
}

void
DecoupledBPUWithBTB::addControlSquashCommitStat(BranchClass cls)
{
    auto idx = static_cast<size_t>(cls);
    if (idx >= NumBranchClasses) {
        DPRINTF(DBPBTBStats,
                "Skip invalid commit squash class stats update %d\n",
                static_cast<int>(cls));
        return;
    }

    dbpBtbStats.controlSquashByClass[idx]++;
    DPRINTF(DBPBTBStats, "Commit squash classified as %s\n",
            branchClassName(cls));
}

void
DecoupledBPUWithBTB::updateStatistics(
    const FetchTarget &target, const PreparedUpdate &update)
{
    if (profiler.recordFetchBlock(target, update)) {
        dbpBtbStats.btbEntriesWithDifferentStart++;
    }

    // --- BTB Statistics ---
    if (target.isHit) {
        // Count BTB hits
        dbpBtbStats.btbHit++;
    } else {
        // Count BTB misses for taken branches
        if (update.outcome.taken) {
            dbpBtbStats.btbMiss++;
            DPRINTF(BTB, "BTB miss detected when update, target start %#lx, predTick %lu, printing branch info:\n",
                    target.startPC, target.predTick);
            const auto &slot = update.outcome.branch;
            DPRINTF(BTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
        }

        // Count false hits
        if (target.falseHit) {
            dbpBtbStats.commitFalseHit++;
        }
    }

    // Track which predictor stage was used
    dbpBtbStats.commitPredsFromEachStage[target.predSource]++;
    overrideStats(target.overrideReason);

    // --- Instruction Statistics ---
    // Track committed fetch block size and instruction counts
    dbpBtbStats.commitFtqInstBytes.sample(target.commitInstBytes);
    dbpBtbStats.commitFtqEntryHasInsts.sample(target.commitInstNum, 1);
    if (target.commitInstNum >= 0 && target.commitInstNum <= maxInstsNum) {
        if (target.commitInstNum == 1 &&
            update.outcome.valid && update.outcome.branch.isUncond()) {
            dbpBtbStats.commitFtqEntryOnlyHasOneJump++;
        }
    }

    // Track fetched instruction counts.
    dbpBtbStats.commitFtqEntryFetchedInsts.sample(target.fetchInstNum, 1);

}

void
DecoupledBPUWithBTB::commitPredWrongSource(
    const FetchTarget &entry, const BranchInfo &actualBranch,
    bool actuallyTaken)
{
    int ubtbid = ubtb->getComponentIdx();
    int abtbid = abtb->getComponentIdx();
    int pairtageid = pairtage->getComponentIdx();
    int mbtbid = mbtb->getComponentIdx();
    int tageid = tage->getComponentIdx();
    int ittageid = ittage->getComponentIdx();
    int rasid = ras->getComponentIdx();

    int s1PredSource = entry.s1Source;
    int s3PredSource = entry.s3Source;

    bool onlyDirectionWrong = actuallyTaken != entry.predTaken;
    int s1SourceBucket = 0;

    assert(s1PredSource < mbtbid);
    if (s1PredSource == ubtbid) {
        dbpBtbStats.s1PredWrongUbtb++;
        s1SourceBucket = 1;
    } else if (s1PredSource == abtbid) {
        dbpBtbStats.s1PredWrongAbtb++;
        s1SourceBucket = 2;
    } else if (s1PredSource == pairtageid) {
        dbpBtbStats.s1PredWrongPairtage++;
        s1SourceBucket = 3;
    } else {
        dbpBtbStats.s1PredWrongFallthrough++;
    }

    dbpBtbStats.s1PredWrongBySourceAndReason
        [s1SourceBucket][overrideReasonBucket(entry.overrideReason)]++;

    if (s3PredSource == rasid) {
        if (actualBranch.isCond) {
            dbpBtbStats.s3PredWrongTage++;
        } else if (actualBranch.isReturn) {
            dbpBtbStats.s3PredWrongRas++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    } else if (s3PredSource == ittageid) {
        if (actualBranch.isIndirect) {
            dbpBtbStats.s3PredWrongIttage++;
        } else if (actualBranch.isCond) {
            dbpBtbStats.s3PredWrongTage++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    } else if (s3PredSource == tageid) {
        if (actualBranch.isCond) {
            if (onlyDirectionWrong) {
                dbpBtbStats.s3PredWrongTage++;
            } else {
                dbpBtbStats.s3PredWrongMbtb++;
            }
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    }else if (s3PredSource == mbtbid) {
        if (actualBranch.isCond) {
            if (onlyDirectionWrong) {
                dbpBtbStats.s3PredWrongTage++;
            } else {
                dbpBtbStats.s3PredWrongMbtb++;
            }
        } else if (actualBranch.isIndirect) {
            dbpBtbStats.s3PredWrongIttage++;
        } else {
            dbpBtbStats.s3PredWrongMbtb++;
        }
    }else if (s3PredSource == -1) {
        dbpBtbStats.s3PredWrongMbtb++;
    }
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

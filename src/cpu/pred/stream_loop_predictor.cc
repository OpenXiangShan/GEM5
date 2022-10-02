#include "cpu/pred/stream_loop_predictor.hh"

namespace gem5
{
namespace branch_prediction
{

StreamLoopPredictor::StreamLoopPredictor(const Params &params)
                    : SimObject(params)
{
        
}

void
StreamLoopPredictor::updateTripCount(unsigned fsqId, Addr branchAddr)
{
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry != loopTable.end()) {
        LoopEntry temp = entry->second;
        if (entry->second.tripCount == entry->second.detectedCount) {
            entry->second.tripCount = 0;
        } else {
            entry->second.tripCount++;
        }
        DPRINTF(DecoupleBP || debugFlagOn, "fsqId: %lu, find loop entry at %#lx, update loop table entry from "
                "[%#lx, %#lx, %#lx, %d, %d] to [%#lx, %#lx, %#lx, %d, %d]\n",
                fsqId, branchAddr, temp.branch, temp.target, temp.outTarget,
                temp.tripCount, temp.detectedCount,
                entry->second.branch, entry->second.target, entry->second.outTarget, 
                entry->second.tripCount, entry->second.detectedCount);
    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "can not find corresponding loop entry at %#lx\n", branchAddr);
        assert(0);
    }
}

std::pair<bool, Addr>
StreamLoopPredictor::makeLoopPrediction(Addr branchAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry != loopTable.end()) {
        if (!entry->second.valid) {
            DPRINTF(DecoupleBP || debugFlagOn, "loop entry at [%#lx, %#lx] is invalid, skip loop prediction\n",
                    branchAddr, entry->second.target);
            return std::make_pair(false, 0);
        }

        if (entry->second.tripCount == entry->second.detectedCount) {
            DPRINTF(DecoupleBP || debugFlagOn, "predict loop: %#lx-->%#lx\n", branchAddr, entry->second.outTarget);
            return std::make_pair(true, entry->second.outTarget);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "predict loop: %#lx-->%#lx\n", branchAddr, entry->second.target);
            return std::make_pair(true, entry->second.target);
        }
        
    }
    DPRINTF(DecoupleBP || debugFlagOn, "can't find loop entry at %#lx, skip loop prediction\n", branchAddr);
    return std::make_pair(false, 0);
}

void
StreamLoopPredictor::updateEntry(Addr branchAddr, Addr targetAddr, Addr outTarget, int detectedCount, bool intraTaken) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry == loopTable.end()) {
        loopTable[branchAddr] = LoopEntry(branchAddr, targetAddr, outTarget, detectedCount, intraTaken);
        DPRINTF(DecoupleBP || debugFlagOn, "insert loop table entry: [%#lx, %#lx, %#lx, %d, %d]\n",
                branchAddr, targetAddr, outTarget, 0, detectedCount);
	} else {
        LoopEntry temp = entry->second;
        entry->second.detectedCount = detectedCount;
        entry->second.intraTaken = intraTaken;
        DPRINTF(DecoupleBP || debugFlagOn, "update loop table entry from [%#lx, %#lx, %#lx, %d, %d] "
                "to [%#lx, %#lx, %#lx, %d, %d]\n",
                temp.branch, temp.target, temp.outTarget, temp.tripCount, temp.detectedCount,
                entry->second.branch, entry->second.target, entry->second.outTarget,
                entry->second.tripCount, entry->second.detectedCount);
    }
}

void
StreamLoopPredictor::controlSquash(unsigned fsqId, FetchStream stream, Addr branchAddr, Addr targetAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry != loopTable.end()) {
        LoopEntry temp = entry->second;
        DPRINTF(DecoupleBP || debugFlagOn, "squash loop table entry: [%#lx, %#lx, %#lx, %d, %d]\n",
                temp.branch, temp.target, temp.outTarget, temp.tripCount, temp.detectedCount);
        entry->second.tripCount = 0;
        if (targetAddr == entry->second.target) {
            entry->second.valid = false;
        } else if(targetAddr == entry->second.outTarget) {
            entry->second.valid = true;
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "target address doesn't match\n");
        }

        DPRINTF(DecoupleBP || debugFlagOn, "control squash fsqId: %lu, predBranch: %#lx, predTarget %#lx, "
                "set trip count at %#lx-->%#lx from %d to %d, %s\n",
                fsqId, stream.predBranchPC, stream.predTarget,
                branchAddr, targetAddr, temp.tripCount, entry->second.tripCount,
                entry->second.valid ? "valid" : "invalid");

    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "control squash fsqId: %lu, not found in table\n",
                fsqId);
    }
}

void
StreamLoopPredictor::isIntraSquash(unsigned fsqId, FetchStream stream, Addr branchAddr) {
    for (auto &iter : loopTable) {
        if (iter.second.target < branchAddr && branchAddr < iter.second.branch) {
            defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
            debugFlagOn = true;
            DPRINTF(DecoupleBP || debugFlagOn, "squash fsq: %lu, pred: %#lx [%#lx --> %#lx], "
                    "exe: %#lx [%#lx --> %#lx] at %#lx, "
                    "in loop [%#lx, %#lx]\n",
                    fsqId, stream.streamStart, stream.predBranchPC, stream.predTarget,
                    stream.streamStart, stream.exeBranchPC, stream.exeTarget, branchAddr,
                    iter.second.target, iter.second.branch);
        }
    }
}

} // namespace branch_prediction
} // namespace gem5
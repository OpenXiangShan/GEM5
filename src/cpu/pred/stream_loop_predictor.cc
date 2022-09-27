#include "cpu/pred/stream_loop_predictor.hh"

namespace gem5
{
namespace branch_prediction
{

StreamLoopPredictor::StreamLoopPredictor(const Params &params)
                    : SimObject(params)
{
        
}

std::pair<bool, Addr>
StreamLoopPredictor::makeLoopPrediction(Addr branchAddr) {
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
        DPRINTF(DecoupleBP || debugFlagOn, "find loop entry at %#lx, update loop table entry from "
                "[%#lx, %#lx, %#lx, %d, %d] to [%#lx, %#lx, %#lx, %d, %d]\n",
                branchAddr, temp.branch, temp.target, temp.outTarget,
                temp.tripCount, temp.detectedCount,
                entry->second.branch, entry->second.target, entry->second.outTarget, 
                entry->second.tripCount, entry->second.detectedCount);

        if (entry->second.tripCount == 0) {
            DPRINTF(DecoupleBP || debugFlagOn, "predict loop: %#lx-->%#lx\n", branchAddr, entry->second.outTarget);
            return std::make_pair(true, entry->second.outTarget);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "predict loop: %#lx-->%#lx\n", branchAddr, entry->second.target);
            return std::make_pair(true, entry->second.target);
        }
        
    }
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
StreamLoopPredictor::resetTripCount(Addr branchAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry != loopTable.end()) {
        LoopEntry temp = entry->second;
        entry->second.tripCount = 0;
        DPRINTF(DecoupleBP || debugFlagOn, "control squash, reset trip count at %#lx from %d to %d\n",
                branchAddr, temp.tripCount, entry->second.tripCount);
    }
}

} // namespace branch_prediction
} // namespace gem5
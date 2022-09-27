#include "cpu/pred/loop_detector.hh"

namespace gem5
{
namespace branch_prediction
{

LoopDetector::LoopDetector(const Params &params)
            : SimObject(params), maxLoopQueueSize(params.maxLoopQueueSize)
{
        
}

void
LoopDetector::update(Addr branchAddr, Addr targetAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
        
    bool taken_backward = targetAddr < branchAddr;

    auto entry = loopTable.find(branchAddr);

    if (taken_backward) {
        if (entry == loopTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop detector from NULL to [%#lx, %#lx, %#lx, %d, %d]\n",
                    branchAddr, 0, 0, 1, 0);
            loopTable[branchAddr] = LoopEntry(branchAddr, targetAddr);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop detector from "
                    "[%#lx, %#lx, %#lx, %d, %d] to [%#lx, %#lx, %#lx, %d, %d]\n",
                    branchAddr, entry->second.target, entry->second.outTarget, entry->second.specCount, entry->second.tripCount,
                    branchAddr, entry->second.target, entry->second.outTarget, entry->second.specCount + 1, entry->second.tripCount);
            entry->second.specCount++;
        }

        if (loopQueue.size() == maxLoopQueueSize) {
            loopQueue.pop_front();
        }
        loopQueue.push_back(branchAddr);

    } else {
        assert(entry != loopTable.end());
        DPRINTF(DecoupleBP || debugFlagOn, "not taken update loop detector from "
                "[%#lx, %#lx, %#lx, %d, %d] to [%#lx, %#lx, %#lx, %d, %d]\n",
                branchAddr, entry->second.target, entry->second.outTarget, entry->second.specCount, entry->second.tripCount,
                branchAddr, entry->second.target, entry->second.outTarget, 0, entry->second.specCount);

        entry->second.tripCount = entry->second.specCount;
        entry->second.specCount = 0;
        entry->second.outTarget = targetAddr;
    }

    if (entry->second.target < forwardTaken.first && entry->second.branch > forwardTaken.first) {
        entry->second.intraTaken = true;
        DPRINTF(DecoupleBP || debugFlagOn, "Detect intra taken at %#lx-->%#lx, loop:[%#lx, %#lx]\n",
                forwardTaken.first, forwardTaken.second, entry->second.target, entry->second.branch);
    }

    if (!taken_backward) {
	    streamLoopPredictor->updateEntry(branchAddr, entry->second.target, entry->second.outTarget, entry->second.tripCount, entry->second.intraTaken);
    }
}

} // namespace branch_prediction
} // namespace gem5
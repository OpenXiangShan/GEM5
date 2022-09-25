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
    
    std::string intraTaken =  entry != loopTable.end() && entry->second.intraTaken ? "intraTaken" : "no intraTaken";

    if (taken_backward) {
        if (entry == loopTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop predictor from NULL to [%#lx, %d, %d, %s]\n",
                    branchAddr, 1, 0, intraTaken);
            loopTable[branchAddr] = LoopEntry(branchAddr, targetAddr);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop predictor from [%#lx, %d, %d, %s] to [%#lx, %d, %d]\n",
                    branchAddr, entry->second.specCount, entry->second.tripCount, intraTaken,
                    branchAddr, entry->second.specCount + 1, entry->second.tripCount);
            entry->second.specCount++;
        }

        if (loopQueue.size() == maxLoopQueueSize) {
            loopQueue.pop_front();
        }
        loopQueue.push_back(branchAddr);

    } else {
        assert(entry != loopTable.end());
        DPRINTF(DecoupleBP || debugFlagOn, "not taken update loop predictor from [%#lx, %d, %d, %s] to [%#lx, %d, %d]\n",
                branchAddr, entry->second.specCount, entry->second.tripCount, intraTaken,
                branchAddr, 0, entry->second.specCount);

        entry->second.tripCount = entry->second.specCount;
        entry->second.specCount = 0;
    }

    if (entry->second.target < forwardTakenPC && entry->second.branch > forwardTakenPC) {
        entry->second.intraTaken = true;
        DPRINTF(DecoupleBP || debugFlagOn, "Detect intra taken at %#lx, loop:[%#lx, %#lx]\n",
                forwardTakenPC, entry->second.target, entry->second.branch);
    }
}

} // namespace branch_prediction
} // namespace gem5
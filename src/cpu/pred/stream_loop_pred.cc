#include "cpu/pred/stream_loop_pred.hh"

namespace gem5
{
namespace branch_prediction
{

StreamLoopPred::StreamLoopPred(const Params &params)
            : SimObject(params), maxLoopQueueSize(params.maxLoopQueueSize)
{
        
}

void
StreamLoopPred::update(Addr branchAddr, Addr targetAddr) {
    Addr pc = branchAddr;
    if (pc == ObservingPC) {
        debugFlagOn = true;
    }
        
    bool taken = targetAddr < branchAddr;

    auto entry = predTable.find(pc);

    for (auto it = loopQueue.begin(); it != loopQueue.end();it++) {
        if (*it >= targetAddr && *it < branchAddr) {
            entry->second.intraTaken = true;
            break;
        }
    }
    while(loopQueue.back() > branchAddr)
        loopQueue.pop_back();

    std::string intraTaken = entry->second.intraTaken ? "intraTaken" : "no intraTaken";
    
    if (taken) {
        if (entry == predTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop predictor from NULL to [%#lx, %d, %d, %s]\n",
                    pc, 1, 0, intraTaken);
            predTable[pc] = PredEntry(pc);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop predictor from [%#lx, %d, %d, %s] to [%#lx, %d, %d, %s]\n",
                    pc, entry->second.specCount, entry->second.tripCount, intraTaken,
                    pc, entry->second.specCount + 1, entry->second.tripCount, intraTaken);
            entry->second.specCount++;
        }

        if (loopQueue.size() == maxLoopQueueSize) {
            loopQueue.pop_front();
        }
        loopQueue.push_back(branchAddr);

    } else {
        assert(entry != predTable.end());
        DPRINTF(DecoupleBP || debugFlagOn, "not taken update loop predictor from [%#lx, %d, %d, %s] to [%#lx, %d, %d, %s]\n",
                pc, entry->second.specCount, entry->second.tripCount, intraTaken,
                pc, 0, entry->second.specCount, intraTaken);

        entry->second.tripCount = entry->second.specCount;
        entry->second.specCount = 0;
    }

}

} // namespace branch_prediction
} // namespace gem5
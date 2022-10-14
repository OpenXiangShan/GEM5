#include "cpu/pred/loop_detector.hh"
#include "sim/core.hh"
#include "base/output.hh"

namespace gem5
{
namespace branch_prediction
{

LoopDetector::LoopDetector(const Params &params)
            : SimObject(params), maxLoopQueueSize(params.maxLoopQueueSize)
{
        registerExitCallback([this]() {
        auto out_handle = simout.create("loopHistory.txt", false, true);
        for (auto iter : loopHistory) {
            *out_handle->stream() << iter << std::endl;
        }
        simout.close(out_handle);
    });
}

bool 
LoopDetector::adjustLoopEntry(bool taken_backward, LoopEntry &entry, Addr branchAddr, Addr targetAddr) {
    if (!entry.valid)
        return false;

    if (taken_backward) {
        if (targetAddr != entry.target) {
            entry.valid = false;
            streamLoopPredictor->deleteEntry(branchAddr);
            return false;
        }
    } else {
        if (targetAddr != entry.outTarget && entry.outValid) {
            entry.valid = false;
            streamLoopPredictor->deleteEntry(branchAddr);
            return false;
        }

        if (entry.tripCount != entry.specCount)
            entry.counter--;
        else
            entry.counter++;

        entry.valid = entry.counter > -8;
    }
    return true;
}

void
LoopDetector::update(Addr branchAddr, Addr targetAddr, Addr fallThruPC) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC || branchAddr == ObservingPC2) {
        debugFlagOn = true;
    }
        
    bool taken_backward = targetAddr < branchAddr;

    auto entry = loopTable.find(branchAddr);

    if (entry != loopTable.end() && !adjustLoopEntry(taken_backward, entry->second, branchAddr, targetAddr)) {
        DPRINTF(DecoupleBP || debugFlagOn, "%#lx is not a loop\n", branchAddr);
        return;
    }

    if (taken_backward) {
        if (entry == loopTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop detector from NULL to [%#lx, %#lx, %#lx, %d, %d, %#lx]\n",
                    branchAddr, 0, 0, 1, 0, fallThruPC);
            loopTable[branchAddr] = LoopEntry(branchAddr, targetAddr, fallThruPC);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop detector from "
                    "[%#lx, %#lx, %#lx, %d, %d, %#lx] to [%#lx, %#lx, %#lx, %d, %d, %#lx]\n",
                    branchAddr, entry->second.target, entry->second.outTarget, entry->second.specCount, entry->second.tripCount, entry->second.fallThruPC,
                    branchAddr, entry->second.target, entry->second.outTarget, entry->second.specCount + 1, entry->second.tripCount, entry->second.fallThruPC);
            entry->second.specCount++;
        }

        if (loopQueue.size() == maxLoopQueueSize) {
            loopQueue.pop_front();
        }
        loopQueue.push_back(branchAddr);

    } else {
        assert(entry != loopTable.end());
        DPRINTF(DecoupleBP || debugFlagOn, "not taken update loop detector from "
                "[%#lx, %#lx, %#lx, %d, %d, %#lx] to [%#lx, %#lx, %#lx, %d, %d, %#lx]\n",
                branchAddr, entry->second.target, entry->second.outTarget, entry->second.specCount, entry->second.tripCount, entry->second.fallThruPC,
                branchAddr, entry->second.target, entry->second.outTarget, 0, entry->second.specCount, entry->second.fallThruPC);

        entry->second.tripCount = entry->second.specCount;
        entry->second.specCount = 0;
        entry->second.outTarget = targetAddr;
        entry->second.outValid = true;

        if (branchAddr == ObservingPC) {
            loopHistory.push_back(entry->second.tripCount);
        }
    }

    if (entry->second.target < forwardTaken.first && entry->second.branch > forwardTaken.first) {
        entry->second.intraTaken = true;
        DPRINTF(DecoupleBP || debugFlagOn, "Detect intra taken at %#lx-->%#lx, loop:[%#lx, %#lx]\n",
                forwardTaken.first, forwardTaken.second, entry->second.target, entry->second.branch);
    }

    if (!taken_backward && entry != loopTable.end() && entry->second.valid) {
	    streamLoopPredictor->updateEntry(branchAddr, entry->second.target, entry->second.outTarget, entry->second.fallThruPC,
                                         entry->second.tripCount, entry->second.intraTaken);
    }
}

} // namespace branch_prediction
} // namespace gem5
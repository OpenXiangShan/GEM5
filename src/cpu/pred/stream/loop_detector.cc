#include "cpu/pred/stream/loop_detector.hh"
#include "sim/core.hh"
#include "base/output.hh"

namespace gem5
{
namespace branch_prediction
{
namespace stream_pred
{

StreamLoopDetector::StreamLoopDetector(const Params &params)
            : SimObject(params), maxLoopQueueSize(params.maxLoopQueueSize), tableSize(params.tableSize)
{
        registerExitCallback([this]() {
        auto out_handle = simout.create("loopHistory.txt", false, true);
        for (auto iter : tripCountVec) {
            *out_handle->stream() << std::dec << iter.first << " counter: " << iter.second << std::endl;
        }
        *out_handle->stream() << "replace: " << replaceCount << " invalidTripCount: " << invalidTripCount << " invalidLoopCount: " << invalidLoopCount << std::endl;
        for (auto iter : loopTable) {
            *out_handle->stream() << "loop entry: " << iter.second.valid << " " << std::hex << iter.first << " " << iter.second.branch << " " 
                                  << iter.second.target << " " << iter.second.outTarget << " " << iter.second.fallThruPC << " " 
                                  << std::dec << iter.second.specCount << " " << iter.second.tripCount 
                                  << " " << iter.second.intraTaken << " " << iter.second.outValid 
                                  << " " << iter.second.counter << " " << iter.second.age << std::endl;
        }
        *out_handle->stream() << std::endl;
        for (auto iter : streamLoopPredictor->getLoopTable()) {
            *out_handle->stream() << "stream loop entry: " << iter.second.valid << " " << std::hex << iter.first << " " << iter.second.branch << " " 
                                  << iter.second.target << " " << iter.second.fallThruPC << " " << std::dec << iter.second.tripCount 
                                  << " " << iter.second.detectedCount << " " << iter.second.intraTaken << " " << iter.second.age << std::endl;
        }
        simout.close(out_handle);
    });
}

bool 
StreamLoopDetector::adjustLoopEntry(bool taken_backward, DetectorEntry &entry, Addr branchAddr, Addr targetAddr) {
    if (!entry.valid) {
        if (branchAddr == ObservingPC2) {
            tripCountVec.push_back(std::make_pair(0, -9));
        }
        return false;
    }

    if (taken_backward) {
        if (targetAddr != entry.target) {
            entry.valid = false;
            streamLoopPredictor->deleteEntry(branchAddr);
            invalidLoopCount++;
            return false;
        }
    } else {
        if (targetAddr != entry.outTarget && entry.outValid) {
            entry.valid = false;
            streamLoopPredictor->deleteEntry(branchAddr);
            invalidLoopCount++;
            return false;
        }

        if (entry.tripCount != entry.specCount)
            entry.counter--;
        else
            entry.counter = entry.counter >= 3 ? 3 : entry.counter + 1;

        entry.valid = entry.counter >= -3;
        if (!entry.valid) {
            streamLoopPredictor->deleteEntry(branchAddr);
            invalidTripCount++;
        }
    }
    return true;
}

void
StreamLoopDetector::insertEntry(Addr branchAddr, DetectorEntry loopEntry) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC2) {
        debugFlagOn = true;
    }
    if (loopTable.size() < tableSize) {
        loopTable[branchAddr] = loopEntry;
    } else {
        for (int i = 0; i < 4; i++) {
            for (auto &it : loopTable) {
                if (it.second.age == i) {
                    loopTable.erase(it.first);
                    replaceCount++;
                    loopTable[branchAddr] = loopEntry;
                    assert(loopTable.size() <= tableSize);
                    return;
                }
            }
        }
    }
}

void
StreamLoopDetector::update(Addr branchAddr, Addr targetAddr, Addr fallThruPC) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC || branchAddr == ObservingPC2) {
        debugFlagOn = true;
    }

    counter++;
    if (counter >= 128) {
        for (auto &it : loopTable) {
            it.second.age = it.second.age > 0 ? it.second.age - 1 : 0;
        }
        counter = 0;
    }
        
    bool taken_backward = targetAddr < branchAddr;

    auto entry = loopTable.find(branchAddr);

    if (entry != loopTable.end()) {
        entry->second.age = 3;
        if (!adjustLoopEntry(taken_backward, entry->second, branchAddr, targetAddr)) {
            DPRINTF(DecoupleBP || debugFlagOn, "%#lx is not a loop\n", branchAddr);
            return;
        }
    }

    if (taken_backward) {
        if (entry == loopTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "taken update loop detector from NULL to [%#lx, %#lx, %#lx, %d, %d, %#lx]\n",
                    branchAddr, 0, 0, 1, 0, fallThruPC);
            insertEntry(branchAddr, DetectorEntry(branchAddr, targetAddr, fallThruPC));
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

        if (branchAddr == ObservingPC2) {
            tripCountVec.push_back(std::make_pair(entry->second.tripCount, entry->second.counter));
        }

        if (branchAddr == ObservingPC2) {
            loopHistory.push_back(entry->second.tripCount);
        }
    }


    // intra loop forward taken detection
    if (entry != loopTable.end() &&
        entry->second.target < forwardTaken.first &&
        entry->second.branch > forwardTaken.first) {
        entry->second.intraTaken = true;
        DPRINTF(DecoupleBP || debugFlagOn,
                "Detect intra taken at %#lx-->%#lx, loop:[%#lx, %#lx]\n",
                forwardTaken.first, forwardTaken.second, entry->second.target,
                entry->second.branch);
    }

    // correct loop prediction
    if (!taken_backward && entry != loopTable.end() && entry->second.valid) {
        streamLoopPredictor->updateEntry(
            branchAddr, entry->second.target, entry->second.outTarget,
            entry->second.fallThruPC, entry->second.tripCount,
            entry->second.intraTaken);
    }
}

} // namespace stream_pred
} // namespace branch_prediction
} // namespace gem5
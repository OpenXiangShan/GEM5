#include "cpu/pred/stream/stream_loop_predictor.hh"

namespace gem5
{
namespace branch_prediction
{
namespace stream_pred
{

StreamLoopPredictor::StreamLoopPredictor(const Params &params)
                    : SimObject(params), tableSize(params.tableSize)
{
        
}

void
StreamLoopPredictor::insertEntry(Addr branchAddr, LoopEntry loopEntry) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    if (loopTable.size() < tableSize) {
        loopTable[branchAddr] = loopEntry;
    } else {
        for (int i = 0; i < 4; i++) {
            for (auto &it : loopTable) {
                if (it.second.age == i) {
                    loopTable.erase(it.first);
                    loopTable[branchAddr] = loopEntry;
                    assert(loopTable.size() <= tableSize);
                    return;
                }
            }
        }
    }
}

void
StreamLoopPredictor::updateTripCount(unsigned fsqId, Addr branchAddr)
{
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

    auto entry = loopTable.find(branchAddr);
    if (entry != loopTable.end()) {
        entry->second.age = 3;
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
        updateMRULoop(branchAddr);
    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "can not find corresponding loop entry at %#lx\n", branchAddr);
    }
}

std::pair<bool, Addr>
StreamLoopPredictor::makeLoopPrediction(Addr branchAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC || branchAddr == ObservingPC2) {
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
StreamLoopPredictor::updateEntry(Addr branchAddr, Addr targetAddr, Addr outTarget, Addr fallThruPC, int detectedCount, bool intraTaken) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC || branchAddr == ObservingPC2) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry == loopTable.end()) {
        insertEntry(branchAddr, LoopEntry(branchAddr, targetAddr, outTarget, fallThruPC, detectedCount, intraTaken));
        DPRINTF(DecoupleBP || debugFlagOn, "insert loop table entry: [%#lx, %#lx, %#lx, %d, %d, %#lx]\n",
                branchAddr, targetAddr, outTarget, 0, detectedCount, fallThruPC);
	} else {
        LoopEntry temp = entry->second;
        entry->second.detectedCount = detectedCount;
        entry->second.intraTaken = intraTaken;
        DPRINTF(DecoupleBP || debugFlagOn, "update loop table entry from [%#lx, %#lx, %#lx, %d, %d, %#lx] "
                "to [%#lx, %#lx, %#lx, %d, %d, %#lx]\n",
                temp.branch, temp.target, temp.outTarget, temp.tripCount, temp.detectedCount, temp.fallThruPC,
                entry->second.branch, entry->second.target, entry->second.outTarget,
                entry->second.tripCount, entry->second.detectedCount, entry->second.fallThruPC);
    }
    updateMRULoop(branchAddr);
}

void
StreamLoopPredictor::updateMRULoop(Addr branchAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC) {
        debugFlagOn = true;
    }
    int tripCount = getTripCount(branchAddr);

    if (tripCount == -1) {
        DPRINTF(DecoupleBP || debugFlagOn, "loop at %#lx is not detected, skip update MRU loop\n", branchAddr);
        assert(0);
    }

    for (auto it = mruLoop.begin(); it != mruLoop.end(); it++) {
        if (it->first == branchAddr) {
            mruLoop.erase(it);
            break;
        }
    }
    if (mruLoop.size() == 10) {
        mruLoop.pop_front();
    }
    mruLoop.push_back(std::make_pair(branchAddr, tripCount));
    DPRINTF(DecoupleBP || debugFlagOn, "update MRU loop: %#lx, tripCount: %d\n", branchAddr, tripCount);
}

void
StreamLoopPredictor::restoreLoopTable(std::list<std::pair<Addr, unsigned int>> mruLoop) {
    this->mruLoop = mruLoop;
    for (auto &it : mruLoop) {
        Addr branchAddr = it.first;
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            entry->second.tripCount = it.second;
        }
    }
}

void
StreamLoopPredictor::controlSquash(unsigned fsqId, FetchStream stream, Addr branchAddr, Addr targetAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC || branchAddr == ObservingPC2) {
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

        updateMRULoop(branchAddr);
    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "control squash fsqId: %lu, not found in table\n",
                fsqId);
    }
}

std::pair<bool, std::vector<DivideEntry> >
StreamLoopPredictor::updateTAGE(Addr streamStart, Addr branchAddr, Addr targetAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (streamStart == ObservingPC || branchAddr == ObservingPC2) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    bool hasIntrLoop = false;
    std::vector<DivideEntry> divideEntryVec;
    if (entry != loopTable.end()) {
        Addr tempStart = streamStart;
        for (const auto it : loopTable) {
            if (tempStart < it.second.branch && it.second.branch < entry->second.branch && it.second.outTarget <= entry->second.branch) {
                hasIntrLoop = true;
                DivideEntry temp = DivideEntry(false, tempStart, it.second.branch, it.second.outTarget, it.second.fallThruPC);
                tempStart = it.second.outTarget;
                divideEntryVec.push_back(temp);
                DPRINTF(DecoupleBP || debugFlagOn, "detect loop: %#lx-->%#lx, update tempStart to %#lx\n",
                        it.second.target, it.second.branch, tempStart);
            }
        }
        if (targetAddr == entry->second.target) {
            DivideEntry temp = DivideEntry(true, tempStart, entry->second.branch, targetAddr, entry->second.fallThruPC);
            divideEntryVec.push_back(temp);
        } else if (targetAddr == entry->second.outTarget) {
            DivideEntry temp = DivideEntry(false, tempStart, entry->second.branch, targetAddr, entry->second.fallThruPC);
            divideEntryVec.push_back(temp);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "target address doesn't match\n");
        }
    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "branchAddr: %#lx, not found in table\n",
                branchAddr);
    }

    for (const auto it : divideEntryVec) {
        DPRINTF(DecoupleBP || debugFlagOn, "divided stream: %s, %#lx [%#lx-->%#lx]\n",
                it.taken ? "taken" : "not taken", it.start, it.branch, it.next);
    }

    return std::make_pair(hasIntrLoop, divideEntryVec);
}

void
StreamLoopPredictor::deleteEntry(Addr branchAddr) {
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (branchAddr == ObservingPC2) {
        debugFlagOn = true;
    }
    auto entry = loopTable.find(branchAddr);
    if (entry != loopTable.end()) {
        DPRINTF(DecoupleBP || debugFlagOn, "delete loop table entry: [%#lx, %#lx, %#lx, %d, %d]\n",
                entry->second.branch, entry->second.target, entry->second.outTarget,
                entry->second.tripCount, entry->second.detectedCount);
        loopTable.erase(entry);
    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "delete loop table entry: %#lx, not found in table\n",
                branchAddr);
    }

    for (auto it = mruLoop.begin(); it != mruLoop.end(); it++) {
        if (it->first == branchAddr) {
            mruLoop.erase(it);
            DPRINTF(DecoupleBP || debugFlagOn, "delete loop table entry: %#lx, not found in table\n",
                    branchAddr);
            break;
        }
    }
}

} // namespace stream_pred
} // namespace branch_prediction
} // namespace gem5
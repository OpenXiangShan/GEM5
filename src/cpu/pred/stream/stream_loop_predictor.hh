#ifndef __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__
#define __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__

#include <map>
#include <list>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "cpu/pred/stream/stream_struct.hh"
#include "base/types.hh"
#include "debug/DecoupleBP.hh"
#include "base/debug_helper.hh"
#include "base/trace.hh"
#include "params/StreamLoopPredictor.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace stream_pred
{

class StreamLoopPredictor : public SimObject
{

    using defer = std::shared_ptr<void>;
public: 
    typedef StreamLoopPredictorParams Params;

    StreamLoopPredictor(const Params &params);
    
private:

    std::map<Addr, LoopEntry> loopTable;

    std::list<std::pair<Addr, unsigned int>> mruLoop; // most recently used loop

    bool debugFlagOn{false};

    unsigned int tableSize;

    unsigned int counter = 0;

    int loopUseCount = 0;

public:

    bool loopValid() { return loopUseCount >= 0; }

    void updateLoopUseCount(bool up) {
        if (up) {
            loopUseCount = loopUseCount >= 8 ? 8 : loopUseCount + 1;
        } else {
            loopUseCount = loopUseCount <= -8 ? -8 : loopUseCount - 1;
        }
    }

    std::pair<bool, Addr> makeLoopPrediction(Addr branchAddr);

    void updateTripCount(unsigned fsqId, Addr branchAddr);

    bool getLoopPredValid(Addr branchAddr) {
        defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
        if (branchAddr == ObservingPC) {
            debugFlagOn = true;
        }
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "get loop entry valid at %#lx: %d\n", branchAddr, entry->second.valid);
            return entry->second.valid;
        }
        DPRINTF(DecoupleBP || debugFlagOn, "can not get loop entry at %#lx\n", branchAddr);
        return false;
    }

    int getTripCount(Addr branchAddr) {
        defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
        if (branchAddr == ObservingPC) {
            debugFlagOn = true;
        }
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            DPRINTF(DecoupleBP || debugFlagOn, "get trip count at %#lx: %d\n", branchAddr, entry->second.tripCount);
            return entry->second.tripCount;
        }
        DPRINTF(DecoupleBP || debugFlagOn, "can not get trip count at %#lx\n", branchAddr);
        return -1;
    }
    
    std::list<std::pair<Addr, unsigned int>> getMRULoop() { return this->mruLoop; }

    void restoreLoopTable(std::list<std::pair<Addr, unsigned int>> mruLoop);

    void updateMRULoop(Addr branchAddr);

    bool isTakenForward(Addr branchAddr) {
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            return entry->second.tripCount == entry->second.detectedCount;
        }
        DPRINTF(DecoupleBP || debugFlagOn, "can not get trip count at %#lx\n", branchAddr);
        return false;
    }

    void updateEntry(Addr branchAddr, Addr targetAddr, Addr outTarget, Addr fallThruPC, int detectedCount, bool intraTaken);

    void controlSquash(unsigned fsqId, FetchStream stream, Addr branchAddr, Addr targetAddr);

    void deleteEntry(Addr branchAddr);

    void insertEntry(Addr branchAddr, LoopEntry loopEntry);

    std::map<Addr, LoopEntry> getLoopTable() { return loopTable; }

    std::pair<bool, std::vector<DivideEntry> > updateTAGE(Addr streamStart, Addr branchAddr, Addr targetAddr);

};

}

}

}

#endif  // __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__

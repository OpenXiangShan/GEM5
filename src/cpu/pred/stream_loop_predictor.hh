#ifndef __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__
#define __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__

#include <map>
#include <list>
#include <utility>

#include "base/statistics.hh"
#include "cpu/pred/stream_struct.hh"
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

class StreamLoopPredictor : public SimObject
{

    using defer = std::shared_ptr<void>;
public: 
    typedef StreamLoopPredictorParams Params;

    StreamLoopPredictor(const Params &params);
    
private:

   struct LoopEntry
    {
        // may need to add a valid bit
        bool valid;
        Addr branch;
        Addr target;
        Addr outTarget;
        int tripCount;
        int detectedCount;
        bool intraTaken;

        LoopEntry() : valid(true), branch(0), target(0), outTarget(0), tripCount(0), detectedCount(0), intraTaken(false) {}
        LoopEntry(Addr branch, Addr target, Addr outTarget, int detectedCount, bool intraTaken) : 
                 valid(true), branch(branch), target(target), outTarget(outTarget), tripCount(0),
                 detectedCount(detectedCount), intraTaken(intraTaken) {}
    };

    std::map<Addr, LoopEntry> loopTable;

    bool debugFlagOn{false};

public:

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

    bool isTakenForward(Addr branchAddr) {
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            return entry->second.tripCount == entry->second.detectedCount;
        }
        DPRINTF(DecoupleBP || debugFlagOn, "can not get trip count at %#lx\n", branchAddr);
        assert(0);
    }

    void updateEntry(Addr branchAddr, Addr targetAddr, Addr outTarget, int detectedCount, bool intraTaken);

    void controlSquash(unsigned fsqId, FetchStream stream, Addr branchAddr, Addr targetAddr);

    void isIntraSquash(unsigned fsqId, FetchStream stream, Addr branchAddr);

};

}

}

#endif  // __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__

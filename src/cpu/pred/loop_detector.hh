#ifndef __CPU_PRED_LOOP_DETECTOR_HH__
#define __CPU_PRED_LOOP_DETECTOR_HH__

#include <map>
#include <list>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/pred/stream_loop_predictor.hh"
#include "debug/DecoupleBP.hh"
#include "base/debug_helper.hh"
#include "base/trace.hh"
#include "params/LoopDetector.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class LoopDetector : public SimObject
{

    using defer = std::shared_ptr<void>;
public: 
    typedef LoopDetectorParams Params;

    LoopDetector(const Params &params);
    
private:

    unsigned maxLoopQueueSize;

    struct LoopEntry
    {
        Addr branch;
        Addr target;
        Addr outTarget;
        int specCount;
        int tripCount;
        bool intraTaken;

        LoopEntry() : branch(0), target(0), outTarget(0), specCount(0), tripCount(0), intraTaken(false) {}
        LoopEntry(Addr branch, Addr target) : branch(branch), target(target), outTarget(0), specCount(1), tripCount(0), intraTaken(false) {}
    };

    std::map<Addr, LoopEntry> loopTable;

    std::list<Addr> loopQueue;

    std::pair<Addr, Addr> forwardTaken; // the most recent forward taken PC

    StreamLoopPredictor *streamLoopPredictor{};

    bool debugFlagOn{false};

    std::vector<int> loopHistory;

public:

    void setStreamLoopPredictor(StreamLoopPredictor *slp) { streamLoopPredictor = slp; }

    bool findLoop(Addr branchAddr) {
        if (loopTable.find(branchAddr) != loopTable.end()) {
            return true;
        }
        return false;
    }

    int getTripCount(Addr pc) {
        if (loopTable.find(pc) == loopTable.end())
            return -1;
        else
            return loopTable[pc].tripCount;
    }

    int getSpecCount(Addr pc) {
        if (loopTable.find(pc) == loopTable.end())
            return -1;
        else
            return loopTable[pc].specCount;
    }

    void update(Addr branchAddr, Addr targetAddr);

    void setRecentForwardTakenPC(Addr branch, Addr target) {
        forwardTaken = std::make_pair(branch, target);
    }

};

}

}

#endif  // __CPU_PRED_LOOP_DETECTOR_HH__

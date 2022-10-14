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
        bool valid;
        Addr branch;
        Addr target;
        Addr outTarget;
        Addr fallThruPC;
        int specCount;
        int tripCount;
        bool intraTaken;
        bool outValid;
        int counter;

        LoopEntry() : valid(true), branch(0), target(0), outTarget(0), fallThruPC(0), specCount(0), tripCount(0), 
                      intraTaken(false), outValid(false), counter(0) {}
        LoopEntry(Addr branch, Addr target, Addr fallThruPC) : valid(true), branch(branch), target(target), 
                                                               outTarget(0), fallThruPC(fallThruPC), specCount(1), 
                                                               tripCount(0), intraTaken(false), outValid(false), counter(0) {}
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

    void update(Addr branchAddr, Addr targetAddr, Addr fallThruPC);

    void setRecentForwardTakenPC(Addr branch, Addr target) {
        forwardTaken = std::make_pair(branch, target);
    }

    bool adjustLoopEntry(bool taken_backward, LoopEntry &entry, Addr branchAddr, Addr targetAddr);

    bool loopUpValid(Addr branchAddr) {
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            return entry->second.valid;
        } else {
            return false;
        }
    }

};

}

}

#endif  // __CPU_PRED_LOOP_DETECTOR_HH__

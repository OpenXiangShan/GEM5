#ifndef __CPU_PRED_LOOP_DETECTOR_HH__
#define __CPU_PRED_LOOP_DETECTOR_HH__

#include <map>
#include <list>

#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/DecoupleBP.hh"
#include "base/debug_helper.hh"
#include "base/trace.hh"
#include "params/StreamLoopPred.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class StreamLoopPred : public SimObject
{

public: 
    typedef StreamLoopPredParams Params;

    StreamLoopPred(const Params &params);
    
private:

    unsigned maxLoopQueueSize;

    struct LoopEntry
    {
        Addr pc;
        int specCount;
        int tripCount;
        bool intraTaken;

        LoopEntry() : pc(0), specCount(0), tripCount(0), intraTaken(false) {}
        LoopEntry(Addr pc) : pc(pc), specCount(1), tripCount(0), intraTaken(false) {}
    };

    std::map<Addr, LoopEntry> loopTable;

    std::list<Addr> loopQueue;

    Addr forwardTakenPC; // the most recent forward taken PC

    bool debugFlagOn{false};

public:

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

    void setRecentForwardTakenPC(Addr pc) {
        forwardTakenPC = pc;
    }

};

}

}

#endif  // __CPU_PRED_LOOP_DETECTOR_HH__

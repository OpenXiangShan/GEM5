#ifndef __CPU_PRED_STREAM_LOOP_PRED_HH__
#define __CPU_PRED_STREAM_LOOP_PRED_HH__

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

    struct PredEntry
    {
        Addr pc;
        int specCount;
        int tripCount;
        bool intraTaken;

        PredEntry() : pc(0), specCount(0), tripCount(0), intraTaken(false) {}
        PredEntry(Addr pc) : pc(pc), specCount(1), tripCount(0), intraTaken(false) {}
    };

    std::map<Addr, PredEntry> predTable;

    std::list<Addr> loopQueue;

    bool debugFlagOn{false};

public:

    bool findLoop(Addr branchAddr) {
        if (predTable.find(branchAddr) != predTable.end()) {
            return true;
        }
        return false;
    }

    int getTripCount(Addr pc) {
        if (predTable.find(pc) == predTable.end())
            return -1;
        else
            return predTable[pc].tripCount;
    }

    int getSpecCount(Addr pc) {
        if (predTable.find(pc) == predTable.end())
            return -1;
        else
            return predTable[pc].specCount;
    }

    void update(Addr branchAddr, Addr targetAddr);

    void detectIntraTaken();

};

}

}

#endif  // __CPU_PRED_STREAM_LOOP_PRED_HH__

#ifndef __CPU_PRED_STREAM_LOOP_DETECTOR_HH__
#define __CPU_PRED_STREAM_LOOP_DETECTOR_HH__

#include <map>
#include <list>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "base/debug_helper.hh"
#include "cpu/pred/stream/stream_loop_predictor.hh"
#include "debug/DecoupleBP.hh"
#include "base/debug_helper.hh"
#include "base/trace.hh"
#include "params/StreamLoopDetector.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace stream_pred
{

class StreamLoopDetector : public SimObject
{

    using defer = std::shared_ptr<void>;
public: 
    typedef StreamLoopDetectorParams Params;

    StreamLoopDetector(const Params &params);
    
private:

    unsigned maxLoopQueueSize;
    unsigned tableSize;

    struct DetectorEntry
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
        unsigned age;

        DetectorEntry() : valid(true), branch(0), target(0), outTarget(0), fallThruPC(0), specCount(0), tripCount(0), 
                      intraTaken(false), outValid(false), counter(0) , age(3) {}
        DetectorEntry(Addr branch, Addr target, Addr fallThruPC) : valid(true), branch(branch), target(target), 
                                                               outTarget(0), fallThruPC(fallThruPC), specCount(1), 
                                                               tripCount(0), intraTaken(false), outValid(false), counter(0), age(3) {}
    };

    std::map<Addr, DetectorEntry> loopTable;

    std::list<Addr> loopQueue;

    std::pair<Addr, Addr> forwardTaken; // the most recent forward taken PC

    StreamLoopPredictor *streamLoopPredictor{};

    bool debugFlagOn{false};

    std::vector<int> loopHistory;

    unsigned int counter = 0;

public:

    void setStreamLoopPredictor(StreamLoopPredictor *slp) { streamLoopPredictor = slp; }

    bool findLoop(Addr branchAddr) {
        if (loopTable.find(branchAddr) != loopTable.end()) {
            return true;
        }
        return false;
    }

    void update(Addr branchAddr, Addr targetAddr, Addr fallThruPC);

    void setRecentForwardTakenPC(Addr branch, Addr target) {
        forwardTaken = std::make_pair(branch, target);
    }

    void insertEntry(Addr branchAddr, DetectorEntry loopEntry);

    bool adjustLoopEntry(bool taken_backward, DetectorEntry &entry, Addr branchAddr, Addr targetAddr);

    bool loopUpValid(Addr branchAddr) {
        auto entry = loopTable.find(branchAddr);
        if (entry != loopTable.end()) {
            return entry->second.valid;
        } else {
            return false;
        }
    }

    long replaceCount = 0;

    long invalidTripCount = 0;

    long invalidLoopCount = 0;

    std::vector<std::pair<int, int>> tripCountVec;

};

}

}

}

#endif  // __CPU_PRED_STREAM_LOOP_DETECTOR_HH__

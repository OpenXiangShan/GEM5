#ifndef __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__
#define __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__

#include <map>
#include <list>
#include <utility>

#include "base/statistics.hh"
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
        Addr branch;
        Addr target;
        Addr outTarget;
        int tripCount;
        int detectedCount;
        bool intraTaken;

        LoopEntry() : branch(0), target(0), outTarget(0), tripCount(0), detectedCount(0), intraTaken(false) {}
        LoopEntry(Addr branch, Addr target, Addr outTarget, int detectedCount, bool intraTaken) : 
                 branch(branch), target(target), outTarget(outTarget), tripCount(0), detectedCount(detectedCount), intraTaken(intraTaken) {}
    };

    std::map<Addr, LoopEntry> loopTable;

    bool debugFlagOn{false};

public:

    std::pair<bool, Addr> makeLoopPrediction(Addr branchAddr);

    void updateEntry(Addr branchAddr, Addr targetAddr, Addr outTarget, int detectedCount, bool intraTaken);

    void resetTripCount(Addr branchAddr);

};

}

}

#endif  // __CPU_PRED_STREAM_LOOP_PREDICTOR_HH__

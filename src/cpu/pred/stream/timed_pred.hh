#ifndef __CPU_PRED_STREAM_TIMED_PRED_HH__
#define __CPU_PRED_STREAM_TIMED_PRED_HH__


#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream/stream_struct.hh"
#include "params/TimedStreamPredictor.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace stream_pred
{

class TimedStreamPredictor: public SimObject
{
    public:

    typedef TimedStreamPredictorParams Params;

    TimedStreamPredictor(const Params &params);

    virtual void tickStart() {}
    virtual void tick() {}
    virtual void putPCHistory(Addr pc, Addr curChunkStart,
                              const boost::dynamic_bitset<> &history) {}

    virtual StreamPrediction getStream() { panic("Not implemented"); }

    virtual unsigned getDelay() {return 0;}

};

} // namespace stream_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_STREAM_TIMED_PRED_HH__

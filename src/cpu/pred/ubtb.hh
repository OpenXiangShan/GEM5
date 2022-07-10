#ifndef __CPU_PRED_UBTB_HH__
#define __CPU_PRED_UBTB_HH__

#include <map>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/timed_pred.hh"
#include "params/StreamUBTB.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

class StreamUBTB : public TimedPredictor
{
  public:
    typedef StreamUBTBParams Params;

    typedef uint64_t PCHistTag;

    struct TickedStreamStorage : public StreamStorage
    {
        uint64_t tick;
    };

    using UBTBMap = std::map<PCHistTag, TickedStreamStorage>;
    using UBTBMapIter = typename UBTBMap::iterator;

    using UBTBHeap = std::vector<UBTBMapIter>;

    struct older
    {
        bool operator()(const UBTBMapIter &a, const UBTBMapIter &b) const
        {
            return a->second.tick > b->second.tick;
        }
    };


  private:
    const unsigned delay{1};

    const unsigned size{32};

    UBTBMap ubtb;

    UBTBHeap mruList;

  public:
    StreamUBTB(const Params &p);

    void tickStart() override;

    void tick() override;

    void putPCHistory(Addr pc,
                      const boost::dynamic_bitset<> &history) override;

    unsigned getDelay() override { return delay; }

    StreamPrediction getStream();

    void update(const FetchStreamId pred_id, Addr stream_start_pc,
                Addr control_pc, Addr target, bool is_conditional,
                bool is_indirect, unsigned control_size, bool actually_taken,
                const boost::dynamic_bitset<> &history);

    uint64_t makePCHistTag(Addr pc, const boost::dynamic_bitset<> &history);

    StreamPrediction prediction;
};

}

}

#endif  // __CPU_PRED_UBTB_HH__

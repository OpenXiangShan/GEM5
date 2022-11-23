#ifndef __CPU_PRED_STREAM_UBTB_HH__
#define __CPU_PRED_STREAM_UBTB_HH__

#include <map>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream/stream_struct.hh"
#include "cpu/pred/stream/timed_pred.hh"
#include "params/StreamUBTB.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace stream_pred
{

class StreamUBTB : public TimedStreamPredictor
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

    const unsigned size;

    UBTBMap ubtb;

    UBTBHeap mruList;

    struct UBTBStatGroup : public statistics::Group
    {
      UBTBStatGroup(StreamUBTB& s);
      statistics::Scalar coldMisses;  // never seen
      statistics::Scalar capacityMisses;  // seen but limited by capacity
      statistics::Scalar compulsoryMisses;  // seen but not predicted correctly
    } ubtbStats;

    struct HistEntry
    {
        Addr streamStart;
        Addr branchAddr;
        Addr targetAddr;
    };

    std::unordered_map<Addr, HistEntry> fullHist;

  public:
    StreamUBTB(const Params &p);

    void tickStart() override;

    void tick() override;

    void putPCHistory(Addr cur_chunk_start, Addr stream_start,
                      const boost::dynamic_bitset<> &history) override;

    unsigned getDelay() override { return delay; }

    StreamPrediction getStream() override;

    void update(/*const FetchStreamId pred_id,*/ 
                Addr last_chunk_start, Addr stream_start_pc,
                Addr control_pc, Addr target, /*bool is_conditional,*/
                /*bool is_indirect,*/ unsigned control_size, bool actually_taken,
                const boost::dynamic_bitset<> &history, EndType end_type);

    void commit(const FetchStreamId pred_id, Addr stream_start_pc,
                Addr control_pc, Addr target, unsigned control_size,
                const boost::dynamic_bitset<> &history);

    uint64_t makePCHistTag(Addr pc, const boost::dynamic_bitset<> &history);

    StreamPrediction prediction;

    boost::dynamic_bitset<> loMask;

    unsigned historyLen{};
    unsigned usedBits{};
    boost::dynamic_bitset<> usedMask;
};

}

}

}

#endif  // __CPU_PRED_STREAM_UBTB_HH__

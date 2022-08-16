#ifndef __CPU_PRED_DECOUPLED_TAGE_HH__
#define __CPU_PRED_DECOUPLED_TAGE_HH__

#include <map>
#include <unordered_map>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/stream_struct.hh"
#include "cpu/pred/timed_pred.hh"
#include "params/StreamTage.hh"

namespace gem5 {

namespace branch_prediction {

class StreamTage : public TimedPredictor {
public:
    typedef StreamTageParams Params;
#define cast(type,x) static_cast<type*>(x)
    using bitset = boost::dynamic_bitset<>;
    struct BaseEntry {
        StreamStorage stream;
        bool valid = false;
    };
    struct TageEntry : public BaseEntry {
        uint64_t tag;
        uint8_t cnt = 0;
        uint8_t useful = 0;
    };

    class Table {
        BaseEntry* entry;
        uint8_t reset_counter = 128;
        uint64_t tagMask;
        uint64_t indexMask;
        uint64_t getIndex(Addr pc, const bitset& history);
        uint64_t getTag(Addr pc, const bitset& history);
    public:
        bool isBasePred;
        BaseEntry* entry_found;
        //set the table size and the tag bit size
        //if the tag bit size is zero,this table will set to be base predictor
        void init(uint32_t Tsize, uint32_t TtagSize);
        bool lookup(Addr pc, const bitset& history);
        //when predict miss,alloc a new entry
        //old_pc:used to replace the old entry
        //new_pc:the new pc for new entry's tag,normally this pc is stream.bbstart
        bool allocate(Addr old_pc, Addr new_pc,const bitset& history, StreamStorage& new_stream);
        //when predict miss or hit, need to update the cnt and usefulbit;
        //if provider hit and alt miss, set the usefulbit to 1
        //if hit,then cnt++,else cnt--;
        //if this is a base predictor,do nothing
        void update(Addr pc,const bitset& history, bool setUsefulBit, bool hit);
    };
    std::vector<Table> targetCache;    


private:
    const unsigned delay{ 1 };
    struct StatGroup : public statistics::Group {
        StatGroup(StreamTage& s);

    } TageStats;

    StreamPrediction prediction;
    std::vector<StreamStorage> lookup_result;
    //return 0:use T1 main pred
    //return 1:use T2 alt pred
    int getProviderIndex(Table& T1, Table& T2);
public:
    StreamTage(const Params& p);

    void tickStart() override;

    void tick() override;
    //look up the table
    void putPCHistory(Addr pc, const boost::dynamic_bitset<>& history) override;

    unsigned getDelay() override { return delay; }
    //get the prediction result
    StreamPrediction getStream();
    //预测错误时调用,冲刷错误预测之后的结果,同时更新表项
    void update(const FetchStreamId pred_id, Addr stream_start_pc,
                Addr control_pc, Addr target, bool is_conditional,
                bool is_indirect, unsigned control_size, bool actually_taken,
                const boost::dynamic_bitset<>& history);
    //预测正确时调用,负责提交预测正确的stream,同时更新表项
    void commit(const FetchStreamId pred_id, Addr stream_start_pc,
                Addr control_pc, Addr target, unsigned control_size,
                const boost::dynamic_bitset<>& history);

private:
    


};

}

}

#endif  

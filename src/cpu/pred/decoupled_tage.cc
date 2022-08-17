#include "cpu/pred/decoupled_tage.hh"



namespace gem5 {

namespace branch_prediction {

void StreamTage::Table::init(uint32_t Tsize, uint32_t TtagSize, uint32_t tagPcShift, uint32_t histLength) {
    if (TtagSize == 0) {
        this->isBasePred = true;
        this->entry = new BaseEntry[(1 << (ceilLog2(Tsize)))];
    }
    else {
        this->isBasePred = false;
        this->entry = new TageEntry[(1 << (ceilLog2(Tsize)))];
        tagMask = (1 << TtagSize) - 1;
    }
    indexMask = (1 << (ceilLog2(Tsize))) - 1;
    this->tagPcShift = tagPcShift;
    this->histLength = histLength;

}
bool StreamTage::Table::lookup(Addr pc,const bitset& history) {
    uint64_t index = this->getIndex(pc, history);
    if (isBasePred) {
        if (cast(BaseEntry, entry)[index].valid &&
            (pc >= cast(BaseEntry, entry)[index].stream.bbStart &&
            pc <= cast(BaseEntry, entry)[index].stream.controlAddr)) {
            entry_found = &cast(BaseEntry, entry)[index];
            return true;
        }
        else {
            return false;
        }
    }
    else {
        uint64_t tag = this->getTag(pc, history);
        if (cast(TageEntry, entry)[index].valid &&
            (cast(TageEntry, entry)[index].tag == tag) &&
            (pc >= cast(TageEntry, entry)[index].stream.bbStart &&
             pc <= cast(TageEntry, entry)[index].stream.controlAddr)) {
            entry_found = &cast(TageEntry, entry)[index];
            return true;
        }
        else {
            return false;
        }
    }
}
bool StreamTage::Table::allocate(Addr old_pc, Addr new_pc,const bitset& history, StreamStorage& new_stream){
    uint64_t new_index = getIndex(old_pc, history);
    if (isBasePred) {
        cast(BaseEntry, entry)[new_index].stream = new_stream;
        cast(BaseEntry, entry)[new_index].valid = true;
        return true;
    }
    else {
        if (cast(TageEntry, entry)[new_index].useful == 0) {
            uint64_t new_tag = getTag(new_pc, history);
            cast(TageEntry, entry)[new_index].stream = new_stream;
            cast(TageEntry, entry)[new_index].valid = true;
            cast(TageEntry, entry)[new_index].tag = new_tag;
            cast(TageEntry, entry)[new_index].cnt = 1;
            cast(TageEntry, entry)[new_index].useful = 0;
            return true;
        }
        else {
            if (reset_counter > 0) {
                reset_counter--;
            }
            if (reset_counter == 0) {
                reset_counter = 128;
                for (uint64_t i = 0; i < (indexMask+1); i++) {
                    cast(TageEntry, entry)[i].useful = 0;
                }
            }
            return false;
        }
    }
}
void StreamTage::Table::update(Addr pc,const bitset& history, bool setUsefulBit, bool hit) {
    //no need to lookup again
    //beacuse update must be called after lookup
    bool findit = true;//lookup(pc, history);
    if (findit) {
        if (isBasePred) {
            //do nothing
        }
        else {
            if (hit) {
                if (cast(TageEntry, entry_found)->cnt < 3) {
                    cast(TageEntry, entry_found)->cnt++;
                }
                if (setUsefulBit) {
                    cast(TageEntry, entry_found)->useful = 1;
                }
            }
            else {
                if (cast(TageEntry, entry_found)->cnt > 0) {
                    cast(TageEntry, entry_found)->cnt--;
                }
            }
        }
    }
}

uint64_t StreamTage::Table::getIndex(Addr pc, const bitset& history) {
    bitset temp(history);
    temp.resize(64);
    uint64_t folded_address, k;
    folded_address = 0;
    for (k = 0; k < 8; k++) {
        folded_address ^= pc;
        pc >>= 8;
    }
    return (folded_address) & indexMask;
}
uint64_t StreamTage::Table::getTag(Addr pc, const bitset& history) {
    bitset temp(history);
    temp.resize(64);
    pc >>= tagPcShift;
    return (pc ^ temp.to_ulong()) & tagMask;
}

/***********************************************************************/
StreamTage::StreamTage(const Params& p):
    TimedPredictor(p),
    TageStats(*this),
    tableSizes(p.tableSizes),
    TTagBitSizes(p.TTagBitSizes),
    TTagPcShifts(p.TTagPcShifts),
    histLengths(p.histLengths) {
    
    targetCache.resize(tableSizes.size());
    for (uint64_t i = 0; i < targetCache.size(); i++) {
        targetCache[i].init(tableSizes[i], TTagBitSizes[i],TTagPcShifts[i],histLengths[i]);
    }
}

StreamTage::StatGroup::StatGroup(StreamTage& s):
    statistics::Group(&s,"none"){

}

void StreamTage::tickStart() {
    prediction.valid = false;
}

void StreamTage::tick() {}

int StreamTage::getProviderIndex(Table& T1, Table& T2) {
    int ret = 0;
    if (T2.isBasePred) {//if T2 is basePred
        return 0;
    }
    else {
        if(cast(TageEntry, T1.entry_found)->useful > cast(TageEntry, T2.entry_found)->useful &&
            cast(TageEntry, T1.entry_found)->cnt > 1) {
            ret = 0;
        }
        else if (cast(TageEntry, T1.entry_found)->useful == cast(TageEntry, T2.entry_found)->useful) {
            
            if (cast(TageEntry, T1.entry_found)->cnt >= cast(TageEntry, T2.entry_found)->cnt) {
                ret = 0;
            }
            else{
                ret = 1;
            }
        }
        else {
            ret = 1;
        }
    }
    if (use_alt_counter > 10) {
        ret = 1;
    }
    return ret;
}

void StreamTage::putPCHistory(Addr pc, const boost::dynamic_bitset<>& history) {
    int provider_rdy = 0;
    BaseEntry* entry_found[2]={nullptr,nullptr};
    int Tfound_index[2];
    for (int i = targetCache.size()-1;i >=0 ;i--) {
        if (targetCache[i].lookup(pc, history)) {
            entry_found[provider_rdy] = targetCache[i].entry_found;
            Tfound_index[provider_rdy] = i;
            provider_rdy++;
        }
        if (provider_rdy == 2) {
            break;
        }
    }
    if (provider_rdy == 0) {
        prediction.valid = false;
        prediction.history=history;
    }
    else if (provider_rdy == 1) {
        prediction.valid = true;
        prediction.bbStart = pc;
        prediction.controlAddr = entry_found[0]->stream.controlAddr;
        prediction.controlSize = entry_found[0]->stream.controlSize;
        prediction.nextStream = entry_found[0]->stream.nextStream;
        prediction.endIsRet = entry_found[0]->stream.endIsRet;
        prediction.history = history;
    }
    else if (provider_rdy == 2) {
        int use_index = getProviderIndex(targetCache[Tfound_index[0]], targetCache[Tfound_index[1]]);
        prediction.valid = true;
        prediction.bbStart = pc;
        prediction.controlAddr = entry_found[use_index]->stream.controlAddr;
        prediction.controlSize = entry_found[use_index]->stream.controlSize;
        prediction.nextStream = entry_found[use_index]->stream.nextStream;
        prediction.endIsRet = entry_found[use_index]->stream.endIsRet;
        prediction.history = history;
    }
}

StreamPrediction StreamTage::getStream() {
    return prediction;
}

void StreamTage::update(const PredictionID fsq_id, Addr stream_start_pc,
                   Addr control_pc, Addr target, bool is_conditional,
                   bool is_indirect, unsigned control_size,
                   bool actually_taken,
                   const boost::dynamic_bitset<>& history) {
    StreamStorage new_stream={
        .bbStart = stream_start_pc,
        .controlAddr = control_pc,
        .nextStream = target,
        .controlSize = (uint16_t)control_size,
        .hysteresis = 0,
        .endIsRet = false
    };
    int provider_rdy = 0;
    int Tfound_index[2];
    for (int i = targetCache.size()-1;i >= 0 ;i--) {
        if (targetCache[i].lookup(stream_start_pc, history)) {
            Tfound_index[provider_rdy] = i;
            provider_rdy++;
        }
        if (provider_rdy == 2) {
            break;
        }
    }
    int Tstart_index = 0;
    if (provider_rdy == 0) {//all in miss
        Tstart_index = 0;
    }
    else if (provider_rdy == 1) {//only one found,this must in missing
        Tstart_index = Tfound_index[0] + 1;
        targetCache[Tfound_index[0]].update(stream_start_pc, history, false, false);
    }
    else if (provider_rdy == 2) {//found 2 entry,and the provider must in missing
        int use_index = getProviderIndex(targetCache[Tfound_index[0]], targetCache[Tfound_index[1]]);
        Tstart_index = Tfound_index[use_index] + 1;
        int provider_index = Tfound_index[use_index];
        int alt_index = Tfound_index[1 - use_index];
        
        //update,the provider is must in missing
        targetCache[provider_index].update(stream_start_pc, history, false, false);
        if (targetCache[alt_index].entry_found->stream.equal(new_stream)) {//if alt is hit
            targetCache[alt_index].update(stream_start_pc, history, true, true);
        }
    }
    //allocate new stream
    int allocate_cnt = 0;
    for (;Tstart_index < targetCache.size();Tstart_index++) {
        targetCache[Tstart_index].allocate(stream_start_pc, stream_start_pc, history, new_stream);
        allocate_cnt++;
        Tstart_index++;
        if (allocate_cnt >= 3) {//allocate 3
            break;
        }
    }
}


void StreamTage::commit(const FetchStreamId pred_id, Addr stream_start_pc,
                   Addr control_pc, Addr target, unsigned control_size,
                        const boost::dynamic_bitset<>& history) {
    //todo:when pred hit,update the stream
    StreamStorage now_stream={
        .bbStart = stream_start_pc,
        .controlAddr = control_pc,
        .nextStream = target,
        .controlSize = (uint16_t)control_size,
        .hysteresis = 0,
        .endIsRet = false
    };
    int provider_rdy = 0;
    for (int i = targetCache.size() - 1;i >= 0;i--) {
        bool foundit = targetCache[i].lookup(stream_start_pc,history);
        if(foundit){
            if(targetCache[i].entry_found->stream.equal(now_stream)){
                targetCache[i].update(stream_start_pc, history, true, true);
                provider_rdy++;
            }
        }
        if (provider_rdy == 2) {
            break;
        }
    }
}







}

}
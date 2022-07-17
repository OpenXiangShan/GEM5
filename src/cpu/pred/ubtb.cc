#include "cpu/pred/ubtb.hh"

#include "base/trace.hh"
#include "debug/DecoupleBP.hh"

namespace gem5 {

namespace branch_prediction {

StreamUBTB::StreamUBTB(const Params& p):
    TimedPredictor(p),
    ubtbStats(*this)
{
    for (auto i = 0; i < size; i++) {
        ubtb[0xfffffff - i];  // dummy initialization
    }
    for (auto it = ubtb.begin(); it != ubtb.end(); it++) {
        it->second.tick = 0;
        mruList.push_back(it);
    }
    std::make_heap(mruList.begin(), mruList.end(), older());
    prediction.valid = false;
}

StreamUBTB::UBTBStatGroup::UBTBStatGroup(StreamUBTB& s):
    statistics::Group(&s,"UBTB"),
    ADD_STAT(coldMisses, "never seen"),
    ADD_STAT(capacityMisses, "seen but limited by capacity"),
    ADD_STAT(compulsoryMisses, "seen but not predicted correctly")
{
    coldMisses.prereq(coldMisses);
    capacityMisses.prereq(capacityMisses);
    compulsoryMisses.prereq(compulsoryMisses);
}

void
StreamUBTB::tickStart()
{
    prediction.valid = false;
}

void
StreamUBTB::tick() {}

void
StreamUBTB::putPCHistory(Addr pc, const boost::dynamic_bitset<> &history) {
    DPRINTF(DecoupleBP, "Prediction request: stream start=%#lx\n", pc);
    const auto &it = ubtb.find(pc);  // TODO: use hash of pc and history
    if (it == ubtb.end()) {
        DPRINTF(DecoupleBP, "No entry found, guess an unlimited stream\n");
        prediction.valid = false;
        // prediction.bbStart = pc;
        // prediction.bbEnd = 0;
        // prediction.streamLength = unlimitedStreamLen;
        // prediction.nextStream = 0;
        // prediction.endIsRet = false;
    } else {
        DPRINTF(DecoupleBP, "UBTB Entry found\n");
        prediction.valid = true;
        prediction.bbStart = pc;
        prediction.bbEnd = it->second.bbEnd;
        prediction.streamLength = it->second.length;
        prediction.nextStream = it->second.nextStream;
        prediction.endIsRet = it->second.endIsRet;
        prediction.history = history;
    }
}

StreamPrediction
StreamUBTB::getStream() {
    if (prediction.valid) {
        DPRINTF(DecoupleBP,
                "Response stream prediction: %#lx->%#lx\n",
                prediction.bbStart,
                prediction.bbEnd);
    } else {
        DPRINTF(DecoupleBP, "Response invalid prediction\n");
    }
    return prediction;
}

void
StreamUBTB::update(const PredictionID fsq_id, Addr stream_start_pc,
                   Addr control_pc, Addr target, bool is_conditional,
                   bool is_indirect, unsigned control_size,
                   bool actually_taken,
                   const boost::dynamic_bitset<> &history) {
    if (control_pc < stream_start_pc) {
        DPRINTF(DecoupleBP,
                "Control PC %#lx is before stream start %#lx, ignore it\n",
                control_pc,
                stream_start_pc);
        return;
    }
    std::string buf;
    boost::to_string(history, buf);
    DPRINTF(DecoupleBP,
            "StreamUBTB::update: fsq id: %d, control_pc: %#x, target: %#x, "
            "is_conditional: %d, is_indirect: %d, "
            "actually_taken: %d, history: %s, control size: %u\n",
            fsq_id,
            control_pc,
            target,
            is_conditional,
            is_indirect,
            actually_taken,
            buf.c_str(),
            control_size);



    auto tag = makePCHistTag(stream_start_pc, history);
    auto it = ubtb.find(tag);
    // if the tag is not found and the table is full
    bool new_entry = it == ubtb.end();
    //important:the size of ubtb is same as the size of mruList
    if (new_entry) {//free a new entry to the new entry
        std::pop_heap(mruList.begin(), mruList.end(), older());
        const auto& ubtb_entry = mruList.back();
        DPRINTF(DecoupleBP,
                "StreamUBTB::update: pop ubtb_entry: %#x\n",
                ubtb_entry->first);
        ubtb.erase(ubtb_entry->first);
    }
    //mydo:seen but not predicted correctly
    if (!new_entry) {
        if (ubtb[tag].nextStream != target) {
            ++ubtbStats.compulsoryMisses;
        }
    }
    //endmydo

    //update the entry
    ubtb[tag].tick = curTick();
    ubtb[tag].bbStart = stream_start_pc;
    ubtb[tag].bbEnd = control_pc;
    ubtb[tag].length = control_pc - stream_start_pc + control_size;
    ubtb[tag].nextStream = target;

    if (new_entry) {
        auto it = ubtb.find(tag);
        mruList.back() = it;
        std::push_heap(mruList.begin(), mruList.end(), older());
    }

    //mydo:never seen
    if (fullHist.find(stream_start_pc) == fullHist.end()) {
        ++ubtbStats.coldMisses;
        fullHist[stream_start_pc] = true;
    }
    else if (new_entry) {//seen but limited by capacity
        ++ubtbStats.capacityMisses;
    }
    //endmydo

    DPRINTF(
        DecoupleBP,
        "StreamUBTB:: %s ubtb_entry, tag: %#lx:  %#lx-%u-[%#lx, %#lx) --> %#lx \n",
        new_entry ? "Insert new" : "update",
        tag,
        ubtb[tag].bbStart,
        ubtb[tag].length,
        control_pc,
        control_pc + control_size,
        ubtb[tag].nextStream);


    // Because fetch has been redirected, here we must make another prediction
}

//get the tag from pc
uint64_t
StreamUBTB::makePCHistTag(Addr pc, const boost::dynamic_bitset<> &history) {
    auto hist = history;
    hist.resize(64);
    return pc ^ history.to_ulong();
}

}  // namespace branch_prediction

}  // namespace gem5
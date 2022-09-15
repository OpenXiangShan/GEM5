#include "cpu/pred/ubtb.hh"

#include "base/trace.hh"
#include "debug/DecoupleBP.hh"

namespace gem5 {

namespace branch_prediction {

StreamUBTB::StreamUBTB(const Params& p):
    TimedPredictor(p),
    size(32),
    ubtbStats(*this),
    loMask(64, (-1UL)),
    historyLen(128), // todo: read it from params
    usedBits(64), // todo: read it from params
    usedMask(usedBits, (-1UL))
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
    loMask.resize(historyLen);
    usedMask.resize(historyLen);
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
StreamUBTB::putPCHistory(Addr cur_chunk_start, Addr stream_start,
                         const boost::dynamic_bitset<> &history)
{
    auto tag = makePCHistTag(cur_chunk_start, history);
    DPRINTF(DecoupleBP,
            "Prediction request: stream start=%#lx, hash tag: %#lx\n", cur_chunk_start,
            tag);
    const auto &it = ubtb.find(tag);  // TODO: use hash of cur_chunk_start and history
    if (it == ubtb.end()) {
        DPRINTF(DecoupleBP,
                "Tag not found for stream=%#lx, guess an unlimited stream\n",
                cur_chunk_start);
        prediction.valid = false;
        prediction.history = history;

    } else {
        DPRINTF(DecoupleBP, "UBTB Entry found\n");
        prediction.valid = true;
        prediction.bbStart = cur_chunk_start;
        prediction.controlAddr = it->second.controlAddr;
        prediction.controlSize = it->second.controlSize;
        prediction.nextStream = it->second.nextStream;
        prediction.endType = it->second.endType;
        prediction.history = history;

        it->second.tick = curTick();
        std::make_heap(mruList.begin(), mruList.end(), older());
    }
}

StreamPrediction
StreamUBTB::getStream() {
    if (prediction.valid) {
        DPRINTF(DecoupleBP,
                "Response stream prediction: %#lx->%#lx\n",
                prediction.bbStart,
                prediction.controlAddr);
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
    // DPRINTF(DecoupleBP,
    //         "StreamUBTB::update: fsq id: %d, control_pc: %#x, target: %#x, "
    //         "is_conditional: %d, is_indirect: %d, "
    //         "actually_taken: %d, history: %s, control size: %u\n",
    //         fsq_id,
    //         control_pc,
    //         target,
    //         is_conditional,
    //         is_indirect,
    //         actually_taken,
    //         buf.c_str(),
    //         control_size);

    auto tag = makePCHistTag(stream_start_pc, history);
    DPRINTF(DecoupleBP, "Update stream start=%#lx, hash tag: %#lx\n",
            stream_start_pc, tag);
    auto it = ubtb.find(tag);
    // if the tag is not found and the table is full
    bool new_entry = it == ubtb.end();
    //important:the size of ubtb is same as the size of mruList
    if (new_entry) {//free a new entry to the new entry
        std::pop_heap(mruList.begin(), mruList.end(), older());
        const auto& ubtb_entry = mruList.back();
        DPRINTF(DecoupleBP,
                "StreamUBTB::update: pop ubtb_entry: %#x, tick: %lu\n",
                ubtb_entry->first, ubtb_entry->second.tick);
        ubtb.erase(ubtb_entry->first);
    }


    if (new_entry) {
        // insert entry
        ubtb[tag].tick = curTick();
        ubtb[tag].bbStart = stream_start_pc;
        ubtb[tag].controlAddr = control_pc;
        ubtb[tag].controlSize = control_size;
        ubtb[tag].nextStream = target;
        ubtb[tag].hysteresis = 1;
    } else {
        // decide whether or not update the entry
        if (ubtb[tag].controlAddr != control_pc &&
            ubtb[tag].nextStream != target) {
            ubtb[tag].hysteresis =
                std::max((int)0, (int)ubtb[tag].hysteresis - 1);
            if (ubtb[tag].hysteresis > 0) {
                DPRINTF(DecoupleBP, "Hysteresis: %d > 0, will not update\n",
                        ubtb[tag].hysteresis);
            } else {
                DPRINTF(DecoupleBP, "Hysteresis: %d <= 0, will update\n",
                        ubtb[tag].hysteresis);
                ubtb[tag].tick = curTick();
                ubtb[tag].bbStart = stream_start_pc;
                ubtb[tag].controlAddr = control_pc;
                ubtb[tag].controlSize = control_size;
                ubtb[tag].nextStream = target;
                ubtb[tag].hysteresis = 0;
            }
        }
    }

    if (new_entry) {
        auto it = ubtb.find(tag);
        mruList.back() = it;
        std::push_heap(mruList.begin(), mruList.end(), older());
    }

    auto fit = fullHist.find(tag);
    if (fit == fullHist.end()) {  // never seen
        ++ubtbStats.coldMisses;
        fullHist[tag].streamStart = stream_start_pc;
        fullHist[tag].branchAddr = control_pc;
        fullHist[tag].targetAddr = target;

    } else if (fit->second.streamStart == stream_start_pc &&
               fit->second.branchAddr == control_pc &&
               fit->second.targetAddr == target) {
        // seen but limited by capacity
        ++ubtbStats.capacityMisses;
    } else {
        ++ubtbStats.compulsoryMisses;
        DPRINTF(DecoupleBP,
                "For tag %#lx, p start: %#lx, p br: %#lx, p target: %#lx, "
                "t start: %#lx, t br: %#lx, t target: %#lx\n",
                tag, fit->second.streamStart, fit->second.branchAddr,
                fit->second.targetAddr, stream_start_pc, control_pc, target);
        fit->second.streamStart = stream_start_pc;
        fit->second.branchAddr = control_pc;
        fit->second.targetAddr = target;
    }

    DPRINTF(
        DecoupleBP,
        "StreamUBTB:: %s ubtb_entry, tag: %#lx:  %#lx-%u-[%#lx, %#lx) --> %#lx \n",
        new_entry ? "Insert new" : "update",
        tag,
        ubtb[tag].bbStart,
        control_pc - ubtb[tag].bbStart + control_size,
        control_pc,
        control_pc + control_size,
        ubtb[tag].nextStream);

    // Because fetch has been redirected, here we must make another prediction
}


void
StreamUBTB::commit(const FetchStreamId pred_id, Addr stream_start_pc,
                   Addr control_pc, Addr target, unsigned control_size,
                   const boost::dynamic_bitset<> &history)
{
    auto tag = makePCHistTag(stream_start_pc, history);
    auto it = ubtb.find(tag);
    if (it == ubtb.end()) {
        DPRINTF(DecoupleBP, "Tag %#lx (to commit) not found\n", tag);
        return;
    }
    if (it->second.bbStart == stream_start_pc &&
        it->second.controlAddr == control_pc &&
        it->second.nextStream == target) {
        it->second.hysteresis = std::min(2, it->second.hysteresis + 1);
        DPRINTF(DecoupleBP,
                "Confirm prediction for stream start %#lx hysteresis: %d\n",
                stream_start_pc, it->second.hysteresis);
        return;
    } else {
        DPRINTF(DecoupleBP,
                "Confirming prediction opposes current prediction for stream "
                "start %#lx\n",
                stream_start_pc);
    }
}

//get the tag from pc
uint64_t
StreamUBTB::makePCHistTag(Addr pc, const boost::dynamic_bitset<> &history) {
    DPRINTF(DecoupleBP, "History size %lu\n", history.size());
    Addr hash = pc;

    // xor 64+64 -> 64
    // Addr hi = (history >> 64).to_ulong();
    // boost::dynamic_bitset<> lo_bits(history & loMask);
    // lo_bits.resize(64);
    // Addr lo = lo_bits.to_ulong();
    // hash ^= hi;
    // hash ^= lo;

    // xor used bits to pc
    // boost::dynamic_bitset<> used_bits(history & usedMask);
    // used_bits.resize(usedBits);
    // Addr used = used_bits.to_ulong();
    // hash ^= used;

    // std::string buf;
    // boost::to_string(history, buf);
    // DPRINTF(DecoupleBP, "PC to hash: %#lx, hash: %#lx, history: %s\n", pc,
    //         hash, buf.c_str());
    return hash;
}

}  // namespace branch_prediction

}  // namespace gem5

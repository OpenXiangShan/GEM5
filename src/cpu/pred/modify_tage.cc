#include "cpu/pred/modify_tage.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/DecoupleBP.hh"

namespace gem5 {

namespace branch_prediction {

StreamTAGE::StreamTAGE(const Params& p):
    TimedPredictor(p),
    numPredictors(p.numPredictors),
    simpleBTBSize(p.simpleBTBSize),
    tableSizes(p.tableSizes),
    TTagBitSizes(p.TTagBitSizes),
    TTagPcShifts(p.TTagPcShifts),
    histLengths(p.histLengths),
    dbpstats(this)
{
    base_predictor.resize(simpleBTBSize);
    for (int i = 0;i < simpleBTBSize;i++)
        base_predictor_valid.push_back(false);
    targetCache.resize(numPredictors);
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        targetCache[i].resize(tableSizes[i]);
    }
}

StreamTAGE::DBPStats::DBPStats(statistics::Group* parent):
    statistics::Group(parent),
    ADD_STAT(coldMisses, statistics::units::Count::get(), "ittage the provider component lookup hit"),
    ADD_STAT(capacityMisses, statistics::units::Count::get(), "ittage the alternate prediction lookup hit"),
    ADD_STAT(compulsoryMisses, statistics::units::Count::get(), "ittage the provider component pred hit")
{
    coldMisses.prereq(coldMisses);
    capacityMisses.prereq(capacityMisses);
    compulsoryMisses.prereq(compulsoryMisses);
}

void
StreamTAGE::tickStart()
{
    prediction.valid = false;
}

void
StreamTAGE::tick() {}

bool
StreamTAGE::lookup_helper(bool flag, Addr streamStart, const bitset& history, TickedStreamStorage& target,
                          TickedStreamStorage& alt_target, int& predictor,
                          int& predictor_index, int& alt_predictor,
                          int& alt_predictor_index, int& pred_count,
                          bool& use_alt_pred)
{
    int pred_counts = 0;
    TickedStreamStorage target_1, target_2;
    int predictor_1 = 0;
    int predictor_2 = 0;
    int predictor_index_1 = 0;
    int predictor_index_2 = 0;

    for (int i = numPredictors - 1; i >= 0; --i) {
        uint32_t index = getAddrFold(streamStart, i);
        uint32_t tmp_index = index;
        uint32_t tmp_tag = getTag(streamStart, history, i);
        const auto &way = targetCache[i][tmp_index];
        if (way.tag == tmp_tag && way.valid && (streamStart >= way.target.bbStart && streamStart <= way.target.controlAddr)) {
            if (pred_counts == 0) {//第一次命中
                target_1 = way.target;
                predictor_1 = i;
                predictor_index_1 = tmp_index;
                ++pred_counts;
            }
            if (pred_counts == 1) {//第二次命中
                target_2 = way.target;
                predictor_2 = i;
                predictor_index_2 = tmp_index;
                ++pred_counts;
                break;
            }
        } else {

        }

        if (flag == false) {
            if (way.tag == tmp_tag && !way.valid)
                dbpstats.compulsoryMisses++;
            else if (way.tag == tmp_tag && way.valid && !(streamStart >= way.target.bbStart && streamStart <= way.target.controlAddr))
                dbpstats.capacityMisses++;
        }
    }
    pred_count = pred_counts;
    if (pred_counts > 0) {
        const auto& way1 = targetCache[predictor_1][predictor_index_1];
        const auto& way2 = targetCache[predictor_2][predictor_index_2];
        if ((way1.counter == 1) && (way1.useful == 0) && (pred_counts == 2) && (way2.counter > 0)) {
            use_alt_pred = true;
        } else {
            use_alt_pred = false;
        }
        target = target_1;
        if (use_alt_pred) {
            alt_target = target_2;
        }
        predictor = predictor_1;
        predictor_index = predictor_index_1;
        alt_predictor = predictor_2;
        alt_predictor_index = predictor_index_2;
        pred_count = pred_counts;
        return true;
    } else {
        use_alt_pred = false;
        target = base_predictor[(streamStart ^ previous_target.bbStart) % simpleBTBSize];
        // no need to set
        pred_count = pred_counts;
        if (base_predictor_valid[(streamStart ^ previous_target.bbStart) % simpleBTBSize] && streamStart >= target.bbStart && streamStart <= target.controlAddr) {
            DPRINTF(DecoupleBP, "%lx, found entry in base predictor, streamStart: %lx, controlAddr: %lx, nextStream: %lx\n", streamStart, target.bbStart, target.controlAddr, target.nextStream);
            return true;
        }
    }
    // may not reach here
    return false;
}

void
StreamTAGE::putPCHistory(Addr pc, const bitset &history) {
    TickedStreamStorage target;
    TickedStreamStorage alt_target;
    int predictor = 0;
    int predictor_index = 0;
    int alt_predictor = 0;
    int alt_predictor_index = 0;
    int pred_count = 0; // no use
    bool use_alt_pred = false;
    bool found = lookup_helper(false, pc, history, target, alt_target, predictor, predictor_index,
                  alt_predictor, alt_predictor_index, pred_count, use_alt_pred);
    if (use_alt_pred) {
        target = alt_target;
        predictor_index = alt_predictor_index;
        predictor = alt_predictor;
    }

    auto& way = targetCache[predictor][predictor_index];
    DPRINTF(DecoupleBP, "valid: %d, bbStart : %lx, controlAddr: %lx, nextStream: %lx\n", way.valid, pc, way.target.controlAddr, way.target.nextStream);
    if (!found) {
        DPRINTF(DecoupleBP,
                "not found for stream=%#lx, guess an unlimited stream\n",
                pc);
        prediction.valid = false;
        prediction.history = history;

    } else {
        DPRINTF(DecoupleBP, "Entry found\n");
        prediction.valid = true;
        prediction.bbStart = pc;
        prediction.controlAddr = target.controlAddr;
        prediction.controlSize = target.controlSize;
        prediction.nextStream = target.nextStream;
        prediction.endIsRet = target.endIsRet;
        prediction.history = history;

        way.target.tick = curTick();
    }
}

StreamPrediction
StreamTAGE::getStream() {
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

bool
StreamTAGE::equals(TickedStreamStorage t, Addr stream_start_pc, Addr control_pc, Addr target) {
    return (t.bbStart == stream_start_pc) && (t.controlAddr == control_pc) && (t.nextStream == target);
}

void
StreamTAGE::update(Addr stream_start_pc,
                   Addr control_pc, Addr target,
                   unsigned control_size,
                   bool actually_taken,
                   const bitset &history) {
    if (control_pc < stream_start_pc) {
        DPRINTF(DecoupleBP,
                "Control PC %#lx is before stream start %#lx, ignore it\n",
                control_pc,
                stream_start_pc);
        return;
    }

    TickedStreamStorage target_1;
    TickedStreamStorage target_2;
    TickedStreamStorage target_sel;
    int predictor = 0;
    int predictor_index = 0;
    int alt_predictor = 0;
    int alt_predictor_index = 0;
    int pred_count = 0; // no use
    int predictor_sel = 0;
    int predictor_index_sel = 0;
    bool use_alt_pred = false;
    bool predictor_found = lookup_helper(true, stream_start_pc, history, target_1, target_2, predictor, predictor_index,
                                         alt_predictor, alt_predictor_index, pred_count, use_alt_pred);
    if (predictor_found && use_alt_pred) {
        target_sel = target_2;
        predictor_sel = alt_predictor;
        predictor_index_sel = alt_predictor_index;
    } else if (predictor_found) {
        target_sel = target_1;
        predictor_sel = predictor;
        predictor_index_sel = predictor_index;
    } else {
        predictor_sel = predictor;
        predictor_index_sel = predictor_index;
    }

    previous_target.tick = curTick();
    previous_target.bbStart = stream_start_pc;
    previous_target.controlAddr = control_pc;
    previous_target.controlSize = control_size;
    previous_target.nextStream = target;
    previous_target.hysteresis = 1;

    base_predictor[(stream_start_pc ^ previous_target.bbStart) %simpleBTBSize].tick = curTick();
    base_predictor[(stream_start_pc ^ previous_target.bbStart) %simpleBTBSize].bbStart = stream_start_pc;
    base_predictor[(stream_start_pc ^ previous_target.bbStart) %simpleBTBSize].controlAddr = control_pc;
    base_predictor[(stream_start_pc ^ previous_target.bbStart) %simpleBTBSize].controlSize = control_size;
    base_predictor[(stream_start_pc ^ previous_target.bbStart) %simpleBTBSize].nextStream = target;
    base_predictor[(stream_start_pc^ previous_target.bbStart) %simpleBTBSize].hysteresis = 1;
    base_predictor_valid[(stream_start_pc ^ previous_target.bbStart) %simpleBTBSize] = true;

    bool allocate_values = true;

    auto& way_sel = targetCache[predictor_sel][predictor_index_sel];

    if (pred_count > 0 && equals(target_sel, stream_start_pc, control_pc, target)) {//pred hit
        // the prediction was from predictor tables and correct
        // increment the counter
        if (way_sel.counter <= 2) {
            ++way_sel.counter;
            ++way_sel.target.hysteresis;
        }
    } else {
        // a misprediction
        auto& way1 = targetCache[predictor][predictor_index];
        auto& way2 = targetCache[alt_predictor][alt_predictor_index];
        if (pred_count > 0) {
            if (equals(way1.target, stream_start_pc, control_pc, target) &&
                pred_count == 2 &&
                !equals(way2.target, stream_start_pc, control_pc, target)) {
                way1.useful = 1;
                allocate_values = false;
                if (use_alt > 0) {
                    --use_alt;
                }
            }
            if (!equals(way1.target, stream_start_pc, control_pc, target) &&
                pred_count == 2 &&
                equals(way2.target, stream_start_pc, control_pc, target)) {
                // if pred was wrong and alt_pred was right
                if (use_alt < 15) {
                    ++use_alt;
                }
            }
            // if counter > 0 then decrement, else replace

            if (way_sel.counter > 0) {
                --way_sel.counter;
                --way_sel.target.hysteresis;
            } else {
                way_sel.target.tick = curTick();
                way_sel.target.bbStart = stream_start_pc;
                way_sel.target.controlAddr = control_pc;
                way_sel.target.controlSize = control_size;
                way_sel.target.nextStream = target;
                way_sel.target.hysteresis = 1;
                way_sel.tag =
                    getTag(stream_start_pc,
                           history,
                           predictor_sel);
                way_sel.counter = 1;
                way_sel.useful = 0;
            }
        }
        //update the tag
        if (pred_count == 0 || allocate_values) {
            int allocated = 0;
            uint32_t start_pos;
            if (pred_count > 0){
                start_pos = predictor_sel + 1;
            } else {
                start_pos = 0;
            }
            for (; start_pos < numPredictors; ++start_pos) {
                uint32_t new_index = getAddrFold(stream_start_pc, start_pos);
                auto& way_new = targetCache[start_pos][new_index];
                if (way_new.useful == 0) {
                    if (reset_counter < 255) reset_counter++;
                    way_new.valid = true;
                    way_new.target.tick = curTick();
                    way_new.target.bbStart = stream_start_pc;
                    way_new.target.controlAddr = control_pc;
                    way_new.target.controlSize = control_size;
                    way_new.target.nextStream = target;
                    way_new.target.hysteresis = 1;
                    way_new.tag =
                        getTag(stream_start_pc,
                               history,
                               start_pos);
                    way_new.counter = 1;
                    ++allocated;
                    ++start_pos; // do not allocate on consecutive predictors
                    if (allocated == 2) {
                        break;
                    }
                } else {
                    // reset useful bits
                    if (reset_counter > 0) {
                        --reset_counter;
                    }
                    if (reset_counter == 0) {
                        for (int i = 0; i < numPredictors; ++i) {
                            for (int j = 0; j < tableSizes[i]; ++j) {
                                targetCache[i][j].useful = 0;
                            }
                        }
                        reset_counter = 128;
                    }
                }
            }
        }
    }
}

void
StreamTAGE::commit(Addr stream_start_pc, Addr controlAddr, Addr target, bitset &history)
{
    for (int i = numPredictors - 1; i >= 0; --i) {
        uint32_t index = getAddrFold(stream_start_pc, i);
        uint32_t tmp_index = index;
        uint32_t tmp_tag = getTag(stream_start_pc, history, i);
        auto& way = targetCache[i][tmp_index];
        if (way.tag == tmp_tag &&
            way.target.bbStart == stream_start_pc &&
            way.target.controlAddr == controlAddr &&
            way.target.nextStream == target) {
            if (way.counter < 2)
                ++way.counter;
            if (way.target.hysteresis < 2) {
                ++way.target.hysteresis;
            }
            way.useful = 1;
            way.valid = true;
            break;
        }
    }
}

uint64_t
StreamTAGE::getTableGhrLen(int table) {
    return histLengths[table];
}

uint64_t
StreamTAGE::getAddrFold(uint64_t address, int table) {
    uint64_t folded_address, k;
    folded_address = 0;
    for (k = 0; k < 8; k++) {
        folded_address ^= address;
        address >>= 8;
    }
    return folded_address & ((1 << ceilLog2(tableSizes[table])) - 1);
}

uint64_t
StreamTAGE::getTag(Addr pc, const bitset& history, int table) {
    bitset temp = history;
    temp.resize(64);
    return ((pc >> TTagPcShifts[table]) ^ temp.to_ulong()) & ((1 << TTagBitSizes[table]) - 1);
}

}  // namespace branch_prediction

}  // namespace gem5

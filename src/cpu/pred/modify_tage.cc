#include "cpu/pred/modify_tage.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/DecoupleBP.hh"
#include "cpu/pred/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

StreamTAGE::StreamTAGE(const Params& p):
    TimedPredictor(p),
    numPredictors(p.numPredictors),
    baseTableSize(p.baseTableSize),
    tableSizes(p.tableSizes),
    tableTagBits(p.TTagBitSizes),
    tablePcShifts(p.TTagPcShifts),
    histLengths(p.histLengths),
    maxHistLen(p.maxHistLen),
    dbpstats(this),
    numTablesToAlloc(p.numTablesToAlloc)
{
    baseTable.resize(baseTableSize);
    for (auto &e: baseTable) {
        e.valid = false;
    }

    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    indexSegments.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    tagSegments.resize(numPredictors);
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        tageTable[i].resize(tableSizes[i]);

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(maxHistLen, true);
        tableIndexMasks[i] >>= (maxHistLen - tableIndexBits[i]);
        indexSegments[i] =
            ceil((float)histLengths[i] / (float)tableIndexBits[i]);

        tableTagMasks[i].resize(maxHistLen, true);
        tableTagMasks[i] >>= (maxHistLen - tableTagBits[i]);
        tagSegments[i] = ceil((float)histLengths[i] / (float)tableTagBits[i]);

        // indexCalcBuffer[i].resize(maxHistLen, false);
        // tagCalcBuffer[i].resize(maxHistLen, false);
    }
}

StreamTAGE::DBPStats::DBPStats(statistics::Group* parent):
    statistics::Group(parent),
    ADD_STAT(coldMisses, statistics::units::Count::get(), "ittage the provider component lookup hit"),
    ADD_STAT(capacityMisses, statistics::units::Count::get(), "ittage the alternate prediction lookup hit"),
    ADD_STAT(compulsoryMisses, statistics::units::Count::get(), "ittage the provider component pred hit"),
    ADD_STAT(providerTableDist, statistics::units::Count::get(), "the distribution of provider component")
{
    using namespace statistics;
    providerTableDist.init(0, 21, 1).flags(statistics::pdf);
}

void
StreamTAGE::tickStart()
{
    prediction.valid = false;
}

void
StreamTAGE::tick() {}

bool
StreamTAGE::lookupHelper(bool flag, Addr last_chunk_start, Addr stream_start,
                         const bitset& history, TickedStreamStorage& target,
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
        uint32_t tmp_index = getTageIndex(last_chunk_start, history, i);
        uint32_t tmp_tag = getTageTag(stream_start, history, i);
        const auto &way = tageTable[i][tmp_index];
        if (way.tag == tmp_tag && way.valid &&
            (last_chunk_start >= way.target.bbStart &&
             last_chunk_start <= way.target.controlAddr)) {
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
            if (way.tag == tmp_tag && !way.valid) {
                dbpstats.compulsoryMisses++;
            } else if (way.tag == tmp_tag && way.valid &&
                       !(last_chunk_start >= way.target.bbStart &&
                         last_chunk_start <= way.target.controlAddr)) {
                dbpstats.capacityMisses++;
            }
        }
    }
    pred_count = pred_counts;
    if (pred_counts > 0) {

        dbpstats.providerTableDist.sample(predictor_1);
        const auto& way1 = tageTable[predictor_1][predictor_index_1];
        const auto& way2 = tageTable[predictor_2][predictor_index_2];
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
        dbpstats.providerTableDist.sample(20U);
        auto base_table_idx = getBaseIndex(last_chunk_start);
        use_alt_pred = false;
        target = baseTable[base_table_idx];
        pred_count = pred_counts;
        if (target.valid &&
            last_chunk_start >= target.bbStart &&
            last_chunk_start <= target.controlAddr) {

            DPRINTF(DecoupleBP,
                     "%#lx, found entry in base predictor, streamStart: %#lx, "
                     "controlAddr: %#lx, nextStream: %#lx\n",
                     last_chunk_start, target.bbStart, target.controlAddr,
                     target.nextStream);
            return true;
        }
    }
    // may not reach here
    return false;
}

void
StreamTAGE::putPCHistory(Addr cur_chunk_start, Addr stream_start, const bitset &history) {
    TickedStreamStorage target;
    TickedStreamStorage alt_target;
    int predictor = 0;
    int predictor_index = 0;
    int alt_predictor = 0;
    int alt_predictor_index = 0;
    int pred_count = 0; // no use
    bool use_alt_pred = false;
    bool found =
        lookupHelper(false, cur_chunk_start, stream_start, history, target,
                     alt_target, predictor, predictor_index, alt_predictor,
                     alt_predictor_index, pred_count, use_alt_pred);
    if (use_alt_pred) {
        target = alt_target;
        predictor_index = alt_predictor_index;
        predictor = alt_predictor;
    }

    auto& way = tageTable[predictor][predictor_index];
    DPRINTF(DecoupleBP,
            "valid: %d, chunkStart: %#lx, streamStart: %#lx  controlAddr: "
            "%#lx, nextStream: %#lx\n",
            way.valid, cur_chunk_start, stream_start, way.target.controlAddr,
            way.target.nextStream);
    if (!found) {
        DPRINTF(DecoupleBP,
                "not found for stream=%#lx, guess an unlimited stream\n",
                cur_chunk_start);
        prediction.valid = false;
        prediction.history = history;
        prediction.endIsCall = false;
        prediction.endIsRet = false;

    } else {
        DPRINTF(DecoupleBP, "Entry found\n");
        prediction.valid = true;
        prediction.bbStart = stream_start;
        prediction.controlAddr = target.controlAddr;
        prediction.controlSize = target.controlSize;
        prediction.nextStream = target.nextStream;
        prediction.endIsCall = target.endIsCall;
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
StreamTAGE::equals(TickedStreamStorage& t, Addr stream_start_pc,
                   Addr control_pc, Addr target)
{
    return (t.bbStart == stream_start_pc) && (t.controlAddr == control_pc) &&
           (t.nextStream == target);
}

void
StreamTAGE::update(Addr last_chunk_start, Addr stream_start_pc,
                   Addr control_pc, Addr target,
                   unsigned control_size,
                   bool actually_taken,
                   const bitset &history, EndType endType) {
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
    bool predictor_found = lookupHelper(
        true, last_chunk_start, stream_start_pc, history, target_1,
        target_2, predictor, predictor_index, alt_predictor,
        alt_predictor_index, pred_count, use_alt_pred);
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
    previous_target.endIsCall = (endType == END_TYPE_CALL);
    previous_target.endIsRet = (endType == END_TYPE_RET);

    auto base_table_idx = getBaseIndex(stream_start_pc);
    baseTable[base_table_idx].set(
        curTick(), stream_start_pc, control_pc, target, control_size, 1,
        (endType == END_TYPE_CALL), (endType == END_TYPE_RET), true);

    bool allocate_values = true;

    auto& way_sel = tageTable[predictor_sel][predictor_index_sel];

    if (pred_count > 0 && equals(target_sel, stream_start_pc, control_pc, target)) {//pred hit
        // the prediction was from predictor tables and correct
        // increment the counter
        if (way_sel.counter <= 2) {
            ++way_sel.counter;
            ++way_sel.target.hysteresis;
        }
        way_sel.target.endIsCall = (endType == END_TYPE_CALL);
        way_sel.target.endIsRet = (endType == END_TYPE_RET);
    } else {
        // a misprediction
        auto& way1 = tageTable[predictor][predictor_index];
        auto& way2 = tageTable[alt_predictor][alt_predictor_index];
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
                way_sel.target.endIsCall = (endType == END_TYPE_CALL);
                way_sel.target.endIsRet = (endType == END_TYPE_RET);
                way_sel.tag =
                    getTageTag(stream_start_pc,
                           history,
                           predictor_sel);
                way_sel.counter = 1;
                way_sel.useful = 0;
            }
        }
        //update the tag
        if (pred_count == 0 || allocate_values) {
            int allocated = 0;
            uint32_t start_tage_table;
            if (pred_count > 0){
                start_tage_table = predictor_sel + 1;
            } else {
                start_tage_table = 0;
            }
            for (; start_tage_table < numPredictors; ++start_tage_table) {
                uint32_t new_index = getTageIndex(last_chunk_start, history, start_tage_table);
                auto& way_new = tageTable[start_tage_table][new_index];
                if (way_new.useful == 0) {
<<<<<<< HEAD
=======
                    DPRINTFV(this->debugFlagOn,
                             "Allocated in table %d index[%d], histlen=%u\n",
                             start_tage_table, new_index, histLengths[start_tage_table]);
>>>>>>> b88b180ba (Predict with chunk start pc for long streams)
                    if (reset_counter < 255) reset_counter++;
                    way_new.valid = true;
                    way_new.target.tick = curTick();
                    way_new.target.bbStart = stream_start_pc;
                    way_new.target.controlAddr = control_pc;
                    way_new.target.controlSize = control_size;
                    way_new.target.nextStream = target;
                    way_new.target.hysteresis = 1;
                    way_new.target.endIsCall = (endType == END_TYPE_CALL);
                    way_new.target.endIsRet = (endType == END_TYPE_RET);
                    way_new.tag =
                        getTageTag(stream_start_pc,
                               history,
                               start_tage_table);
                    way_new.counter = 1;
                    ++allocated;
                    ++start_tage_table; // do not allocate on consecutive predictors
                    if (allocated == numTablesToAlloc) {
                        break;
                    }
                } else {
<<<<<<< HEAD
=======
                    DPRINTFV(this->debugFlagOn,
                             "Table %d index[%d] histlen=%u is useful\n",
                             start_tage_table, new_index, histLengths[start_tage_table]);
>>>>>>> b88b180ba (Predict with chunk start pc for long streams)
                    // reset useful bits
                    if (reset_counter > 0) {
                        --reset_counter;
                    }
                    if (reset_counter == 0) {
                        for (int i = 0; i < numPredictors; ++i) {
                            for (int j = 0; j < tableSizes[i]; ++j) {
                                tageTable[i][j].useful = 0;
                            }
                        }
                        DPRINTF(DecoupleBP, "Resetting all useful bits");
                        reset_counter = 128;
                    }
                }
            }
        }
    }
    debugFlagOn = false;
}

void
StreamTAGE::commit(Addr stream_start_pc, Addr controlAddr, Addr target, bitset &history)
{
    for (int i = numPredictors - 1; i >= 0; --i) {
        uint32_t tmp_index = getTageIndex(
            computeLastChunkStart(controlAddr, stream_start_pc), history, i);
        uint32_t tmp_tag = getTageTag(stream_start_pc, history, i);
        auto& way = tageTable[i][tmp_index];
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

Addr
StreamTAGE::getTageTag(Addr pc, const bitset& history, int t)
{
    bitset buf(tableTagBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf.resize(maxHistLen);
    bitset hist(history);  // copy a writable history
    hist.resize(histLengths[t]);
    hist.resize(maxHistLen);
    assert(history.size() == buf.size());
    for (unsigned i = 0; i < tagSegments[t]; i++) {
        assert(history.size() == tableTagMasks[t].size());
        auto masked = hist & tableTagMasks[t];
        buf ^= masked;  // fold into the buf
        hist >>= tableTagBits[t];  // shift right to get next fold
    }
    return buf.to_ulong();
}

Addr
StreamTAGE::getTageIndex(Addr pc, const bitset& history, int t)
{
    bitset buf(tableIndexBits[t], pc >> tablePcShifts[t]);
    buf.resize(maxHistLen);
    bitset hist(history);  // copy a writable history
    hist.resize(histLengths[t]);
    hist.resize(maxHistLen);
    for (unsigned i = 0; i < indexSegments[t]; i++) {
        assert(history.size() == tableIndexMasks[t].size());
        auto masked = hist & tableIndexMasks[t];
        buf ^= masked;  // fold into the buf
        hist >>= tableIndexBits[t];  // shift right to get next fold
    }
    return buf.to_ulong();
}

Addr
StreamTAGE::getBaseIndex(Addr pc)
{
    return (pc >> 1) % baseTableSize;
}


}  // namespace branch_prediction

}  // namespace gem5

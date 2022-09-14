#include "cpu/pred/modify_tage.hh"

#include <algorithm>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPUseful.hh"
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
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(maxHistLen, true);
        tableIndexMasks[i] >>= (maxHistLen - tableIndexBits[i]);

        assert(histLengths.size() >= numPredictors);
        indexSegments[i] =
            ceil((float)histLengths[i] / (float)tableIndexBits[i]);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(maxHistLen, true);
        tableTagMasks[i] >>= (maxHistLen - tableTagBits[i]);
        tagSegments[i] = ceil((float)histLengths[i] / (float)tableTagBits[i]);

        assert(tablePcShifts.size() >= numPredictors);
        // indexCalcBuffer[i].resize(maxHistLen, false);
        // tagCalcBuffer[i].resize(maxHistLen, false);
    }
    usefulResetCounter = 128;
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
                         int& alt_predictor_index, int& tage_pred_count,
                         bool& use_alt_pred)
{
    int tage_pred_counts = 0;
    TickedStreamStorage target_1, target_2;
    int predictor_1 = 0;
    int predictor_2 = 0;
    int predictor_index_1 = 0;
    int predictor_index_2 = 0;

    for (int i = numPredictors - 1; i >= 0; --i) {
        uint32_t tmp_index = getTageIndex(last_chunk_start, history, i);
        uint32_t tmp_tag = getTageTag(stream_start, history, i);
        const auto &way = tageTable[i][tmp_index];
        DPRINTF(DecoupleBP || debugFlagOn,
                "TAGE table[%u] index[%u], valid: %i, expected tag: %#lx, "
                "found tag: %#lx, match: %i, useful: %i\n",
                i, tmp_index, way.valid, tmp_tag, way.tag, way.tag == tmp_tag, way.useful);
        
        bool match = way.tag == tmp_tag && way.valid;
        bool sane = (last_chunk_start >= way.target.bbStart) &&
                    ((way.target.endNotTaken &&
                      last_chunk_start <
                          way.target.controlAddr + way.target.controlSize) ||
                     (!way.target.endNotTaken &&
                      last_chunk_start <= way.target.controlAddr));
        if (match && sane) {
            if (tage_pred_counts == 0) {//第一次命中
                target_1 = way.target;
                predictor_1 = i;
                predictor_index_1 = tmp_index;
                ++tage_pred_counts;
            }
            if (tage_pred_counts == 1) {//第二次命中
                target_2 = way.target;
                predictor_2 = i;
                predictor_index_2 = tmp_index;
                ++tage_pred_counts;
                break;
            }
            DPRINTFR(DecoupleBP || debugFlagOn, ", is usable\n");
        } else if (!match) {
            DPRINTFR(DecoupleBP || debugFlagOn, ", but exclude caz tag mismatch\n");
        } else {
            DPRINTFR(DecoupleBP || debugFlagOn,
                    ", but exclude because chunk start %#lx not in stream %#lx-%#lx\n",
                    last_chunk_start, way.target.bbStart,
                    way.target.controlAddr);
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
    tage_pred_count = tage_pred_counts;
    if (tage_pred_counts > 0) {
        DPRINTFV(debugFlagOn || ::gem5::debug::DecoupleBP,
                 "Select predictor %d, index %d\n", predictor_1,
                 predictor_index_1);
        DPRINTFV(debugFlagOn || ::gem5::debug::DecoupleBP,
                 "Select predictor %d, index %d\n", predictor_2,
                 predictor_index_2);
        dbpstats.providerTableDist.sample(predictor_1);
        const auto& way1 = tageTable[predictor_1][predictor_index_1];
        const auto& way2 = tageTable[predictor_2][predictor_index_2];
        if ((way1.target.hysteresis == 1) && (way1.useful == 0) &&
            (tage_pred_counts == 2) && (way2.target.hysteresis > 0)) {
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
        tage_pred_count = tage_pred_counts;
        return true;
    } else {
        dbpstats.providerTableDist.sample(20U);
        auto base_table_idx = getBaseIndex(last_chunk_start);
        use_alt_pred = false;
        target = baseTable[base_table_idx];
        tage_pred_count = tage_pred_counts;
        if (target.valid &&
            last_chunk_start >= target.bbStart &&
            last_chunk_start <= target.controlAddr) {

            DPRINTF(
                DecoupleBP || debugFlagOn,
                "%#lx, found entry in base predictor[%lu], streamStart: %#lx, "
                "controlAddr: %#lx, nextStream: %#lx\n",
                last_chunk_start, base_table_idx, target.bbStart, target.controlAddr,
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

    if (stream_start == ObservingPC) {
        debugFlagOn = true;
        DPRINTFV(true, "Predict for stream %#lx, chunk: %#lx\n", stream_start,
                 cur_chunk_start);
    }

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
    DPRINTF(DecoupleBP || debugFlagOn,
            "found: %d, valid: %d, chunkStart: %#lx, streamStart: %#lx  controlAddr: "
            "%#lx, nextStream: %#lx\n",
            found, way.valid, cur_chunk_start, stream_start, way.target.controlAddr,
            way.target.nextStream);
    if (!found) {
        DPRINTF(DecoupleBP || debugFlagOn,
                "not found for stream=%#lx, chunk=%#lx, guess an unlimited stream\n",
                stream_start, cur_chunk_start);
        prediction.valid = false;
        prediction.history = history;
        prediction.endType = END_NONE;
        prediction.endNotTaken = true;

    } else {
        DPRINTF(DecoupleBP || debugFlagOn,
                "Entry found in predictor %i: %#lx->%#lx, taken: %i\n", predictor,
                target.controlAddr, target.nextStream, !target.endNotTaken);
        prediction.valid = true;
        prediction.bbStart = stream_start;
        prediction.controlAddr = target.controlAddr;
        prediction.controlSize = target.controlSize;
        prediction.nextStream = target.nextStream;
        prediction.endType = target.endType;
        prediction.endNotTaken = target.endNotTaken;
        prediction.history = history;

        way.target.tick = curTick();
    }
    debugFlagOn = false;
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
StreamTAGE::equals(const TickedStreamStorage& t, Addr stream_start_pc,
                   Addr control_pc, Addr target)
{
    return (t.bbStart == stream_start_pc) && (t.controlAddr == control_pc) &&
           (t.nextStream == target);
}

void
StreamTAGE::update(Addr last_chunk_start, Addr stream_start_pc,
                   Addr control_pc, Addr target, unsigned control_size,
                   bool actually_taken, const bitset& history, EndType end_type)
{
    if (stream_start_pc == ObservingPC) {
        debugFlagOn = true;
        DPRINTFV(true, "Update for stream %#lx, chunk:%#lx\n", stream_start_pc,
                 last_chunk_start);
    }
    if (actually_taken && (control_pc < stream_start_pc)) {
        DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
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
    int nonbase_pred_count = 0;
    int predictor_sel = 0;
    int predictor_index_sel = 0;
    bool use_alt_pred = false;
    bool predictor_found = lookupHelper(
        true, last_chunk_start, stream_start_pc, history, target_1,
        target_2, predictor, predictor_index, alt_predictor,
        alt_predictor_index, nonbase_pred_count, use_alt_pred);
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

    if (nonbase_pred_count == 0) {
        // only need to update predictor when there is no
        // TAGE provider found
        auto &entry = baseTable[getBaseIndex(last_chunk_start)];
        if (entry.valid && entry.hysteresis >= 1) {
            // dec conf
            entry.hysteresis--;
            DPRINTF(
                DecoupleBP || debugFlagOn,
                "Dec conf of baseTable[%u] that %s at [%#lx, %#lx) -> %#lx\n",
                getBaseIndex(last_chunk_start),
                !entry.endNotTaken ? "taken" : "NT", entry.controlAddr,
                entry.controlAddr + entry.controlSize, entry.nextStream);
        } else {
            // replace it
            DPRINTF(DecoupleBP || debugFlagOn,
                    "Set baseTable[%u] to %s at [%#lx, %#lx) -> %#lx\n",
                    getBaseIndex(last_chunk_start), actually_taken ? "taken": "NT", control_pc,
                    control_pc + control_size, target);
            entry.set(curTick(), stream_start_pc, control_pc, target,
                      control_size, 1, end_type, true);
            entry.endNotTaken = !actually_taken;
        }
        DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                 "No TAGE provider found, only update base predictor\n");
    }

    bool allocate_values = true;

    auto& way_sel = tageTable[predictor_sel][predictor_index_sel];

    if (nonbase_pred_count > 0 && equals(target_sel, stream_start_pc, control_pc, target)) {//pred hit
        // the prediction was from predictor tables and correct
        // increment the counter
        if (way_sel.target.endNotTaken == !actually_taken) {
            if (way_sel.target.hysteresis <= 2) {
                ++way_sel.target.hysteresis;
            }
            way_sel.target.endType = end_type;
            way_sel.useful = 1;
            DPRINTF(DecoupleBP || this->debugFlagOn,
                     "Predict correctly, inc conf for TAGE table %lu, mark as useful\n",
                     predictor_sel);
        } else {
            if (way_sel.target.hysteresis > 1) {
                way_sel.target.hysteresis -= 1;
                DPRINTF(DecoupleBP || this->debugFlagOn,
                         "Target is correct, but direction is wrong, dec "
                         "conf to %u for TAGE table %lu\n",
                         way_sel.target.hysteresis, predictor_sel);
            } else {
                way_sel.target.hysteresis = 1;
                way_sel.target.endNotTaken = !actually_taken;
                DPRINTF(DecoupleBP || this->debugFlagOn,
                         "Target is correct, but direction is wrong, "
                         "switch it to %s\n",
                         way_sel.target.endNotTaken ? "not taken" : "taken");
            }
        }
    } else {
        // a misprediction
        auto& way1 = tageTable[predictor][predictor_index];
        auto& way2 = tageTable[alt_predictor][alt_predictor_index];
        if (nonbase_pred_count > 0) {
            if (equals(way1.target, stream_start_pc, control_pc, target) &&
                nonbase_pred_count == 2 &&
                !equals(way2.target, stream_start_pc, control_pc, target)) {
                way1.useful = 1;
                allocate_values = false;
                if (use_alt > 0) {
                    --use_alt;
                }
            }
            if (!equals(way1.target, stream_start_pc, control_pc, target) &&
                nonbase_pred_count == 2 &&
                equals(way2.target, stream_start_pc, control_pc, target)) {
                // if pred was wrong and alt_pred was right
                if (use_alt < 15) {
                    ++use_alt;
                }
            }
            // if counter > 0 then decrement, else replace
            if (way_sel.target.hysteresis > 0) {
                --way_sel.target.hysteresis;
                DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                         "Decrement conf to %d for predictor %d index %d\n",
                         way_sel.target.hysteresis, predictor_sel, predictor_index_sel);
            } else {
                DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                         "predictor %d index %d now conf=%d, replace it\n",
                         predictor_sel, predictor_index_sel, way_sel.target.hysteresis);

                way_sel.target.set(curTick(), stream_start_pc, control_pc,
                                   target, control_size, 1, end_type, true);
                way_sel.tag =
                    getTageTag(stream_start_pc, history, predictor_sel);

                way_sel.useful = 0;
                if (actually_taken) {
                    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                             "Change to taken at %#lx to %#lx\n", control_pc,
                             target);
                    way_sel.target.endNotTaken = false;
                } else {
                    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                             "Change to not taken\n");
                    way_sel.target.endNotTaken = true;
                }
            }
        }
        //update the tag
        if (nonbase_pred_count == 0 || allocate_values) {
            int allocated = 0;
            uint32_t start_tage_table;
            if (nonbase_pred_count > 0){
                start_tage_table = predictor_sel + 1;
            } else {
                start_tage_table = 0;
            }
            for (; start_tage_table < numPredictors; ++start_tage_table) {
                uint32_t new_index = getTageIndex(last_chunk_start, history, start_tage_table);
                auto& way_new = tageTable[start_tage_table][new_index];
                if (way_new.useful == 0) {
                    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                             "Allocated in table[%d] index[%d], histlen=%u\n",
                             start_tage_table, new_index, histLengths[start_tage_table]);

                    way_new.valid = true;
                    way_new.target.tick = curTick();
                    way_new.target.bbStart = stream_start_pc;
                    way_new.target.controlAddr = control_pc;
                    way_new.target.controlSize = control_size;
                    way_new.target.nextStream = target;
                    way_new.target.hysteresis = 1;
                    way_new.target.endType = end_type;
                    way_new.target.endNotTaken = !actually_taken;
                    way_new.tag =
                        getTageTag(stream_start_pc, history, start_tage_table);
                    way_new.target.hysteresis = 1;

                    ++allocated;
                    ++start_tage_table; // do not allocate on consecutive predictors
                    if (allocated == numTablesToAlloc) {
                        break;
                    }
                } else {
                    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
                             "Table %d index[%d] histlen=%u is useful\n",
                             start_tage_table, new_index, histLengths[start_tage_table]);
                    
                }
            }
            if (allocated) {
                if (usefulResetCounter < 255) {
                    usefulResetCounter++;
                }
                DPRINTF(DecoupleBPUseful,
                        "Succeed to allocate table[%u] for stream %#lx, "
                        "useful resetting counter now: %u\n",
                        start_tage_table - 1, stream_start_pc, usefulResetCounter);
            } else {
                if (usefulResetCounter > 0) {
                    --usefulResetCounter;
                }
                DPRINTF(DecoupleBPUseful,
                        "Failed to allocate for stream %#lx, "
                        "useful resetting counter now: %u\n",
                        stream_start_pc, usefulResetCounter);

                if (usefulResetCounter == 0) {
                    for (int i = 0; i < numPredictors; ++i) {
                        for (int j = 0; j < tableSizes[i]; ++j) {
                            tageTable[i][j].useful = 0;
                        }
                    }
                    DPRINTF(DecoupleBPUseful,
                            "Resetting all useful bits in stream TAGE\n");
                    warn("Resetting all useful bits in stream TAGE\n");
                    usefulResetCounter = 128;
                }
            }
        }
    }
    debugFlagOn = false;
}

void
StreamTAGE::commit(Addr stream_start_pc, Addr controlAddr, Addr target, bitset &history)
{
    if (stream_start_pc == ObservingPC) {
        debugFlagOn = true;
    }
    for (int i = numPredictors - 1; i >= 0; --i) {
        uint32_t tmp_index = getTageIndex(
            computeLastChunkStart(controlAddr, stream_start_pc), history, i);
        uint32_t tmp_tag = getTageTag(stream_start_pc, history, i);
        auto& way = tageTable[i][tmp_index];
        if (way.tag == tmp_tag &&
            way.target.bbStart == stream_start_pc &&
            way.target.controlAddr == controlAddr &&
            way.target.nextStream == target) {
            if (way.target.hysteresis < 2) {
                ++way.target.hysteresis;
            }
            DPRINTF(DecoupleBP || debugFlagOn,
                    "mark table[%lu] index[%lu] as useful\n", i, tmp_index);
            way.useful = 1;
            way.valid = true;
            break;
        }
    }
    debugFlagOn = false;
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

    DPRINTFV(this->debugFlagOn && ::gem5::debug::DecoupleBPVerbose,
             "Calc index: allocate a %u bit buf, using hist %s\n",
             tableIndexBits[t], hist);

    for (unsigned i = 0; i < indexSegments[t]; i++) {
        assert(history.size() == tableIndexMasks[t].size());
        auto masked = hist & tableIndexMasks[t];
        buf ^= masked;  // fold into the buf
        hist >>= tableIndexBits[t];  // shift right to get next fold
    }
    return buf.to_ulong();
}

Addr
StreamTAGE::getBaseIndex(Addr pc) const
{
    return (pc >> 1) % baseTableSize;
}


}  // namespace branch_prediction

}  // namespace gem5

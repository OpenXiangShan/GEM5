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
    tableSizes(p.tableSizes),
    tableTagBits(p.TTagBitSizes),
    tablePcShifts(p.TTagPcShifts),
    histLengths(p.histLengths),
    maxHistLen(p.maxHistLen),
    dbpstats(this),
    numTablesToAlloc(p.numTablesToAlloc)
{
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    indexSegments.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    tagSegments.resize(numPredictors);
    hasTag.resize(numPredictors);
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
        hasTag[i] = tableTagBits[i] > 0;

        assert(tablePcShifts.size() >= numPredictors);
        // indexCalcBuffer[i].resize(maxHistLen, false);
        // tagCalcBuffer[i].resize(maxHistLen, false);
    }
    useAlt.resize(altSelectorSize, 0);
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
                         const bitset& history, TickedStreamStorage* &main_target,
                         TickedStreamStorage* &alt_target, int& main_table,
                         int& main_table_index, int& alt_table,
                         int& alt_table_index, int& provider_counts,
                         bool& use_alt_pred)
{
    main_table = -1;
    main_table_index = -1;
    alt_table = -1;
    alt_table_index = -1;

    for (int i = numPredictors - 1; i >= 0; --i) {
        Addr tmp_index = getTageIndex(last_chunk_start, history, i);
        Addr tmp_tag = getTageTag(stream_start, history, i);
        auto &way = tageTable[i][tmp_index];
        bool match = way.valid && matchTag(tmp_tag, way.tag, i);

        DPRINTF(DecoupleBP || debugFlagOn,
                "TAGE table[%u] index[%u], valid: %i, expected tag: %#lx, "
                "found tag: %#lx, match: %i, useful: %i, taken@%#lx->%#lx\n",
                i, tmp_index, way.valid, tmp_tag, way.tag, match, way.useful,
                way.target.controlAddr, way.target.nextStream);

        bool sane = (last_chunk_start >= way.target.bbStart) &&
                    ((way.target.endNotTaken &&
                      last_chunk_start <
                          way.target.controlAddr + way.target.controlSize) ||
                     (!way.target.endNotTaken &&
                      last_chunk_start <= way.target.controlAddr));
        if (match && sane) {
            if (provider_counts == 0) {
                main_target = &way.target;
                main_table = i;
                main_table_index = tmp_index;
                ++provider_counts;
                DPRINTFR(DecoupleBP || debugFlagOn, ", is use as main pred\n");
            } else if (provider_counts == 1) {
                alt_target = &way.target;
                alt_table = i;
                alt_table_index = tmp_index;
                ++provider_counts;
                DPRINTFR(DecoupleBP || debugFlagOn, ", is use as alt pred\n");
                break;
            }
        } else if (!match) {
            DPRINTFR(DecoupleBP || debugFlagOn, ", but exclude caz tag mismatch\n");
        } else {
            DPRINTFR(DecoupleBP || debugFlagOn,
                    ", but exclude because chunk start %#lx not in stream %#lx-%#lx\n",
                    last_chunk_start, way.target.bbStart,
                    way.target.controlAddr);
        }
    }
    if (provider_counts > 0) {
        DPRINTFV(debugFlagOn || ::gem5::debug::DecoupleBP,
                 "Select main predictor %d, index %d\n", main_table,
                 main_table_index);
        if (provider_counts > 1) {
            DPRINTFV(debugFlagOn || ::gem5::debug::DecoupleBP,
                     "Select alt predictor %d, index %d\n", alt_table,
                     alt_table_index);
        }
        // const auto& way1 = tageTable[provider][table_index];
        // const auto& way2 = tageTable[alt_predictor][alt_predictor_index];
        if (provider_counts > 1 &&
            useAlt[computeAltSelHash(stream_start, history)] > 0 &&
            main_target->hysteresis == 0) {
            DPRINTF(DecoupleBP || debugFlagOn,
                    "Use alt predictor table[%d] index[%d]\n", alt_table,
                    alt_table_index);
            use_alt_pred = true;
            dbpstats.providerTableDist.sample(alt_table);
        } else {
            DPRINTF(DecoupleBP || debugFlagOn,
                    "Use main predictor table[%d] index[%d]\n", main_table,
                    main_table_index);
            use_alt_pred = false;
            dbpstats.providerTableDist.sample(main_table);
        }
        return true;
    } else {
        return false;
    }
}

void
StreamTAGE::putPCHistory(Addr cur_chunk_start, Addr stream_start, const bitset &history) {
    TickedStreamStorage *target = nullptr;
    TickedStreamStorage *alt_target = nullptr;
    int main_table = -1;
    int main_table_index = -1;
    int alt_table = -1;
    int alt_table_index = -1;
    int pred_count = 0;
    bool use_alt_pred = false;

    if (stream_start == ObservingPC) {
        debugFlagOn = true;
    }
    DPRINTF(DecoupleBP || debugFlagOn,
            "Predict for stream %#lx, chunk: %#lx\n", stream_start,
            cur_chunk_start);

    bool found =
        lookupHelper(false, cur_chunk_start, stream_start, history, target,
                     alt_target, main_table, main_table_index, alt_table,
                     alt_table_index, pred_count, use_alt_pred);
    if (use_alt_pred) {
        target = alt_target;
        main_table_index = alt_table_index;
        main_table = alt_table;
    }
    
    if (!found) {
        DPRINTF(DecoupleBP || debugFlagOn,
                "not found for stream=%#lx, chunk=%#lx, guess an unlimited stream\n",
                stream_start, cur_chunk_start);
        prediction.valid = false;
        prediction.history = history;
        prediction.endType = END_NONE;
        prediction.endNotTaken = true;

    } else {
        auto& way = tageTable[main_table][main_table_index];
        DPRINTF(DecoupleBP || debugFlagOn,
                "Valid: %d, chunkStart: %#lx, stream: [%#lx-%#lx] -> %#lx, taken: %i\n",
                way.valid, cur_chunk_start, stream_start,
                target->controlAddr, target->nextStream, !target->endNotTaken);

        prediction.valid = true;
        prediction.bbStart = stream_start;
        prediction.controlAddr = target->controlAddr;
        prediction.controlSize = target->controlSize;
        prediction.nextStream = target->nextStream;
        prediction.endType = target->endType;
        prediction.endNotTaken = target->endNotTaken;
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
                   Addr control_pc, Addr target_pc)
{
    return (t.bbStart == stream_start_pc) && (t.controlAddr == control_pc) &&
           (t.nextStream == target_pc);
}

void
StreamTAGE::update(Addr last_chunk_start, Addr stream_start_pc,
                   Addr control_pc, Addr target_pc, unsigned control_size,
                   bool actually_taken, const bitset& history, EndType end_type)
{
    if (stream_start_pc == ObservingPC) {
        debugFlagOn = true;
        DPRINTFV(true, "Update for stream %#lx, chunk:%#lx\n", stream_start_pc,
                 last_chunk_start);
    }
    if (actually_taken && (control_pc < stream_start_pc)) {
        DPRINTF(DecoupleBP || debugFlagOn,
                "Control PC %#lx is before stream start %#lx, ignore it\n",
                control_pc,
                stream_start_pc);
        return;
    }

    TickedStreamStorage *main_target = nullptr;
    TickedStreamStorage *alt_target = nullptr;
    TickedStreamStorage *target_sel = nullptr;
    int main_table = -1;
    int main_table_index = -1;
    int alt_table = -1;
    int alt_table_index = -1;

    int pred_count = 0;
    bool use_alt_pred = false;

    bool predictor_found = lookupHelper(
        true, last_chunk_start, stream_start_pc, history, main_target,
        alt_target, main_table, main_table_index, alt_table,
        alt_table_index, pred_count, use_alt_pred);

    auto pred_match = [stream_start_pc, control_pc,
                          target_pc](const TickedStreamStorage& t) {
        return (t.bbStart == stream_start_pc) &&
               (t.controlAddr == control_pc) && (t.nextStream == target_pc);
    };

    if (predictor_found && use_alt_pred) {
        target_sel = alt_target;
    } else if (predictor_found) {
        target_sel = main_target;
    }

    bool main_is_useless = false;
    if (pred_count > 0) {
        assert (main_table >= 0);
        // update counter
        auto& main_entry = tageTable[main_table][main_table_index];
        bool main_match = pred_match(*main_target);
        bool alt_match = alt_target && pred_match(*alt_target);

        DPRINTF(DecoupleBP || debugFlagOn,
                "Previous main pred: [%#lx-%#lx] -> %#lx, taken: %i\n",
                main_target->bbStart, main_target->controlAddr,
                main_target->nextStream, !main_target->endNotTaken);
        if (alt_target) {
            DPRINTF(DecoupleBP || debugFlagOn,
                    "Previous alt pred: [%#lx-%#lx] -> %#lx, taken: %i\n",
                    alt_target->bbStart, alt_target->controlAddr,
                    alt_target->nextStream, !alt_target->endNotTaken);
        }

        if (pred_match(*main_target)) {
            // inc main entry confidence
            satIncrement(*main_target);
            DPRINTF(DecoupleBP || debugFlagOn,
                     "Increment conf to %d for table[%d] index[%d]\n",
                     main_target->hysteresis, main_table, main_table_index);
        } else {
            satDecrement(*main_target);
            DPRINTF(DecoupleBP || debugFlagOn,
                     "Decrement conf to %d for table[%d] index[%d]\n",
                     main_target->hysteresis, main_table, main_table_index);
        }

        // update usefull
        if (pred_match(*main_target)) {
            bool no_alt = pred_count == 1;
            bool main_neq_alt = (pred_count > 1) && !pred_match(*alt_target);
            if (no_alt || main_neq_alt) {
                DPRINTF(DecoupleBP || debugFlagOn,
                         "mark table[%d] index[%d] as useful\n",
                         main_table, main_table_index);
                main_entry.useful = 1;
            }
        }
        
        // update alt choice counter
        if (main_target->hysteresis == 0 && pred_count > 1 &&
            (main_match ^ alt_match)) {  // one of them is correct
            auto &alt_entry = useAlt[computeAltSelHash(stream_start_pc, history)];
            if (!main_match) {
                satIncrement(8, alt_entry);
                DPRINTF(DecoupleBP || debugFlagOn,
                         "Increment alt choice counter to %d\n",
                         alt_entry);
            } else {
                satDecrement(-7, alt_entry);
                DPRINTF(DecoupleBP || debugFlagOn,
                         "Decrement alt choice counter to %d\n",
                         alt_entry);
            }
        }
        main_is_useless = main_entry.useful == 0 && main_table > 0;
    }

    if (predictor_found && pred_match(*target_sel)) {
        // correct, do not allocate
        return;
    }

    // allocate
    unsigned start_table;
    unsigned allocated = 0;
    if (pred_count == 0) {  // no entry
        start_table = 0;  // allocate since base
    } else if (main_is_useless) {
        start_table = main_table;
    } else {
        start_table = main_table + 1;
    }

    for (; start_table < numPredictors; start_table++) {
        uint32_t new_index =
            getTageIndex(last_chunk_start, history, start_table);
        uint32_t new_tag =
            getTageTag(stream_start_pc, history, start_table);
        auto &entry = tageTable[start_table][new_index];
        DPRINTF(DecoupleBP || debugFlagOn,
                "Table %d index[%d] histlen=%u is %s\n", start_table,
                new_index, histLengths[start_table], entry.useful ? "useful" : "not useful");

        if (!entry.useful) {
            DPRINTF(DecoupleBP || debugFlagOn,
                    "%s %#lx-%#lx -> %#lx with %#lx-%#lx -> %#lx, new "
                    "tag=%#lx\n", entry.valid ? "Replacing": "Allocating",
                    entry.target.bbStart, entry.target.controlAddr,
                    entry.target.nextStream, stream_start_pc, control_pc,
                    target_pc, new_tag);

            entry.target.set(curTick(), stream_start_pc, control_pc, target_pc,
                             control_size, 0, end_type, true, !actually_taken);
            entry.useful = 0;
            entry.valid = true;
            setTag(entry.tag, new_tag, start_table);

            allocated++;
            if (allocated == numTablesToAlloc) {
                break;
            }
        }
    }
    maintainUsefulCounters(allocated);
    debugFlagOn = false;
}

void
StreamTAGE::commit(Addr stream_start_pc, Addr controlAddr, Addr target, bitset &history)
{
    panic("Not implemented\n");
}

uint64_t
StreamTAGE::getTableGhrLen(int table) {
    return histLengths[table];
}

Addr
StreamTAGE::getTageTag(Addr pc, const bitset& history, int t)
{
    if (!hasTag[t]) {
        return 0;
    }
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
    if (histLengths[t] == 0) {
        return (pc >> tablePcShifts[t]) % tableSizes[t];
    }
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

unsigned
StreamTAGE::computeAltSelHash(Addr pc, const bitset& ghr)
{
    return pc % altSelectorSize;
}

bool
StreamTAGE::matchTag(Addr expected, Addr found, int table)
{
    return !hasTag[table] || expected == found;
}

void
StreamTAGE::setTag(Addr& dest, Addr src, int table)
{
    if (hasTag[table]) {
        dest = src;
    }
}

bool
StreamTAGE::satIncrement(int max, int &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
StreamTAGE::satIncrement(TickedStreamStorage &target)
{
    return satIncrement(2, target.hysteresis);
}

bool 
StreamTAGE::satDecrement(int min, int &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

bool 
StreamTAGE::satDecrement(TickedStreamStorage &target)
{
    return satDecrement(0, target.hysteresis);
}

void
StreamTAGE::maintainUsefulCounters(int allocated)
{
    if (allocated) {
        if (usefulResetCounter < 255) {
            usefulResetCounter++;
        }
        DPRINTF(DecoupleBPUseful,
                "Succeed to allocate, useful resetting counter now: %u\n",
                usefulResetCounter);
    } else {
        if (usefulResetCounter > 0) {
            --usefulResetCounter;
        }
        DPRINTF(DecoupleBPUseful,
                "Failed to allocate, useful resetting counter now: %u\n",
                usefulResetCounter);

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

}  // namespace branch_prediction

}  // namespace gem5

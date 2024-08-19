#include "cpu/pred/stream/modify_tage.hh"

#include <algorithm>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPUseful.hh"
#include "debug/Override.hh"
#include "cpu/pred/stream/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

namespace stream_pred {

StreamTAGE::StreamTAGE(const Params& p):
    TimedStreamPredictor(p),
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
    tagFoldedHist.resize(numPredictors);
    indexFoldedHist.resize(numPredictors);
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(maxHistLen, true);
        tableIndexMasks[i] >>= (maxHistLen - tableIndexBits[i]);

        tagFoldedHist[i].resize(tableTagBits[i]);
        indexFoldedHist[i].resize(tableIndexBits[i]);

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
                         bool& use_alt_pred, std::vector<bitset> index_folded_hist,
                         std::vector<bitset> tag_folded_hist)
{
    if (stream_start == ObservingPC2) {
        debugFlagOn = true;
    }
    main_table = -1;
    main_table_index = -1;
    alt_table = -1;
    alt_table_index = -1;

    DPRINTF(DecoupleBP || debugFlagOn, "history: %s\n", history);

    for (int i = numPredictors - 1; i >= 0; --i) {
        Addr tmp_index = getTageIndex(last_chunk_start, history, i, index_folded_hist);
        Addr tmp_tag = getTageTag(stream_start, history, i, tag_folded_hist);
        auto &way = tageTable[i][tmp_index];
        bool match = way.valid && matchTag(tmp_tag, way.tag, i);

        DPRINTF(DecoupleBP || debugFlagOn,
                "TAGE table[%u] index[%u], valid: %i, expected tag: %#lx, "
                "found tag: %#lx, match: %i, useful: %i, start: %#lx, taken@%#lx->%#lx, end_type:%d\n",
                i, tmp_index, way.valid, tmp_tag, way.tag, match, way.useful, way.target.bbStart,
                way.target.controlAddr, way.target.nextStream, way.target.endType);

        bool sane = (last_chunk_start >= way.target.bbStart) &&
                    ((!way.target.isTaken() &&
                      last_chunk_start < way.target.getFallThruPC()) ||
                     (way.target.isTaken() &&
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

std::pair<bool, std::pair<bool, Addr>>
StreamTAGE::makeLoopPrediction(bool use_alt_pred, int pred_count, TickedStreamStorage *target, TickedStreamStorage *alt_target) {
    bool useMainLoopPrediction = false;
    Addr mainLoopPredAddr = 0;
    
    bool useAltLoopPrediction = false;
    Addr altLoopPredAddr = 0;

    if (pred_count > 1) {
        std::tie(useAltLoopPrediction, altLoopPredAddr) = 
        loopPredictor->makeLoopPrediction(alt_target->controlAddr);
    }

    if (pred_count > 0) {
        std::tie(useMainLoopPrediction, mainLoopPredAddr) = 
        loopPredictor->makeLoopPrediction(target->controlAddr);
    }

    if (pred_count > 1 && target->controlAddr == alt_target->controlAddr) {
        if (useMainLoopPrediction) {
            return std::make_pair(true, std::make_pair(true, mainLoopPredAddr));
        } else if (useAltLoopPrediction) {
            return std::make_pair(true, std::make_pair(false, altLoopPredAddr));
        } else {
            return std::make_pair(false, std::make_pair(false, 0));
        }
    } else {
        if (use_alt_pred && useAltLoopPrediction) {
            return std::make_pair(true, std::make_pair(false, altLoopPredAddr));
        }
        if (!use_alt_pred && useMainLoopPrediction) {
            return std::make_pair(true, std::make_pair(true, mainLoopPredAddr));
        }
        return std::make_pair(false, std::make_pair(false, 0));
    }
}

void
StreamTAGE::putPCHistory(Addr cur_chunk_start, Addr stream_start, const bitset &history) {
    DPRINTF(Override, "In tage.putPCHistory().\n");

    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    TickedStreamStorage *target = nullptr;
    TickedStreamStorage *alt_target = nullptr;
    int main_table = -1;
    int main_table_index = -1;
    int alt_table = -1;
    int alt_table_index = -1;
    int pred_count = 0;
    bool use_alt_pred = false;

    if (stream_start == ObservingPC || stream_start == ObservingPC2) {
        debugFlagOn = true;
    }
    DPRINTF(DecoupleBP || debugFlagOn,
            "Predict for stream %#lx, chunk: %#lx\n", stream_start,
            cur_chunk_start);

    bool found =
        lookupHelper(false, cur_chunk_start, stream_start, history, target,
                     alt_target, main_table, main_table_index, alt_table,
                     alt_table_index, pred_count, use_alt_pred, indexFoldedHist, tagFoldedHist);
 
    auto res = makeLoopPrediction(use_alt_pred, pred_count, target, alt_target);
    bool useLoopPrediction = res.first;
    bool useAltLoopPred = !res.second.first;
    Addr loopPredAddr = res.second.second;

    if (useLoopPrediction && loopPredictor->loopValid()) {
        if (useAltLoopPred) {
            target = alt_target;
            main_table_index = alt_table_index;
            main_table = alt_table;
            DPRINTF(DecoupleBP || debugFlagOn, "use alt loop prediction\n");
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "use main loop prediction\n");
        }
    } else {
        if (use_alt_pred) {
            target = alt_target;
            main_table_index = alt_table_index;
            main_table = alt_table;
            DPRINTF(DecoupleBP || debugFlagOn, "use alt prediction\n");
        } else {
            DPRINTF(DecoupleBP || debugFlagOn, "use main prediction\n");
        }
    }
    
    if (!found) {
        DPRINTF(DecoupleBP || debugFlagOn,
                "not found for stream=%#lx, chunk=%#lx, guess an unlimited stream\n",
                stream_start, cur_chunk_start);
        prediction.valid = false;
        prediction.history = history;
        prediction.endType = END_NONE;
        prediction.useLoopPrediction = false;
        prediction.predSource = 2;

    } else {
        bool loopValid = loopPredictor->loopValid();
        auto& way = tageTable[main_table][main_table_index];
        DPRINTF(DecoupleBP || debugFlagOn,
                "use loop prediction: %d, Valid: %d, chunkStart: %#lx, stream: [%#lx-%#lx] -> %#lx, taken: %i\n",
                useLoopPrediction && loopValid, way.valid, cur_chunk_start, stream_start,
                target->controlAddr, useLoopPrediction && loopValid ? loopPredAddr : target->nextStream, target->isTaken());

        prediction.valid = true;
        prediction.bbStart = stream_start;
        prediction.controlAddr = target->controlAddr;
        prediction.controlSize = target->controlSize;
        prediction.tageTarget = target->nextStream;
        prediction.nextStream = useLoopPrediction && loopValid ? loopPredAddr : target->nextStream;
        prediction.endType = useLoopPrediction && loopValid ? (loopPredictor->isTakenForward(target->controlAddr) ? END_NOT_TAKEN : END_OTHER_TAKEN) : target->endType;
        prediction.history = history;
        prediction.useLoopPrediction = useLoopPrediction;

        way.target.tick = curTick();

        if (useLoopPrediction && !loopValid)
            prediction.predSource = LoopButInvalid;
        else if (useLoopPrediction && loopValid)
            prediction.predSource = LoopAndValid;
        else
            prediction.predSource = TAGE;
    }
    prediction.indexFoldedHist = indexFoldedHist;
    prediction.tagFoldedHist = tagFoldedHist;
    debugFlagOn = false;
    DPRINTF(Override, "Ends tage.putPCHistory().\n");
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

void
StreamTAGE::recordFoldedHist(StreamPrediction &pred) {
    pred.indexFoldedHist = indexFoldedHist;
    pred.tagFoldedHist = tagFoldedHist;
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
                   bool actually_taken, const bitset& history, EndType end_type, 
                   std::vector<bitset> stream_indexFoldedHist, std::vector<bitset> stream_tagFoldedHist)
{
    if (stream_start_pc == ObservingPC || control_pc == ObservingPC2) {
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
        alt_table_index, pred_count, use_alt_pred, stream_indexFoldedHist, stream_tagFoldedHist);

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

    DPRINTF(DecoupleBP || debugFlagOn,
            "Update for stream %#lx, chunk:%#lx, predictor_found: %d, use_alt_pred: %d, pred_count: %d\n",
            stream_start_pc, last_chunk_start, predictor_found, use_alt_pred, pred_count);

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
                main_target->nextStream, main_target->isTaken());
        if (alt_target) {
            DPRINTF(DecoupleBP || debugFlagOn,
                    "Previous alt pred: [%#lx-%#lx] -> %#lx, taken: %i\n",
                    alt_target->bbStart, alt_target->controlAddr,
                    alt_target->nextStream, alt_target->isTaken());
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

        main_is_useless = main_entry.useful == 0 && main_table > 0;
    }

    if (predictor_found && pred_match(*target_sel)) {
        // correct, do not allocate
        return;
    }

    // allocate
    unsigned start_table;
    unsigned allocated = 0, new_allocated = 0;
    if (pred_count == 0) {  // no entry
        start_table = 0;  // allocate since base
    } else if (main_is_useless) {
        start_table = main_table;
    } else {
        start_table = main_table + 1;
    }

    for (; start_table < numPredictors; start_table++) {
        uint32_t new_index =
            getTageIndex(last_chunk_start, history, start_table, stream_indexFoldedHist);
        uint32_t new_tag =
            getTageTag(stream_start_pc, history, start_table, stream_tagFoldedHist);
        auto &entry = tageTable[start_table][new_index];
        DPRINTF(DecoupleBP || debugFlagOn,
                "Table %d index[%d] histlen=%u is %s\n", start_table,
                new_index, histLengths[start_table], entry.useful ? "useful" : "not useful");

        if (!entry.useful) {
            DPRINTF(DecoupleBP || debugFlagOn,
                    "%s %#lx-%#lx -> %#lx with %#lx-%#lx -> %#lx, new "
                    "tag=%#lx, end type=%i\n", entry.valid ? "Replacing": "Allocating",
                    entry.target.bbStart, entry.target.controlAddr,
                    entry.target.nextStream, stream_start_pc, control_pc,
                    target_pc, new_tag, end_type);

            entry.target.set(curTick(), stream_start_pc, control_pc, target_pc,
                            control_size, 0, end_type, true, !actually_taken);
            entry.useful = 0;
            setTag(entry.tag, new_tag, start_table);

            allocated++;
            if (!entry.valid) {
                new_allocated++;
            }
            entry.valid = true;
            if (allocated == numTablesToAlloc) {
                break;
            }
        }
    }
    maintainUsefulCounters(allocated, new_allocated);
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
StreamTAGE::getTageTag(Addr pc, const bitset& history, int t, std::vector<bitset> tag_folded_hist)
{
    if (!hasTag[t]) {
        return 0;
    }

    bitset buf2(tableTagBits[t], pc >> tablePcShifts[t]);
    buf2 ^= tag_folded_hist[t];
    return buf2.to_ulong();
}

void
StreamTAGE::maintainFoldedHist(const bitset& history, bitset hash)
{
    DPRINTF(DecoupleBP, "history:\t%s\n", history);
    // update the folded history when the global history is updated in the decoupled_bpred.cc
    for (int t = 1;t < numPredictors;t++) {
        for (int type = 0;type < 2;type++) {
            bool res = false;

            DPRINTF(DecoupleBP, "t: %d, type: %d\n", t, type);
            std::string buf1;
            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            DPRINTF(DecoupleBP, "foldedHist:\t%s\n", foldedHist);
            unsigned int foldedLen = type ? tableTagBits[t] : tableIndexBits[t];
            DPRINTF(DecoupleBP, "foldLen: %d, histLengths: %d\n", foldedLen, histLengths[t]);
            unsigned int modResult = histLengths[t] % foldedLen;
            bitset tempHist(history);

            for(int i = 0;i < 2;i++) {
                tempHist.resize(histLengths[t]);
                bool foldedHighRes = foldedHist[foldedLen - 1];
                bool oriHighRes = tempHist[histLengths[t] - 1];
                DPRINTF(DecoupleBP, "oriHighRes: %d, foldedHighRes: %d\n", oriHighRes, foldedHighRes);

                foldedHist <<= 1;
                tempHist <<= 1;
                foldedHist[0] = res ^ foldedHighRes;
                foldedHist[modResult] ^= oriHighRes;
                foldedHist.resize(foldedLen);

                DPRINTF(DecoupleBP, "%d iteration updated foldedHist:\t%s\n", i, foldedHist);
            }
            assert(foldedLen >= 8);
            hash.resize(foldedLen);
            foldedHist ^= hash;
            DPRINTF(DecoupleBP, "final foldedHist:\t%s\n", foldedHist);
        }
    }
}

void
StreamTAGE::checkFoldedHist(const bitset& history)
{
    DPRINTF(DecoupleBP, "history:\t%s\n", history);
    // update the folded history when the global history is updated in the decoupled_bpred.cc
    for (int t = 1;t < numPredictors;t++) {
        for (int type = 0;type < 2;type++) {
            DPRINTF(DecoupleBP, "t: %d, type: %d\n", t, type);
            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            unsigned int foldedLen = type ? tableTagBits[t] : tableIndexBits[t];

            // check history
            bitset idealHist(history);
            idealHist.resize(histLengths[t]);
            DPRINTF(DecoupleBP, "idealHist:\t%s\n", idealHist);

            bitset idealFoldedHist;
            idealFoldedHist.resize(foldedLen);
            for (int i = 0;i < histLengths[t];i++) {
                idealFoldedHist[i % foldedLen] ^= idealHist[i];
            }
            assert(foldedLen >= 8);
            DPRINTF(DecoupleBP, "idealFoldedHist:\t%s\tfoldedHist:\t%s\n", idealFoldedHist, foldedHist);
            assert(idealFoldedHist == foldedHist);
        }
    }
}

void
StreamTAGE::recoverFoldedHist(const bitset& history)
{
    DPRINTF(DecoupleBP, "recoverFoldedHist: %s\n", history);
    // manually compute the folded history
    for (int t = 1;t < numPredictors;t++) {
        for (int type = 0;type < 2;type++) {
            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            unsigned int foldedLen = type ? tableTagBits[t] : tableIndexBits[t];
            DPRINTF(DecoupleBP, "foldedHist:\t%s\tcleared:\t%s\n", foldedHist, foldedLen);
            foldedHist.clear();
            foldedHist.resize(foldedLen);
            bitset tempHist(history);
            tempHist.resize(histLengths[t]);
            for (int j = 0;j < histLengths[t];j++) {
                foldedHist[j % foldedLen] ^= tempHist[j];
            }
            DPRINTF(DecoupleBP, "t:%d, type: %d, history:\t%s\n", t, type, foldedHist);
        }
    }
}

Addr
StreamTAGE::getTageIndex(Addr pc, const bitset& history, int t, std::vector<bitset> index_folded_hist)
{
    if (histLengths[t] == 0) {
        return (pc >> tablePcShifts[t]) % tableSizes[t];
    }

    bitset buf2(tableIndexBits[t], pc >> tablePcShifts[t]);
    buf2 ^= index_folded_hist[t];
    return buf2.to_ulong();
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
StreamTAGE::maintainUsefulCounters(int allocated, int new_allocated)
{
    if (allocated && !new_allocated) {
        DPRINTF(DecoupleBPUseful,
                "Allocated from useless entry, dont modify reset counter: %u\n",
                usefulResetCounter);
        return;
    }
    if (new_allocated) {  // allocated from new entry
        if (usefulResetCounter < 255) {
            usefulResetCounter++;
        }
        DPRINTF(DecoupleBPUseful,
                "Succeed to allocate, useful resetting counter now: %u\n",
                usefulResetCounter);
    } else if (!allocated) {  // allocation completely failed
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

}  // namespace stream_pred

}  // namespace branch_prediction

}  // namespace gem5

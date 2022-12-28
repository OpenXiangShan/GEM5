#include "cpu/pred/ftb/ftb_ittage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPUseful.hh"
#include "debug/FTBITTAGE.hh"
#include "cpu/pred/ftb/stream_common.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred{

FTBITTAGE::FTBITTAGE(const Params& p):
TimedBaseFTBPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
maxHistLen(p.maxHistLen),
numTablesToAlloc(p.numTablesToAlloc),
numBr(p.numBr)
{
    DPRINTF(FTBITTAGE || debugFlag, "FTBITTAGE constructor numBr=%d\n", numBr);
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    baseTable.resize(2048); // need modify
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        for (int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(1);
        }

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(tableIndexBits[i], true);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(tableTagBits[i], true);

        assert(tablePcShifts.size() >= numPredictors);

        tagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i], (int)numBr));
        indexFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableIndexBits[i], (int)numBr));
    }
    for (unsigned i = 0; i < baseTable.size(); ++i) {
        baseTable[i].resize(1);
    }
    useAlt.resize(128);
    for (unsigned i = 0; i < useAlt.size(); ++i) {
        useAlt[i].resize(1, 0);
    }
    usefulResetCnt.resize(1, 0);
}

void
FTBITTAGE::tickStart()
{
}

void
FTBITTAGE::tick() {}

std::vector<bool>
FTBITTAGE::lookupHelper(Addr startAddr,
                      std::vector<TageEntry> &main_entries,
                      std::vector<int> &main_tables,
                      std::vector<int> &main_table_indices,
                      std::vector<bool> &use_alt_preds, std::vector<bitset> &usefulMasks)
{
    DPRINTF(FTBITTAGE || debugFlag, "lookupHelper startAddr: %#lx\n", startAddr);
    for (auto &t : main_tables) { t = -1; }
    for (auto &t : main_table_indices) { t = -1; }

    std::vector<bool> provided;
    provided.resize(1, false);

    for (int b = 0; b < 1; b++) {
        // make main prediction
        int provider_counts = 0;
        for (int i = numPredictors - 1; i >= 0; --i) {
            Addr tmp_index = getTageIndex(startAddr, i);
            Addr tmp_tag = getTageTag(startAddr, i);
            auto &way = tageTable[i][tmp_index][b];
            bool match = way.valid && matchTag(tmp_tag, way.tag);

            if (match) {
                main_entries[b] = way;
                main_tables[b] = i;
                main_table_indices[b] = tmp_index;
                ++provider_counts;
                break;
            }

            usefulMasks[b].resize(numPredictors-i);
            usefulMasks[b] <<= 1;
            usefulMasks[b][0] = way.useful;
            DPRINTF(FTBITTAGE || debugFlag, "table %d, index %d, lookup tag %d, tag %d, useful %d\n",
                i, tmp_index, tmp_tag, way.tag, way.useful);
        }


        if (provider_counts > 0) {
            auto main_entry = main_entries[b];
            if (useAlt[getUseAltIdx(startAddr)][b] > 0 &&
                (main_entry.counter == -1 || main_entry.counter == 0)) {
                use_alt_preds[b] = true;
            } else {
                use_alt_preds[b] = false;
            }
            provided[b] = true;
        } else {
            use_alt_preds[b] = true;
            provided[b] = false;
        }
        DPRINTF(FTBITTAGE || debugFlag, "lookup cond %d, provider_counts %d, main_table %d, main_table_index %d, use_alt %d\n",
                    b, provider_counts, main_tables[b], main_table_indices[b], use_alt_preds[b]);
    }
    return provided;
}

void
FTBITTAGE::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullFTBPrediction> &stagePreds) {
    if (debugPC == stream_start) {
        debugFlag = true;
    }
    DPRINTF(FTBITTAGE || debugFlag, "putPCHistory startAddr: %#lx\n", stream_start);
    std::vector<TageEntry> entries;
    entries.resize(1);
    for (int i = 0; i < 1; ++i) {
        entries[i].valid = false;
    }
    std::vector<int> main_tables;
    std::vector<int> main_table_indices;
    std::vector<bool> use_alt_preds;
    std::vector<bitset> usefulMasks;
    main_tables.resize(1, -1);
    main_table_indices.resize(1, -1);
    use_alt_preds.resize(1, false);
    usefulMasks.resize(1);

    std::vector<bool> takens;
    takens.resize(1, false);

    // get prediction and save it
    std::vector<bool> found = lookupHelper(stream_start, entries, main_tables,
                                    main_table_indices, use_alt_preds, usefulMasks);

    std::vector<std::pair<Addr, short>> altRes = baseTable.at(getBaseTableIndex(stream_start));

    std::vector<TagePrediction> preds;
    preds.resize(1);
    for (int b = 0; b < 1; ++b) {
        preds[b] = TagePrediction(found[b], entries[b].target, altRes[b].first,
            main_tables[b], main_table_indices[b], entries[b].tag, 0, use_alt_preds[b], usefulMasks[b]);
    }
    
    assert(getDelay() < stagePreds.size());
    for (int s = getDelay(); s < stagePreds.size(); ++s) { // need modify
        Addr useTarget = use_alt_preds[0] ? altRes[0].first : entries[0].target;
        DPRINTF(FTBITTAGE || debugFlag, "indirect target=%#lx\n", useTarget);
        stagePreds[s].indirectTarget = useTarget;
    }

    meta.preds = preds;
    meta.tagFoldedHist = tagFoldedHist;
    meta.indexFoldedHist = indexFoldedHist;
    DPRINTF(FTBITTAGE || debugFlag, "putPCHistory end\n");
    debugFlag = false;
}

std::shared_ptr<void>
FTBITTAGE::getPredictionMeta() {
    std::shared_ptr<void> meta_void_ptr = std::make_shared<TageMeta>(meta);
    return meta_void_ptr;
}

void
FTBITTAGE::update(const FetchStream &entry)
{
    if (debugPC == entry.startPC || debugPC2 == entry.startPC) {
        debugFlag = true;
    }
    Addr startAddr = entry.startPC;
    DPRINTF(FTBITTAGE || debugFlag, "update startAddr: %#lx\n", startAddr);
    auto ftb_entry = entry.updateFTBEntry;

    // get tage predictions from meta
    // TODO: use component idx
    auto meta = std::static_pointer_cast<TageMeta>(entry.predMetas[4]);
    auto preds = meta->preds;
    auto updateTagFoldedHist = meta->tagFoldedHist;
    auto updateIndexFoldedHist = meta->indexFoldedHist;
    for (int b = 0; b < 1; b++) {
        DPRINTF(FTBITTAGE || debugFlag, "try to update cond %d \n", b);

        TagePrediction pred = preds[b];
        bool mainFound = pred.mainFound;
        bool mainTarget = pred.mainTarget;
        bool altTarget = baseTable.at(getBaseTableIndex(startAddr))[b].first;

        // update useful bit, counter and predTaken for main entry
        if (mainFound) { // updateProvided
            DPRINTF(FTBITTAGE || debugFlag, "prediction provided by table %d, idx %d, updating corresponding entry\n",
                pred.table, pred.index);
            auto &way = tageTable[pred.table][pred.index][b];

            if (mainTarget != altTarget) { // updateAltDiffers
                way.useful = entry.exeBranchInfo.target == mainTarget; // updateProviderCorrect
            }
            DPRINTF(FTBITTAGE || debugFlag, "useful bit set to %d\n", way.useful);

            updateCounter(entry.exeBranchInfo.target == mainTarget, 3, way.counter); // need modify
        }

        // update base table counter
        if (pred.useAlt) {
            unsigned base_idx = getBaseTableIndex(startAddr);
            DPRINTF(FTBITTAGE || debugFlag, "prediction provided by base table idx %d, updating corresponding entry\n", base_idx);
            baseTable.at(base_idx)[b].first = entry.exeBranchInfo.target;
            updateCounter(entry.exeBranchInfo.target == altTarget, 2, baseTable.at(base_idx)[b].second); // need mofigy
        }

        // update use_alt_counters
        if (pred.mainFound && (pred.counter == 0 || pred.counter == -1) &&
                mainTarget != altTarget) {
            auto &use_alt_counter = useAlt.at(getUseAltIdx(startAddr))[b];
            if (altTarget == entry.exeBranchInfo.target) {
                satIncrement(7, use_alt_counter);
            } else {
                satDecrement(-8, use_alt_counter);
            }
            DPRINTF(FTBITTAGE || debugFlag, "updating use_alt_counter %d\n", use_alt_counter);
        }

        Addr slotPC = 0;
        for (auto iter : ftb_entry.slots) {
            if (iter.getBranchInfo().isIndirect) {
                slotPC = iter.pc;
                break;
            }
        }

        DPRINTF(FTBITTAGE || debugFlag, "squashType %d, squashPC %#lx, slot pc %#lx\n", entry.squashType, entry.squashPC, slotPC);
        bool this_mispred = entry.squashType == SquashType::SQUASH_CTRL && entry.squashPC == slotPC;
        if (this_mispred) {
            DPRINTF(FTBITTAGE || debugFlag, "miss target=%#lx, correct target=%#lx\n", entry.predBranchInfo.target, entry.exeBranchInfo.target);
        } else {
            DPRINTF(FTBITTAGE || debugFlag, "hit target=%#lx, correct target=%#lx\n", entry.predBranchInfo.target, entry.exeBranchInfo.target);
        }
        // update useful reset counter
        bool use_alt_on_main_found_correct = pred.useAlt && pred.mainFound && mainTarget == entry.exeBranchInfo.target;
        bool needToAllocate = this_mispred && !use_alt_on_main_found_correct;
        DPRINTF(FTBITTAGE || debugFlag, "this_mispred %d, use_alt_on_main_found_correct %d, needToAllocate %d\n",
            this_mispred, use_alt_on_main_found_correct, needToAllocate);

        int num_tables_can_allocate = (~pred.usefulMask).count();
        int total_tables_to_allocate = pred.usefulMask.size();
        bool incUsefulResetCounter = num_tables_can_allocate < (total_tables_to_allocate - num_tables_can_allocate);
        bool decUsefulResetCounter = num_tables_can_allocate > (total_tables_to_allocate - num_tables_can_allocate);
        int changeVal = std::abs(num_tables_can_allocate - (total_tables_to_allocate - num_tables_can_allocate));
        if (needToAllocate) {
            if (incUsefulResetCounter) { // need modify: clear the useful bit of all entries
                usefulResetCnt[b] += changeVal;
                if (usefulResetCnt[b] >= 128)
                    usefulResetCnt[b] = 128;
                DPRINTF(FTBITTAGE || debugFlag, "incUsefulResetCounter, changeVal %d, usefulResetCnt %d\n", changeVal, usefulResetCnt[b]);
            } else if (decUsefulResetCounter) {
                usefulResetCnt[b] -= changeVal;
                if (usefulResetCnt[b] <= 0)
                    usefulResetCnt[b] = 0;
                DPRINTF(FTBITTAGE || debugFlag, "decUsefulResetCounter, changeVal %d, usefulResetCnt %d\n", changeVal, usefulResetCnt[b]);
            }

            if (usefulResetCnt[b] == 128) {
                DPRINTF(FTBITTAGE || debugFlag, "reset useful bit of all entries\n");
                for (auto &table : tageTable) {
                    for (auto &entries : table) {
                        for (auto &entry : entries) {
                            entry.useful = 0;
                        }
                    }
                }
                usefulResetCnt[b] = 0;
            }
        }

        if (needToAllocate) {
            // allocate new entry
            unsigned maskMaxNum = std::pow(2, (numPredictors - (pred.table + 1)));
            unsigned mask = allocLFSR.get() % maskMaxNum;
            bitset allocateLFSR(numPredictors - (pred.table + 1), mask);
            std::string buf;
            boost::to_string(allocateLFSR, buf);
            DPRINTF(FTBITTAGE || debugFlag, "allocateLFSR %s, size %d\n", buf, allocateLFSR.size());
            auto flipped_usefulMask = pred.usefulMask.flip();
            boost::to_string(flipped_usefulMask, buf);
            DPRINTF(FTBITTAGE || debugFlag, "pred usefulmask %s, size %d\n", buf, pred.usefulMask.size());
            bitset masked = allocateLFSR & flipped_usefulMask;
            boost::to_string(masked, buf);
            DPRINTF(FTBITTAGE || debugFlag, "masked %s, size %d\n", buf, masked.size());
            bitset allocate = masked.any() ? masked : flipped_usefulMask;
            boost::to_string(allocate, buf);
            DPRINTF(FTBITTAGE || debugFlag, "allocate %s, size %d\n", buf, allocate.size());

            bool allocateValid = flipped_usefulMask.any();
            if (needToAllocate && allocateValid) {
                DPRINTF(FTBITTAGE || debugFlag, "allocate new entry\n");
                unsigned startTable = pred.table + 1;

                for (int ti = startTable; ti < numPredictors; ti++) {
                    Addr newIndex = getTageIndex(startAddr, ti, updateIndexFoldedHist[ti].get());
                    Addr newTag = getTageTag(startAddr, ti, updateTagFoldedHist[ti].get());
                    auto &newEntry = tageTable[ti][newIndex][b];

                    if (allocate[ti - startTable]) {
                        DPRINTF(FTBITTAGE || debugFlag, "found allocatable entry, table %d, index %d, tag %d, counter %d\n",
                            ti, newIndex, newTag, -1);
                        newEntry = TageEntry(newTag, entry.exeBranchInfo.target, -1);
                        break; // allocate only 1 entry
                    }
                }
            }
        }
    }
    DPRINTF(FTBITTAGE || debugFlag, "end update\n");
    debugFlag = false;
}

void
FTBITTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width-1)) - 1;
    int min = -(1 << (width-1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

Addr
FTBITTAGE::getTageTag(Addr pc, int t, bitset &foldedHist)
{
    bitset buf(tableTagBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf ^= foldedHist;
    return buf.to_ulong();
}

Addr
FTBITTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get());
}

Addr
FTBITTAGE::getTageIndex(Addr pc, int t, bitset &foldedHist)
{
    bitset buf(tableIndexBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf ^= foldedHist;
    return buf.to_ulong();
}

Addr
FTBITTAGE::getTageIndex(Addr pc, int t)
{
    return getTageIndex(pc, t, indexFoldedHist[t].get());
}

unsigned
FTBITTAGE::getBaseTableIndex(Addr pc) {
    return (pc >> instShiftAmt) % baseTable.size();
}

bool
FTBITTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
FTBITTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
FTBITTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

Addr
FTBITTAGE::getUseAltIdx(Addr pc) {
    return (pc >> instShiftAmt) & (useAlt.size() - 1); // need modify
}

void
FTBITTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken)
{
    std::string buf;
    boost::to_string(history, buf);
    DPRINTF(FTBITTAGE || debugFlag, "in doUpdateHist, shamt %d, taken %d, history %s\n", shamt, taken, buf);
    if (shamt == 0) {
        DPRINTF(FTBITTAGE || debugFlag, "shamt is 0, returning\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 2; type++) {
            DPRINTF(FTBITTAGE || debugFlag, "t: %d, type: %d\n", t, type);

            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            foldedHist.update(history, shamt, taken);
        }
    }
}

void
FTBITTAGE::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getHistInfo();
    doUpdateHist(history, shamt, cond_taken);
}

void
FTBITTAGE::recoverHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    // TODO: need to get idx
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[4]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken);
}

void
FTBITTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    DPRINTF(FTBITTAGE || debugFlag, "checking folded history when %s\n", when);
    std::string hist_str;
    boost::to_string(hist, hist_str);
    DPRINTF(FTBITTAGE || debugFlag, "history:\t%s\n", hist_str.c_str());
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 2; type++) {
            DPRINTF(FTBITTAGE || debugFlag, "t: %d, type: %d\n", t, type);
            std::string buf2, buf3;
            auto &foldedHist = type ? tagFoldedHist[t] : indexFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

} // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
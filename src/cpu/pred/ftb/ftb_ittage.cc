#include "cpu/pred/ftb/ftb_ittage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPUseful.hh"
#include "debug/FTBITTAGE.hh"

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
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(tableIndexBits[i], true);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(tableTagBits[i], true);

        assert(tablePcShifts.size() >= numPredictors);

        tagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i], (int)numBr));
        altTagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i]-1, (int)numBr));
        indexFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableIndexBits[i], (int)numBr));
    }
    // useAlt.resize(128);
    // for (unsigned i = 0; i < useAlt.size(); ++i) {
    //     useAlt[i].resize(1, 0);
    // }
    usefulResetCnt = 0;
}

void
FTBITTAGE::tickStart()
{
}

void
FTBITTAGE::tick() {}

std::pair<bool, bool>
FTBITTAGE::lookupHelper(Addr startAddr, TageEntry &main_entry, int &main_table, int &main_table_index,
                        TageEntry &alt_entry, int &alt_table, int &alt_table_index, bool &use_alt_pred,
                        bool &use_base_table, bitset &usefulMask)
{
    DPRINTF(FTBITTAGE || debugFlag, "lookupHelper startAddr: %#lx\n", startAddr);
    main_table = -1;
    alt_table = -1;
    main_table_index = -1;
    alt_table_index = -1;

    bool provided = false;
    bool alt_provided = false;
    // make main prediction
    int provider_counts = 0;
    for (int i = numPredictors - 1; i >= 0; --i) {
        Addr tmp_index = getTageIndex(startAddr, i);
        Addr tmp_tag = getTageTag(startAddr, i);
        auto &way = tageTable[i][tmp_index];
        bool match = way.valid && matchTag(tmp_tag, way.tag);
        if (match) {
            ++provider_counts;
            DPRINTF(FTBITTAGE || debugFlag, "matches table %d index %d tag %d\n", i, tmp_index, tmp_tag);
            if (!provided) {
                main_entry = way;
                main_table = i;
                main_table_index = tmp_index;
                provided = true;
            } else {
                alt_entry = way;
                alt_table = i;
                alt_table_index = tmp_index;
                alt_provided = true;
                break;
            }
        }
        if (!provided) {
            usefulMask.resize(numPredictors-i);
            usefulMask <<= 1;
            usefulMask[0] = way.useful;
        }
        DPRINTF(FTBITTAGE || debugFlag, "table %d, index %d, lookup tag %d, tag %d, useful %d\n",
            i, tmp_index, tmp_tag, way.tag, way.useful);
    }

    if (provider_counts > 0) {
        use_alt_pred = main_entry.counter == 0 && alt_provided;
        use_base_table = main_entry.counter == 0 && !alt_provided;
        // if (!(!use_alt_pred || (use_alt_pred && alt_provided && provider_counts > 1))) {
        //     debugFlag = true;
        // }
        DPRINTF(FTBITTAGE || debugFlag,
                "lookup provided %d, provider_counts %d, main_table %d, main_table_index %d, use_alt %d\n", provided,
                provider_counts, main_table, main_table_index, use_alt_pred);
        assert(!use_alt_pred || (use_alt_pred && alt_provided && provider_counts > 1));
    } else {
        use_alt_pred = false;
        use_base_table = true;
    }
    DPRINTF(FTBITTAGE || debugFlag, "lookup provided %d, provider_counts %d, main_table %d, main_table_index %d, use_alt %d\n",
                provided, provider_counts, main_table, main_table_index, use_alt_pred);
    return std::make_pair(provided, alt_provided);
}

void
FTBITTAGE::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullFTBPrediction> &stagePreds) {
    // if (debugPC == stream_start) {
    //     debugFlag = true;
    // }
    DPRINTF(FTBITTAGE || debugFlag, "putPCHistory startAddr: %#lx\n", stream_start);
    TageEntry main_entry, alt_entry;
    main_entry.valid = false;
    alt_entry.valid = false;
    int main_table, alt_table;
    int main_table_index, alt_table_index;
    bool use_alt_pred, use_base_table;
    bitset usefulMask;

    bool taken = false;
    bool main_found, alt_found;
    // get prediction and save it
    std::tie(main_found, alt_found) = lookupHelper(stream_start, main_entry, main_table, main_table_index,
                                                   alt_entry, alt_table, alt_table_index, use_alt_pred, use_base_table, usefulMask);

    TagePrediction pred(main_found, alt_found, main_entry, alt_entry, main_table,
                        alt_table, main_table_index, alt_table_index, use_alt_pred,
                        use_base_table, usefulMask);
    
    DPRINTF(FTBITTAGE || debugFlag, "main_found %d, alt_found %d, main_table %d, alt_table %d, main_index %d, alt_index %d\n",
        main_found, alt_found, main_table, alt_table, main_table_index, alt_table_index);
    
    assert(getDelay() < stagePreds.size());
    assert(getDelay() >= 1);
    Addr base_target = stagePreds[getDelay()-1].indirectTarget;
    for (int s = getDelay(); s < stagePreds.size(); ++s) { // need modify
        Addr useTarget;
        if (main_found && !use_alt_pred && !use_base_table) {
            taken = main_entry.counter >= 2;
            useTarget = main_entry.target;
        } else if (alt_found && use_alt_pred) {
            taken = alt_entry.counter >= 2;
            useTarget = alt_entry.target;
        } else if (use_base_table) {
            taken = true;
            useTarget = base_target;
        } else {
            useTarget = base_target;
            warn("no target found\n");
        }
        DPRINTF(FTBITTAGE || debugFlag, "indirect target=%#lx\n", useTarget);
        if (taken) {
            stagePreds[s].indirectTarget = useTarget;
        }
    }

    meta.pred = pred;
    meta.tagFoldedHist = tagFoldedHist;
    meta.altTagFoldedHist = altTagFoldedHist;
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
    // if (debugPC == entry.startPC || debugPC2 == entry.startPC) {
    //     debugFlag = true;
    // }
    Addr startAddr = entry.getRealStartPC();
    DPRINTF(FTBITTAGE || debugFlag, "update startAddr: %#lx\n", startAddr);
    auto ftb_entry = entry.updateFTBEntry;

    // get tage predictions from meta
    // TODO: use component idx
    auto meta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    auto pred = meta->pred;
    auto updateTagFoldedHist = meta->tagFoldedHist;
    auto updateAltTagFoldedHist = meta->altTagFoldedHist;
    auto updateIndexFoldedHist = meta->indexFoldedHist;
    
    FTBSlot indirect_slot;
    for (auto slot : ftb_entry.slots) {
        if (slot.isIndirect) {
            indirect_slot = slot;
            break;
        }
    }
    // if the non-return indirect branch is actually executed, then we should update it
    bool should_update = entry.exeTaken && entry.exeBranchInfo == indirect_slot &&
        entry.exeBranchInfo.isIndirect && !entry.exeBranchInfo.isReturn;
    bool mispred = entry.squashType == SquashType::SQUASH_CTRL && entry.squashPC == indirect_slot.pc;
    if (should_update) {
        DPRINTF(FTBITTAGE || debugFlag, "try to update indirect pc %#lx \n", indirect_slot.pc);

        bool mainFound = pred.mainFound;
        Addr mainTarget = pred.mainEntry.target;

        // update useful bit, counter and predTaken for main entry
        if (mainFound) { // updateProvided
            DPRINTF(FTBITTAGE || debugFlag, "prediction provided by table %d, idx %d, updating corresponding entry\n",
                pred.main_table, pred.main_index);
            assert(pred.main_table < numPredictors && pred.main_index < tableSizes[pred.main_table]);
            auto &way = tageTable[pred.main_table][pred.main_index];

            // if (mainTarget != altTarget) { // updateAltDiffers
            //     way.useful = entry.exeBranchInfo.target == mainTarget; // updateProviderCorrect
            // }
            DPRINTF(FTBITTAGE || debugFlag, "useful bit set to %d\n", way.useful);

            updateCounter(entry.exeBranchInfo.target == mainTarget, 2, way.counter); // need modify
            if (way.counter == 0) {
                way.target = entry.exeBranchInfo.target;
            }
            bool altTaken = (pred.altFound && pred.altEntry.counter >= 2) || !pred.altFound;
            bool altDiffers = altTaken != (pred.mainEntry.counter >= 2);
            if (altDiffers) {
                way.useful = entry.exeBranchInfo.target == mainTarget;
            }

            if (pred.useAlt && mispred) {
                DPRINTF(FTBITTAGE, "prediction provided by alt table %d, idx %d, updating corresponding entry\n",
                    pred.alt_table, pred.alt_index);
                assert(pred.alt_table < numPredictors && pred.alt_index < tableSizes[pred.alt_table]);
                auto &alt_way = tageTable[pred.alt_table][pred.alt_index];
                updateCounter(false, 2, alt_way.counter);
                if (alt_way.counter == 0) {
                    alt_way.target = entry.exeBranchInfo.target;
                }
            }
        }

        // update base table counter
        // if (pred.useAlt) {
        //     unsigned base_idx = getBaseTableIndex(startAddr);
        //     DPRINTF(FTBITTAGE || debugFlag, "prediction provided by base table idx %d, updating corresponding entry\n", base_idx);
        //     baseTable.at(base_idx)[b].first = entry.exeBranchInfo.target;
        //     updateCounter(entry.exeBranchInfo.target == altTarget, 2, baseTable.at(base_idx)[b].second); // need mofigy
        // }

        // update use_alt_counters
        // if (pred.mainFound && (pred.counter == 0 || pred.counter == -1) &&
        //         mainTarget != altTarget) {
        //     auto &use_alt_counter = useAlt.at(getUseAltIdx(startAddr))[b];
        //     if (altTarget == entry.exeBranchInfo.target) {
        //         satIncrement(7, use_alt_counter);
        //     } else {
        //         satDecrement(-8, use_alt_counter);
        //     }
        //     DPRINTF(FTBITTAGE || debugFlag, "updating use_alt_counter %d\n", use_alt_counter);
        // }

        if (mispred) {
            DPRINTF(FTBITTAGE || debugFlag, "miss target=%#lx, correct target=%#lx\n", entry.predBranchInfo.target, entry.exeBranchInfo.target);
        } else {
            DPRINTF(FTBITTAGE || debugFlag, "hit target=%#lx, correct target=%#lx\n", entry.predBranchInfo.target, entry.exeBranchInfo.target);
        }
        // update useful reset counter
        bool use_alt_on_main_found_correct = (pred.useAlt || pred.useBase) && pred.mainFound && mainTarget == entry.exeBranchInfo.target;
        bool needToAllocate = mispred && !use_alt_on_main_found_correct;
        DPRINTF(FTBITTAGE || debugFlag, "mispred %d, use_alt_on_main_found_correct %d, needToAllocate %d\n",
            mispred, use_alt_on_main_found_correct, needToAllocate);

        int num_tables_can_allocate = (~pred.usefulMask).count();
        // int total_tables_to_allocate = pred.usefulMask.size();
        // bool incUsefulResetCounter = num_tables_can_allocate < (total_tables_to_allocate - num_tables_can_allocate);
        // bool decUsefulResetCounter = num_tables_can_allocate > (total_tables_to_allocate - num_tables_can_allocate);
        // int changeVal = std::abs(num_tables_can_allocate - (total_tables_to_allocate - num_tables_can_allocate));
        bool canAllocate = num_tables_can_allocate > 0;
        if (needToAllocate) {
            // if (incUsefulResetCounter) { // need modify: clear the useful bit of all entries
            //     usefulResetCnt[b] += changeVal;
            //     if (usefulResetCnt[b] >= 128)
            //         usefulResetCnt[b] = 128;
            //     DPRINTF(FTBITTAGE || debugFlag, "incUsefulResetCounter, changeVal %d, usefulResetCnt %d\n", changeVal, usefulResetCnt[b]);
            // } else if (decUsefulResetCounter) {
            //     usefulResetCnt[b] -= changeVal;
            //     if (usefulResetCnt[b] <= 0)
            //         usefulResetCnt[b] = 0;
            //     DPRINTF(FTBITTAGE || debugFlag, "decUsefulResetCounter, changeVal %d, usefulResetCnt %d\n", changeVal, usefulResetCnt[b]);
            // }

            if (canAllocate) {
                usefulResetCnt -= 1;
                if (usefulResetCnt <= 0) {
                    usefulResetCnt = 0;
                }
                DPRINTF(FTBITTAGE || debugFlag, "can allocate, usefulResetCnt %d\n", usefulResetCnt);
            } else {
                usefulResetCnt += 1;
                if (usefulResetCnt >= 256) {
                    usefulResetCnt = 256;
                }
                DPRINTF(FTBITTAGE || debugFlag, "can not allocate, usefulResetCnt %d\n", usefulResetCnt);
            }
            if (usefulResetCnt == 256) {
                DPRINTF(FTBITTAGE || debugFlag, "reset useful bit of all entries\n");
                for (auto &table : tageTable) {
                    for (auto &entry : table) {
                        entry.useful = 0;
                    }
                }
                usefulResetCnt = 0;
            }
        }

        if (needToAllocate) {
            // allocate new entry
            unsigned maskMaxNum = std::pow(2, (numPredictors - (pred.main_table + 1)));
            unsigned mask = allocLFSR.get() % maskMaxNum;
            bitset allocateLFSR(numPredictors - (pred.main_table + 1), mask);
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
                unsigned startTable = pred.main_table + 1;

                for (int ti = startTable; ti < numPredictors; ti++) {
                    Addr newIndex = getTageIndex(startAddr, ti, updateIndexFoldedHist[ti].get());
                    Addr newTag = getTageTag(startAddr, ti, updateTagFoldedHist[ti].get(), updateAltTagFoldedHist[ti].get());
                    assert(newIndex < tageTable[ti].size());
                    auto &newEntry = tageTable[ti][newIndex];

                    if (allocate[ti - startTable]) {
                        DPRINTF(FTBITTAGE || debugFlag, "found allocatable entry, table %d, index %d, tag %d, counter %d\n",
                            ti, newIndex, newTag, 2);
                        newEntry = TageEntry(newTag, entry.exeBranchInfo.target, 2);
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
    int max = (1 << (width)) - 1;
    int min = 0;
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

Addr
FTBITTAGE::getTageTag(Addr pc, int t, bitset &foldedHist, bitset &altFoldedHist)
{
    bitset buf(tableTagBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    bitset altTagBuf(altFoldedHist);
    altTagBuf.resize(tableTagBits[t]);
    altTagBuf <<= 1;
    buf ^= foldedHist;
    buf ^= altTagBuf;
    return buf.to_ulong();
}

Addr
FTBITTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get());
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
        for (int type = 0; type < 3; type++) {
            DPRINTF(FTBITTAGE || debugFlag, "t: %d, type: %d\n", t, type);

            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
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
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
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
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

void
FTBITTAGE::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

} // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
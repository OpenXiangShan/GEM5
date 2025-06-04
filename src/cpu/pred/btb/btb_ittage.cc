#include "cpu/pred/btb/btb_ittage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPUseful.hh"
#include "debug/ITTAGE.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

BTBITTAGE::BTBITTAGE(const Params& p):
TimedBaseBTBPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
maxHistLen(p.maxHistLen),
numTablesToAlloc(p.numTablesToAlloc)
{
    DPRINTF(ITTAGE, "BTBITTAGE constructor numBr=%d\n", numBr);
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

        tagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i], (int)16));
        altTagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i]-1, (int)16));
        indexFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableIndexBits[i], (int)16));
    }
    // useAlt.resize(128);
    // for (unsigned i = 0; i < useAlt.size(); ++i) {
    //     useAlt[i].resize(1, 0);
    // }
    usefulResetCnt = 0;
}

void
BTBITTAGE::tickStart()
{
}

void
BTBITTAGE::tick() {}

void
BTBITTAGE::lookupHelper(Addr startAddr, const std::vector<BTBEntry> &btbEntries, IndirectTargets& results)
{
    DPRINTF(ITTAGE, "lookupHelper startAddr: %#lx\n", startAddr);
    std::vector<TagePrediction> preds;
    for (auto &btb_entry : btbEntries) {
        if (btb_entry.isIndirect && !btb_entry.isReturn && btb_entry.valid) {
            DPRINTF(ITTAGE, "lookupHelper btbEntry: %#lx, always taken %d\n", btb_entry.pc, btb_entry.alwaysTaken);
            bool provided = false;
            bool alt_provided = false;

            TageTableInfo main_info, alt_info;

            for (int i = numPredictors - 1; i >= 0; --i) {
                auto &way = lookupEntries[i];
                // TODO: count alias hit (offset match but pc differs)
                bool match = way.valid && lookupTags[i] == way.tag && btb_entry.pc == way.pc;
                DPRINTF(ITTAGE, "hit %d, table %d, index %d, lookup tag %d, tag %d, useful %d, btb_pc %#lx, entry_pc %#lx\n",
                    match, i, lookupIndices[i], lookupTags[i], way.tag, way.useful, btb_entry.pc, way.pc);

                if (match) {
                    if (!provided) {
                        main_info = TageTableInfo(true, way, i, lookupIndices[i], lookupTags[i]);
                        provided = true;
                    } else if (!alt_provided) {
                        alt_info = TageTableInfo(true, way, i, lookupIndices[i], lookupTags[i]);
                        alt_provided = true;
                        break;
                    }
                }
            }

            Addr main_target = main_info.entry.target;
            Addr alt_target = alt_info.entry.target;

            Addr base_target = btb_entry.target;
            bool base_as_alt = false;


            Addr alt_pred = alt_provided ? alt_target : base_target;
            bool taken = false;
            Addr target;

            // TODO: dynamic control whether to use alt prediction
            bool main_weak = main_info.entry.counter == 0;
            bool use_alt_provider = main_weak && alt_provided;
            bool use_base = !provided || (provided && main_weak && !alt_provided);
            bool use_alt = use_alt_provider || use_base;
            if (provided && !main_weak) {
                taken = main_info.taken();
                target = main_target;
            } else if (alt_provided && use_alt_provider) {
                taken = alt_info.taken();
                target = alt_target;
            } else if (use_base) {
                taken = true;
                target = base_target;
            } else {
                target = base_target;
                warn("no target found\n");
            }
            if (taken) {
                results.push_back({btb_entry.pc, target});
            }
            DPRINTF(ITTAGE, "tage predict %#lx target %#lx\n", btb_entry.pc, target);

            TagePrediction pred(btb_entry.pc, main_info, alt_info, use_alt, main_target);
            meta->preds[btb_entry.pc] = pred;
        }
    }
}

void
BTBITTAGE::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    if (debugPC == stream_start) {
        debugFlag = true;
    }
    DPRINTF(ITTAGE, "putPCHistory startAddr: %#lx\n", stream_start);

    // clear old metas
    meta = std::make_shared<TageMeta>();
    // assign history for meta
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;

    lookupEntries.clear();
    lookupIndices.clear();
    lookupTags.clear();
    bitset useful_mask(numPredictors, false);
    // all btb entries should use the same lookup result
    // but each btb entry can use prediction from different tables
    for (int i = 0; i < numPredictors; ++i) {
        Addr index = getTageIndex(stream_start, i);
        Addr tag = getTageTag(stream_start, i);
        auto &entry = tageTable[i][index];
        lookupEntries.push_back(entry);
        lookupIndices.push_back(index);
        lookupTags.push_back(tag);
        useful_mask[i] = entry.useful;
        DPRINTF(ITTAGE, "lookup table %d[%d]: valid %d, tag %d, ctr %d, useful %d\n",
            i, index, entry.valid, entry.tag, entry.counter, entry.useful);
    }
    meta->usefulMask = std::move(useful_mask);

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        auto &stage_pred = stagePreds[s];
        stage_pred.indirectTargets.clear();
        lookupHelper(stream_start, stage_pred.btbEntries, stage_pred.indirectTargets);
    }
    DPRINTF(ITTAGE, "putPCHistory end\n");
    debugFlag = false;
}

std::shared_ptr<void>
BTBITTAGE::getPredictionMeta() {
    return meta;
}

void
BTBITTAGE::update(const FetchStream &stream)
{
    if (debugPC == stream.startPC || debugPC2 == stream.startPC) {
        debugFlag = true;
    }
    Addr startAddr = stream.getRealStartPC();
    DPRINTF(ITTAGE, "update startAddr: %#lx\n", startAddr);
    // update at the basis of btb entries
    auto all_entries_to_update = stream.updateBTBEntries;
    // only update conditional branches that is not always taken
    auto remove_it = std::remove_if(all_entries_to_update.begin(), all_entries_to_update.end(),
        [](const BTBEntry &e) { return !(e.isIndirect && !e.isReturn); });
    all_entries_to_update.erase(remove_it, all_entries_to_update.end());

    // check if pred btb miss branch need to be updated
    auto &potential_new_entry = stream.updateNewBTBEntry;
    if (!stream.updateIsOldEntry && !(potential_new_entry.isIndirect && !potential_new_entry.isReturn)) {
        all_entries_to_update.push_back(potential_new_entry);
    }

    // get tage predictions from meta
    // TODO: use component idx
    auto meta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    auto preds = meta->preds;
    auto updateTagFoldedHist = meta->tagFoldedHist;
    auto updateAltTagFoldedHist = meta->altTagFoldedHist;
    auto updateIndexFoldedHist = meta->indexFoldedHist;
    
    // update each branch
    for (auto &btb_entry : all_entries_to_update) {
        bool this_indirect_actual_taken = stream.exeTaken && stream.exeBranchInfo == btb_entry;
        auto pred_it = preds.find(btb_entry.pc);
        TagePrediction pred;
        if (pred_it != preds.end()) {
            pred = pred_it->second;
        }
        bool mispred = stream.squashType == SQUASH_CTRL && stream.squashPC == btb_entry.pc;
        Addr exe_target = stream.exeBranchInfo.target;
        auto &main_info = pred.mainInfo;
        bool &main_found = main_info.found;
        auto &main_counter = main_info.entry.counter;
        bool main_taken = main_info.taken();
        bool main_weak  = main_counter == 0;
        Addr main_target = main_info.entry.target;

        bool &used_alt = pred.useAlt;
        auto &alt_info = pred.altInfo;
        // update provider
        if (main_found) {
            DPRINTF(ITTAGE, "prediction provided by table %d, idx %d, updating corresponding entry\n",
                main_info.table, main_info.index);
            auto &way = tageTable[main_info.table][main_info.index];
            updateCounter(exe_target == main_target, 2, way.counter); // need modify
            if (way.counter == 0) {
                way.target = exe_target;
            }
            bool alt_taken = (alt_info.found && alt_info.taken()) || !pred.altInfo.found;
            bool alt_diff = alt_taken != main_taken;
            if (alt_diff) {
                way.useful = exe_target == main_target;
            }

            if (used_alt && mispred) {
                auto &alt_way = tageTable[pred.altInfo.table][pred.altInfo.index];
                updateCounter(false, 2, alt_way.counter);
                if (alt_way.counter == 0) {
                    alt_way.target = exe_target;
                }
            }
            DPRINTF(ITTAGE, "useful bit set to %d\n", way.useful);
        }

        bool use_alt_on_main_found_correct = used_alt && main_found && main_target == exe_target;
        bool needToAllocate = mispred && !use_alt_on_main_found_correct;
        DPRINTF(ITTAGE, "mispred %d, use_alt_on_main_found_correct %d, needToAllocate %d\n",
            mispred, use_alt_on_main_found_correct, needToAllocate);

        auto useful_mask = meta->usefulMask;
        int alloc_table_num = numPredictors - (main_info.found ? main_info.table + 1 : 0);
        if (main_found) {
            useful_mask >>= main_info.table + 1;
            useful_mask.resize(alloc_table_num);
        }
        int num_tables_can_allocate = (~useful_mask).count();
        bool canAllocate = num_tables_can_allocate > 0;
        if (needToAllocate) {
            if (canAllocate) {
                usefulResetCnt -= 1;
                if (usefulResetCnt <= 0) {
                    usefulResetCnt = 0;
                }
                DPRINTF(ITTAGE, "can allocate, usefulResetCnt %d\n", usefulResetCnt);
            } else {
                usefulResetCnt += 1;
                if (usefulResetCnt >= 256) {
                    usefulResetCnt = 256;
                }
                DPRINTF(ITTAGE, "can not allocate, usefulResetCnt %d\n", usefulResetCnt);
            }
            if (usefulResetCnt == 256) {
                DPRINTF(ITTAGE, "reset useful bit of all entries\n");
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
            unsigned maskMaxNum = std::pow(2, alloc_table_num);
            unsigned mask = allocLFSR.get() % maskMaxNum;
            bitset allocateLFSR(alloc_table_num, mask);

            auto flipped_usefulMask = useful_mask.flip();
            bitset masked = allocateLFSR & flipped_usefulMask;
            bitset allocate = masked.any() ? masked : flipped_usefulMask;
            if (debugFlag) {
                std::string buf;
                boost::to_string(allocateLFSR, buf);
                DPRINTF(ITTAGE, "allocateLFSR %s, size %d\n", buf, allocateLFSR.size());
                boost::to_string(flipped_usefulMask, buf);
                DPRINTF(ITTAGE, "pred usefulmask %s, size %d\n", buf, useful_mask.size());
                boost::to_string(masked, buf);
                DPRINTF(ITTAGE, "masked %s, size %d\n", buf, masked.size());
                boost::to_string(allocate, buf);
                DPRINTF(ITTAGE, "allocate %s, size %d\n", buf, allocate.size());
            }

            bool allocateValid = flipped_usefulMask.any();
            if (needToAllocate && allocateValid) {
                DPRINTF(ITTAGE, "allocate new entry\n");
                unsigned startTable = main_found ? main_info.table + 1 : 0;

                for (int ti = startTable; ti < numPredictors; ti++) {
                    Addr newIndex = getTageIndex(startAddr, ti, updateIndexFoldedHist[ti].get());
                    Addr newTag = getTageTag(startAddr, ti, updateTagFoldedHist[ti].get(), updateAltTagFoldedHist[ti].get());
                    assert(newIndex < tageTable[ti].size());
                    auto &newEntry = tageTable[ti][newIndex];

                    if (allocate[ti - startTable]) {
                        DPRINTF(ITTAGE, "found allocatable entry, table %d, index %d, tag %d, counter %d\n",
                            ti, newIndex, newTag, 2);
                        newEntry = TageEntry(newTag, exe_target, 2, btb_entry.pc);
                        break; // allocate only 1 entry
                    }
                }
            }
        }
    }

    DPRINTF(ITTAGE, "end update\n");
    debugFlag = false;
}

void
BTBITTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width)) - 1;
    int min = 0;
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

Addr
BTBITTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist)
{
    // Create mask for tableTagBits[t]
    uint64_t mask = ((1ULL << tableTagBits[t]) - 1);

    // Extract lower bits of PC
    uint64_t pcBits = (pc >> floorLog2(blockSize));

    // Prepare alt tag: shift left by 1 and mask
    uint64_t altTagBits = (altFoldedHist << 1);

    // XOR all components
    return (pcBits ^ foldedHist ^ altTagBits) & mask;
}

Addr
BTBITTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get());
}

Addr
BTBITTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist)
{
    // Create mask for tableIndexBits[t]
    uint64_t mask = ((1ULL << tableIndexBits[t]) - 1);

    // Extract lower bits of PC and XOR with folded history
    uint64_t pcBits = (pc >> floorLog2(blockSize));
    return (pcBits ^ foldedHist) & mask;
}

Addr
BTBITTAGE::getTageIndex(Addr pc, int t)
{
    return getTageIndex(pc, t, indexFoldedHist[t].get());
}

bool
BTBITTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
BTBITTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBITTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

void
BTBITTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken)
{
    if (debugFlag) {
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(ITTAGE, "in doUpdateHist, shamt %d, taken %d, history %s\n", shamt, taken, buf);
    }
    if (shamt == 0) {
        DPRINTF(ITTAGE, "shamt is 0, returning\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            DPRINTF(ITTAGE, "t: %d, type: %d\n", t, type);

            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.update(history, shamt, taken);
        }
    }
}

void
BTBITTAGE::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getHistInfo();
    doUpdateHist(history, shamt, cond_taken);
}

void
BTBITTAGE::recoverHist(const boost::dynamic_bitset<> &history,
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
BTBITTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    if (debugFlag) {
        DPRINTF(ITTAGE, "checking folded history when %s\n", when);
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(ITTAGE, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 2; type++) {
            DPRINTF(ITTAGE, "t: %d, type: %d\n", t, type);
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

void
BTBITTAGE::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

} // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

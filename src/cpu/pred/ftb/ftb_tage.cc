#include "cpu/pred/ftb/ftb_tage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "debug/FTBTAGE.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred{

FTBTAGE::FTBTAGE(const Params& p)
    : TimedBaseFTBPredictor(p),
      numPredictors(p.numPredictors),
      baseTableSize(p.baseTableSize),
      tableSizes(p.tableSizes),
      tableTagBits(p.TTagBitSizes),
      tablePcShifts(p.TTagPcShifts),
      histLengths(p.histLengths),
      maxHistLen(p.maxHistLen),
      numTablesToAlloc(p.numTablesToAlloc),
      numBr(p.numBr),
      enableSC(p.enableSC),
      sc(p.numBr, this)
{
    tageBankStats = new TageBankStats * [numBr];
    for (int i = 0; i < numBr; i++) {
        tageBankStats[i] = new TageBankStats(this, 
            (std::string("bank_")+std::to_string(i)).c_str(),
            numPredictors);
    }

    DPRINTF(FTBTAGE, "FTBTAGE constructor\n");
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    baseTable.resize(baseTableSize);
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        for (int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numBr);
        }


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
    for (unsigned i = 0; i < baseTable.size(); ++i) {
        baseTable[i].resize(numBr);
    }
    usefulResetCnt.resize(numBr, 0);

    useAlt.resize(128);
    for (unsigned i = 0; i < useAlt.size(); ++i) {
        useAlt[i].resize(numBr, 0);
    }
    
    std::vector<TageBankStats *> statsPtr;
    for (int i = 0; i < numBr; i++) {
        statsPtr.push_back(tageBankStats[i]);
    }
    sc.setStats(statsPtr);

    hasDB = true;
    dbName = std::string("tage");
}

FTBTAGE::~FTBTAGE()
{
    for (int i = 0; i < numBr; i++) {
        delete tageBankStats[i];
    }
    delete [] tageBankStats;
}

void
FTBTAGE::setTrace()
{
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("startPC", UINT64),
            std::make_pair("branchPC", UINT64),
            std::make_pair("lgcBank", UINT64),
            std::make_pair("phyBank", UINT64),
            std::make_pair("mainFound", UINT64),
            std::make_pair("mainCounter", UINT64),
            std::make_pair("mainUseful", UINT64),
            std::make_pair("altCounter", UINT64),
            std::make_pair("mainTable", UINT64),
            std::make_pair("mainIndex", UINT64),
            std::make_pair("altIndex", UINT64),
            std::make_pair("tag", UINT64),
            std::make_pair("useAlt", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("allocSuccess", UINT64),
            std::make_pair("allocFailure", UINT64),
            std::make_pair("predUseSC", UINT64),
            std::make_pair("predSCDisagree", UINT64),
            std::make_pair("predSCCorrect", UINT64)
        };
        tageMissTrace = _db->addAndGetTrace("TAGEMISSTRACE", fields_vec);
        tageMissTrace->init_table();
    }
}

void
FTBTAGE::tick() {}

void
FTBTAGE::tickStart() {}

std::vector<bool>
FTBTAGE::lookupHelper(Addr startAddr,
                      std::vector<TageEntry> &main_entries,
                      std::vector<int> &main_tables,
                      std::vector<int> &main_table_indices,
                      std::vector<bool> &use_alt_preds, std::vector<bitset> &usefulMasks)
{
    // DPRINTF(FTBTAGE, "lookupHelper startAddr: %#lx\n", startAddr);
    for (auto &t : main_tables) { t = -1; }
    for (auto &t : main_table_indices) { t = -1; }

    std::vector<bool> provided;
    provided.resize(numBr, false);

    for (int b = 0; b < numBr; b++) {
        // make main prediction
        int phyBrIdx = getShuffledBrIndex(startAddr, b);
        int provider_counts = 0;
        for (int i = numPredictors - 1; i >= 0; --i) {
            Addr tmp_index = getTageIndex(startAddr, i);
            Addr tmp_tag = getTageTag(startAddr, i);
            auto &way = tageTable[i][tmp_index][phyBrIdx];
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
            DPRINTF(FTBTAGE, "table %d, index %d, lookup tag %d, tag %d, useful %d\n",
                i, tmp_index, tmp_tag, way.tag, way.useful);
        }


        if (provider_counts > 0) {
            auto main_entry = main_entries[b];
            // in RTL, we do not shuffle on useAltCtrs
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
        DPRINTF(FTBTAGE, "lookup startAddr %#lx cond %d, provider_counts %d, main_table %d, main_table_index %d, use_alt %d\n",
                    startAddr, b, provider_counts, main_tables[b], main_table_indices[b], use_alt_preds[b]);
    }
    return provided;
}

void
FTBTAGE::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullFTBPrediction> &stagePreds) {
    // DPRINTF(FTBTAGE, "putPCHistory startAddr: %#lx\n", stream_start);
    std::vector<TageEntry> entries;
    entries.resize(numBr);

    for (int i = 0; i < numBr; ++i) {
        entries[i].valid = false;
    }
    std::vector<int> main_tables;
    std::vector<int> main_table_indices;
    std::vector<bool> use_alt_preds;
    std::vector<bitset> usefulMasks;
    std::vector<bool> takens;
    main_tables.resize(numBr, -1);
    main_table_indices.resize(numBr, -1);
    use_alt_preds.resize(numBr, false);
    usefulMasks.resize(numBr);
    takens.resize(numBr, false);

    // get prediction and save it
    std::vector<bool> found = lookupHelper(stream_start, entries, main_tables,
                                    main_table_indices, use_alt_preds, usefulMasks);


    std::vector<short> altRes = baseTable.at(getBaseTableIndex(stream_start));

    std::vector<TagePrediction> preds;
    preds.resize(numBr);
    for (int b = 0; b < numBr; ++b) {
        int phyBrIdx = getShuffledBrIndex(stream_start, b);
        takens[b] = use_alt_preds[b] ? altRes[phyBrIdx] >= 0 : entries[b].counter >= 0;
        preds[b] = TagePrediction(found[b], entries[b].counter, entries[b].useful, altRes[phyBrIdx],
            main_tables[b], main_table_indices[b], entries[b].tag, use_alt_preds[b],
            usefulMasks[b], takens[b]);
        tageBankStats[b]->updateStatsWithTagePrediction(preds[b], true);
    }

    // sc prediction
    if (enableSC) {
        auto scPreds = sc.getPredictions(stream_start, preds);
        for (int i = 0; i < numBr; ++i) {
            takens[i] = scPreds[i].scPred;
        }
        meta.scMeta.scPreds = scPreds;
        meta.scMeta.indexFoldedHist = sc.getFoldedHist();
    }
    assert(getDelay() < stagePreds.size());
    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        // assume that ftb entry is provided in stagePreds

        for (int i = 0; i < numBr; ++i) {
            // always taken logic
            // TODO: move to bpu
            auto &entry = stagePreds[s].ftbEntry;
            if (entry.slots.size() > i) {
                takens[i] = takens[i] || entry.slots[i].alwaysTaken;
            }
            stagePreds[s].condTakens[i] = takens[i];
        }
    }

    meta.preds = preds;
    meta.tagFoldedHist = tagFoldedHist;
    meta.altTagFoldedHist = altTagFoldedHist;
    meta.indexFoldedHist = indexFoldedHist;
    // DPRINTF(FTBTAGE, "putPCHistory end\n");
}

std::shared_ptr<void>
FTBTAGE::getPredictionMeta() {
    std::shared_ptr<void> meta_void_ptr = std::make_shared<TageMeta>(meta);
    return meta_void_ptr;
}

void
FTBTAGE::update(const FetchStream &entry)
{

    Addr startAddr = entry.getRealStartPC();
    DPRINTF(FTBTAGE, "update startAddr: %#lx\n", startAddr);
    std::vector<bool> need_to_update;
    need_to_update.resize(numBr, false);
    auto ftb_entry = entry.updateFTBEntry;

    // get number of conditional branches to update
    int cond_num = 0;
    if (entry.exeTaken) {
        cond_num = ftb_entry.getNumCondInEntryBefore(entry.exeBranchInfo.pc);
        // for case of ftb entry is not full
        if (cond_num < numBr) {
            cond_num += !entry.exeBranchInfo.isUncond() ? 1 : 0;
        }
        // if ftb entry is full, and this branch is conditional,
        // we cannot update the last branch, as it will be removed
        // from current ftb entry
    } else {
        // corresponding to RTL, but in fact we should consider
        // whether the branches are flushed
        // TODO: fix it and check whether it can bring performance improvement
        cond_num = ftb_entry.getTotalNumConds();
    }
    assert(cond_num <= numBr);
    for (int i = 0; i < cond_num; i++) {
        auto &slot = ftb_entry.slots[i];
        // only update branches with both taken/not taken behaviors observed
        need_to_update[i] = !slot.alwaysTaken;
    }
    // DPRINTF(FTBTAGE, "need to update size %d\n", need_to_update.size());

    // get tage predictions from meta
    auto meta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    auto preds = meta->preds;
    auto scMeta = meta->scMeta;
    std::vector<bool> actualTakens;
    actualTakens.resize(numBr, false);

    auto updateTagFoldedHist = meta->tagFoldedHist;
    auto updateAltTagFoldedHist = meta->altTagFoldedHist;
    auto updateIndexFoldedHist = meta->indexFoldedHist;
    for (int b = 0; b < numBr; b++) {
        DPRINTF(FTBTAGE, "try to update cond %d \n", b);
        if (!need_to_update[b]) {
            DPRINTF(FTBTAGE, "no need to update, skipping\n");
            // we could also use break
            // because we assume that the branches are in order
            // if the first branch is not executed (need to update), the following branches
            // will not be executed either, thus we don't need to update them
            continue;
        }
        // DPRINTF(FTBTAGE, "Update cond %d, pc %#lx, predTaken %d, actualTaken %d,"
        //     " main found %d, main table %d, main table index %d, tag %#lx, main counter %d, main counter now %d,"
        //     " use alt %d, alt table index %d, alt counter %d, alt counter now  %d, useful mask %#x, ")

        auto stat = tageBankStats[b];
        stat->updateStatsWithTagePrediction(preds[b], false);
        bool this_cond_actually_taken = entry.exeTaken && entry.exeBranchInfo == ftb_entry.slots[b];
        actualTakens[b] = this_cond_actually_taken;

        int phyBrIdx = getShuffledBrIndex(startAddr, b);
        TagePrediction pred = preds[b];
        bool mainFound = pred.mainFound;
        bool mainTaken = pred.mainCounter >= 0;
        bool mainWeak = pred.mainCounter == 0 || pred.mainCounter == -1;
        bool altTaken = baseTable.at(getBaseTableIndex(startAddr))[phyBrIdx] >= 0;

        // update useful bit, counter and predTaken for main entry
        if (mainFound) { // updateProvided
            DPRINTF(FTBTAGE, "prediction provided by table %d, idx %d, updating corresponding entry\n",
                pred.table, pred.index);
            auto &way = tageTable[pred.table][pred.index][phyBrIdx];

            if (mainTaken != altTaken) { // updateAltDiffers
                way.useful = this_cond_actually_taken == mainTaken; // updateProviderCorrect
            }
            DPRINTF(FTBTAGE, "useful bit set to %d\n", way.useful);

            updateCounter(this_cond_actually_taken, 3, way.counter);
        }

        // update base table counter
        if (pred.useAlt) {
            unsigned base_idx = getBaseTableIndex(startAddr);
            DPRINTF(FTBTAGE, "prediction provided by base table idx %d, updating corresponding entry\n", base_idx);
            updateCounter(this_cond_actually_taken, 2, baseTable.at(base_idx)[phyBrIdx]);
        }

        // update use_alt_counters
        if (pred.mainFound && mainWeak && mainTaken != altTaken) {
            DPRINTF(FTBTAGE, "use_alt_on_provider_weak, alt %s, updating use_alt_counter\n",
                altTaken == this_cond_actually_taken ? "correct" : "incorrect");
            auto &use_alt_counter = useAlt.at(getUseAltIdx(startAddr))[b];
            if (altTaken == this_cond_actually_taken) {
                stat->updateUseAltOnNaInc++;
                satIncrement(7, use_alt_counter);
            } else {
                stat->updateUseAltOnNaDec++;
                satDecrement(-8, use_alt_counter);
            }
        }

        // stats
        if (!pred.mainFound || pred.useAlt) {
            bool altTaken = pred.altCounter >= 0;
            bool mainTaken = pred.mainCounter >= 0;
            bool altCorrect = altTaken == this_cond_actually_taken;
            bool altDiffers = altTaken != mainTaken;
            if (altCorrect) {
                stat->updateUseAltCorrect++;
            } else {
                stat->updateUseAltWrong++;
            }
            if (altDiffers) {
                stat->updateAltDiffers++;
            }
        }

        if (pred.mainFound && mainWeak) {
            stat->updateProviderNa++;
            if (!pred.useAlt) {
                bool mainCorrect = mainTaken == this_cond_actually_taken;
                if (mainCorrect) {
                    stat->updateUseNaCorrect++;
                } else {
                    stat->updateUseNaWrong++;
                }
            } else {
                stat->updateUseAltOnNa++;
                bool altCorrect = altTaken == this_cond_actually_taken;
                if (altCorrect) {
                    stat->updateUseAltOnNaCorrect++;
                } else {
                    stat->updateUseAltOnNaWrong++;
                }
            }
        }

        DPRINTF(FTBTAGE, "squashType %d, squashPC %#lx, slot pc %#lx\n", entry.squashType, entry.squashPC, ftb_entry.slots[b].pc);
        bool this_cond_mispred = entry.squashType == SquashType::SQUASH_CTRL && entry.squashPC == ftb_entry.slots[b].pc;
        if (this_cond_mispred) {
            stat->updateMispred++; // TODO: count tage predictions instead of overall predictions
            if (!pred.useAlt && pred.mainFound) {
                stat->updateTableMispreds[pred.table]++;
            }
        }
        assert(!this_cond_mispred || ftb_entry.slots[b].condValid());
        // update useful reset counter
        bool use_alt_on_main_found_correct = pred.useAlt && pred.mainFound && mainTaken == this_cond_actually_taken;
        bool needToAllocate = this_cond_mispred && !use_alt_on_main_found_correct;
        DPRINTF(FTBTAGE, "this_cond_mispred %d, use_alt_on_main_found_correct %d, needToAllocate %d\n",
            this_cond_mispred, use_alt_on_main_found_correct, needToAllocate);

        int num_tables_can_allocate = (~pred.usefulMask).count();
        int total_tables_to_allocate = pred.usefulMask.size();
        bool incUsefulResetCounter = num_tables_can_allocate < (total_tables_to_allocate - num_tables_can_allocate);
        bool decUsefulResetCounter = num_tables_can_allocate > (total_tables_to_allocate - num_tables_can_allocate);
        int changeVal = std::abs(num_tables_can_allocate - (total_tables_to_allocate - num_tables_can_allocate));
        if (needToAllocate) {
            if (incUsefulResetCounter) { // need modify: clear the useful bit of all entries
                stat->updateResetUCtrInc.sample(changeVal, 1);
                usefulResetCnt[b] += changeVal;
                if (usefulResetCnt[b] >= 128) {
                    usefulResetCnt[b] = 128;
                }
                // usefulResetCnt[b] = (usefulResetCnt[b] + changeVal >= 128) ? 128 : (usefulResetCnt[b] + changeVal);
                DPRINTF(FTBTAGEUseful, "incUsefulResetCounter %d, changeVal %d, usefulResetCnt %d\n", b, changeVal, usefulResetCnt[b]);
            } else if (decUsefulResetCounter) {
                stat->updateResetUCtrDec.sample(changeVal, 1);
                usefulResetCnt[b] -= changeVal;
                if (usefulResetCnt[b] <= 0) {
                    usefulResetCnt[b] = 0;
                }
                // usefulResetCnt[b] = (usefulResetCnt[b] - changeVal <= 0) ? 0 : (usefulResetCnt[b] - changeVal);
                DPRINTF(FTBTAGEUseful, "decUsefulResetCounter %d, changeVal %d, usefulResetCnt %d\n", b, changeVal, usefulResetCnt[b]);
            }
            if (usefulResetCnt[b] == 128) {
                stat->updateResetU++;
                DPRINTF(FTBTAGEUseful, "reset useful bit of all entries\n");
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

        bool allocSuccess, allocFailure;
        if (needToAllocate) {
            // allocate new entry
            unsigned maskMaxNum = std::pow(2, (numPredictors - (pred.table + 1)));
            unsigned mask = allocLFSR.get() % maskMaxNum;
            bitset allocateLFSR(numPredictors - (pred.table + 1), mask);
            std::string buf;
            auto flipped_usefulMask = ~pred.usefulMask;
            bitset masked = allocateLFSR & flipped_usefulMask;
            bitset allocate = masked.any() ? masked : flipped_usefulMask;
            short newCounter = this_cond_actually_taken ? 0 : -1;

            bool allocateValid = flipped_usefulMask.any();
            if (allocateValid) {
                DPRINTF(FTBTAGE, "allocate new entry\n");
                stat->updateAllocSuccess++;
                allocSuccess = true;
                unsigned startTable = pred.table + 1;

                for (int ti = startTable; ti < numPredictors; ti++) {
                    Addr newIndex = getTageIndex(startAddr, ti, updateIndexFoldedHist[ti].get());
                    Addr newTag = getTageTag(startAddr, ti, updateTagFoldedHist[ti].get(), updateAltTagFoldedHist[ti].get());
                    auto &entry = tageTable[ti][newIndex][phyBrIdx];

                    if (allocate[ti - startTable]) {
                        DPRINTF(FTBTAGE, "found allocatable entry, table %d, index %d, tag %d, counter %d\n",
                            ti, newIndex, newTag, newCounter);
                        entry = TageEntry(newTag, newCounter);
                        break; // allocate only 1 entry
                    }
                }
            } else {
                allocFailure = true;
                stat->updateAllocFailure++;
            }
        }
        if (enableDB) {
            TageMissTrace t;
            if (enableSC) {
                t.set(startAddr, ftb_entry.slots[b].pc, b, phyBrIdx, mainFound, pred.mainCounter,
                    pred.mainUseful, pred.altCounter, pred.table, pred.index, getBaseTableIndex(startAddr),
                    pred.tag, pred.useAlt, pred.taken, this_cond_actually_taken, allocSuccess, allocFailure,
                    scMeta.scPreds[b].scUsed, scMeta.scPreds[b].scPred != scMeta.scPreds[b].tageTaken,
                    scMeta.scPreds[b].scPred == this_cond_actually_taken);
            } else {
                t.set(startAddr, ftb_entry.slots[b].pc, b, phyBrIdx, mainFound, pred.mainCounter,
                    pred.mainUseful, pred.altCounter, pred.table, pred.index, getBaseTableIndex(startAddr),
                    pred.tag, pred.useAlt, pred.taken, this_cond_actually_taken, allocSuccess, allocFailure,
                    0, 0, 0);
            }
            tageMissTrace->write_record(t);
        }
    }

    // update sc
    if (enableSC) {
        sc.update(startAddr, scMeta, need_to_update, actualTakens);
    }
    DPRINTF(FTBTAGE, "end update\n");
}

void
FTBTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width-1)) - 1;
    int min = -(1 << (width-1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

Addr
FTBTAGE::getTageTag(Addr pc, int t, bitset &foldedHist, bitset &altFoldedHist)
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
FTBTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get());
}

Addr
FTBTAGE::getTageIndex(Addr pc, int t, bitset &foldedHist)
{
    bitset buf(tableIndexBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf ^= foldedHist;
    return buf.to_ulong();
}

Addr
FTBTAGE::getTageIndex(Addr pc, int t)
{
    return getTageIndex(pc, t, indexFoldedHist[t].get());
}

Addr
FTBTAGE::getBrIndexUnshuffleBits(Addr pc)
{
    return (pc >> instShiftAmt) & ((1 << ceilLog2(numBr)) - 1);
}

Addr
FTBTAGE::getShuffledBrIndex(Addr pc, int brIdxToShuffle)
{
    return getBrIndexUnshuffleBits(pc) ^ brIdxToShuffle;
}


unsigned
FTBTAGE::getBaseTableIndex(Addr pc) {
    return (pc >> instShiftAmt) % baseTable.size();
}

bool
FTBTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
FTBTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
FTBTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

Addr
FTBTAGE::getUseAltIdx(Addr pc) {
    return (pc >> instShiftAmt) & (useAlt.size() - 1); // need modify
}

void
FTBTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken)
{
    DPRINTF(FTBTAGE, "in doUpdateHist, shamt %d, taken %d, history %s\n", shamt, taken, history);
    if (shamt == 0) {
        DPRINTF(FTBTAGE, "shamt is 0, returning\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            DPRINTF(FTBTAGE, "t: %d, type: %d\n", t, type);

            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.update(history, shamt, taken);
        }
    }
}

void
FTBTAGE::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getHistInfo();
    doUpdateHist(history, shamt, cond_taken);
    if (enableSC) {
        sc.doUpdateHist(history, shamt, cond_taken);
    }
}

void
FTBTAGE::recoverHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken);
    if (enableSC) {
        sc.recoverHist(predMeta->scMeta.indexFoldedHist);
        sc.doUpdateHist(history, shamt, cond_taken);
    }
}

void
FTBTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {

            // DPRINTF(FTBTAGE, "t: %d, type: %d\n", t, type);
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

std::vector<FTBTAGE::StatisticalCorrector::SCPrediction>
FTBTAGE::StatisticalCorrector::getPredictions(Addr pc, std::vector<TagePrediction> &tagePreds)
{
    std::vector<int> scSums = {0,0};
    std::vector<int> tageCtrCentereds;
    std::vector<SCPrediction> scPreds;
    std::vector<bool> sumAboveThresholds;
    scPreds.resize(numBr);
    sumAboveThresholds.resize(numBr);
    for (int b = 0; b < numBr; b++) {
        int phyBrIdx = tage->getShuffledBrIndex(pc, b);
        std::vector<int> scOldCounters;
        tageCtrCentereds.push_back((2 * tagePreds[b].mainCounter + 1) * 8);
        for (int i = 0;i < scCntTable.size();i++) {
            int index = getIndex(pc, i);
            int tOrNt = tagePreds[b].taken ? 1 : 0;
            int ctr = scCntTable[i][index][phyBrIdx][tOrNt];
            scSums[b] += 2 * ctr + 1;
        }
        scSums[b] += tageCtrCentereds[b];
        sumAboveThresholds[b] = abs(scSums[b]) > thresholds[b];

        scPreds[b].tageTaken = tagePreds[b].taken;
        scPreds[b].scUsed = tagePreds[b].mainFound;
        scPreds[b].scPred = tagePreds[b].mainFound && sumAboveThresholds[b] ?
            scSums[b] >= 0 : tagePreds[b].taken;
        scPreds[b].scSum = scSums[b];

        // stats
        auto &stat = stats[b];
        if (tagePreds[b].mainFound) {
            stat->scUsedAtPred++;
            if (sumAboveThresholds[b]) {
                stat->scConfAtPred++;
                if (scPreds[b].scPred == scPreds[b].tageTaken) {
                    stat->scAgreeAtPred++;
                } else {
                    stat->scDisagreeAtPred++;
                }
            } else {
                stat->scUnconfAtPred++;
            }
        }
    }
    return scPreds;
}

std::vector<FoldedHist>
FTBTAGE::StatisticalCorrector::getFoldedHist()
{
    return foldedHist;
}

bool
FTBTAGE::StatisticalCorrector::satPos(int &counter, int counterBits)
{
    return counter == ((1 << (counterBits-1)) - 1);
}

bool
FTBTAGE::StatisticalCorrector::satNeg(int &counter, int counterBits)
{
    return counter == -(1 << (counterBits-1));
}

Addr
FTBTAGE::StatisticalCorrector::getIndex(Addr pc, int t)
{
    return getIndex(pc, t, foldedHist[t].get());
}

Addr
FTBTAGE::StatisticalCorrector::getIndex(Addr pc, int t, bitset &foldedHist)
{
    bitset buf(tableIndexBits[t], pc >> tablePcShifts[t]);  // lower bits of PC
    buf ^= foldedHist;
    return buf.to_ulong();
}

void
FTBTAGE::StatisticalCorrector::counterUpdate(int &ctr, int nbits, bool taken)
{
    if (taken) {
		if (ctr < ((1 << (nbits-1)) - 1))
			ctr++;
	} else {
		if (ctr > -(1 << (nbits-1)))
			ctr--;
    }
}

void
FTBTAGE::StatisticalCorrector::update(Addr pc, SCMeta meta, std::vector<bool> needToUpdates,
    std::vector<bool> actualTakens)
{
    auto predHist = meta.indexFoldedHist;
    auto preds = meta.scPreds;

    for (int b = 0; b < numBr; b++) {
        if (!needToUpdates[b]) {
            continue;
        }
        int phyBrIdx = tage->getShuffledBrIndex(pc, b);
        auto &p = preds[b];
        bool scTaken = p.scPred;
        bool actualTaken = actualTakens[b];
        int tOrNt = p.tageTaken ? 1 : 0;
        int sumAbs = std::abs(p.scSum);
        // perceptron update
        if (p.scUsed) {
            if (sumAbs <= (thresholds[b] * 8 + 21) || scTaken != actualTaken) {
                for (int i = 0; i < numPredictors; i++) {
                    auto idx = getIndex(pc, i, predHist[i].get());
                    auto &ctr = scCntTable[i][idx][phyBrIdx][tOrNt];
                    counterUpdate(ctr, scCounterWidth, actualTaken);
                }
                if (scTaken != actualTaken) {
                    stats[b]->scUpdateOnMispred++;
                } else {
                    stats[b]->scUpdateOnUnconf++;
                }
            }

            if (scTaken != p.tageTaken && sumAbs >= thresholds[b] - 4 && sumAbs <= thresholds[b] - 2) {

                bool cause = scTaken != actualTaken;
                counterUpdate(TCs[b], TCWidth, cause);
                if (satPos(TCs[b], TCWidth) && thresholds[b] <= maxThres) {
                    thresholds[b] += 2;
                } else if (satNeg(TCs[b], TCWidth) && thresholds[b] >= minThres) {
                    thresholds[b] -= 2;
                }

                if (satPos(TCs[b], TCWidth) || satNeg(TCs[b], TCWidth)) {
                    TCs[b] = neutralVal;
                }
            }

            // stats
            auto &stat = stats[b];
            // bool sumAboveUpdateThreshold = sumAbs >= (thresholds[b] * 8 + 21);
            bool sumAboveUseThreshold = sumAbs >= thresholds[b];
            stat->scUsedAtCommit++;
            if (sumAboveUseThreshold) {
                stat->scConfAtCommit++;
                if (scTaken == p.tageTaken) {
                    stat->scAgreeAtCommit++;
                } else {
                    stat->scDisagreeAtCommit++;
                    if (scTaken == actualTaken) {
                        stat->scCorrectTageWrong++;
                    } else {
                        stat->scWrongTageCorrect++;
                    }
                }
            } else {
                stat->scUnconfAtCommit++;
            }
        }

    }
}

void
FTBTAGE::StatisticalCorrector::recoverHist(std::vector<FoldedHist> &fh)
{
    for (int i = 0; i < numPredictors; i++) {
        foldedHist[i].recover(fh[i]);
    }
}

void
FTBTAGE::StatisticalCorrector::doUpdateHist(const boost::dynamic_bitset<> &history,
    int shamt, bool cond_taken)
{
    if (shamt == 0) {
        return;
    }
    for (int t = 0; t < numPredictors; t++) {
        foldedHist[t].update(history, shamt, cond_taken);
    }
}

FTBTAGE::TageBankStats::TageBankStats(statistics::Group* parent, const char *name, int numPredictors):
    statistics::Group(parent, name),
    ADD_STAT(predTableHits, statistics::units::Count::get(), "hit of each tage table on prediction"),
    ADD_STAT(predNoHitUseBim, statistics::units::Count::get(), "use bimodal when no hit on prediction"),
    ADD_STAT(predUseAlt, statistics::units::Count::get(), "use alt on prediction"),
    ADD_STAT(updateTableHits, statistics::units::Count::get(), "hit of each tage table on update"),
    ADD_STAT(updateNoHitUseBim, statistics::units::Count::get(), "use bimodal when no hit on update"),
    ADD_STAT(updateUseAlt, statistics::units::Count::get(), "use alt on update"),
    ADD_STAT(updateUseAltCorrect, statistics::units::Count::get(), "use alt on update and correct"),
    ADD_STAT(updateUseAltWrong, statistics::units::Count::get(), "use alt on update and wrong"),
    ADD_STAT(updateAltDiffers, statistics::units::Count::get(), "alt differs on update"),
    ADD_STAT(updateUseAltOnNaUpdated, statistics::units::Count::get(), "use alt on na ctr updated when update"),
    ADD_STAT(updateUseAltOnNaInc, statistics::units::Count::get(), "use alt on na ctr inc when update"),
    ADD_STAT(updateUseAltOnNaDec, statistics::units::Count::get(), "use alt on na ctr dec when update"),
    ADD_STAT(updateProviderNa, statistics::units::Count::get(), "provider weak when update"),
    ADD_STAT(updateUseNaCorrect, statistics::units::Count::get(), "use na on update and correct"),
    ADD_STAT(updateUseNaWrong, statistics::units::Count::get(), "use na on update and wrong"),
    ADD_STAT(updateUseAltOnNa, statistics::units::Count::get(), "use alt on na when update"),
    ADD_STAT(updateUseAltOnNaCorrect, statistics::units::Count::get(), "use alt on na correct when update"),
    ADD_STAT(updateUseAltOnNaWrong, statistics::units::Count::get(), "use alt on na wrong when update"),
    ADD_STAT(updateAllocFailure, statistics::units::Count::get(), "alloc failure when update"),
    ADD_STAT(updateAllocSuccess, statistics::units::Count::get(), "alloc success when update"),
    ADD_STAT(updateMispred, statistics::units::Count::get(), "mispred when update"),
    ADD_STAT(updateResetU, statistics::units::Count::get(), "reset u when update"),
    ADD_STAT(updateResetUCtrInc, statistics::units::Count::get(), "reset u ctr inc when update"),
    ADD_STAT(updateResetUCtrDec, statistics::units::Count::get(), "reset u ctr dec when update"),
    ADD_STAT(updateTableMispreds, statistics::units::Count::get(), "mispreds of each table when update"),
    ADD_STAT(scAgreeAtPred, statistics::units::Count::get(), "sc agrees with tage on prediction"),
    ADD_STAT(scAgreeAtCommit, statistics::units::Count::get(), "sc agrees with tage when update"),
    ADD_STAT(scDisagreeAtPred, statistics::units::Count::get(), "sc disagrees with tage on prediction"),
    ADD_STAT(scDisagreeAtCommit, statistics::units::Count::get(), "sc disagrees with tage when update"),
    ADD_STAT(scConfAtPred, statistics::units::Count::get(), "sc is confident on prediction"),
    ADD_STAT(scConfAtCommit, statistics::units::Count::get(), "sc is confident when update"),
    ADD_STAT(scUnconfAtPred, statistics::units::Count::get(), "sc is unconfident on prediction"),
    ADD_STAT(scUnconfAtCommit, statistics::units::Count::get(), "sc is unconfident when update"),
    ADD_STAT(scUpdateOnMispred, statistics::units::Count::get(), "sc update because of misprediction"),
    ADD_STAT(scUpdateOnUnconf, statistics::units::Count::get(), "sc update because of unconfidence"),
    ADD_STAT(scUsedAtPred, statistics::units::Count::get(), "sc used on prediction"),
    ADD_STAT(scUsedAtCommit, statistics::units::Count::get(), "sc used when update"),
    ADD_STAT(scCorrectTageWrong, statistics::units::Count::get(), "sc correct and tage wrong when update"),
    ADD_STAT(scWrongTageCorrect, statistics::units::Count::get(), "sc wrong and tage correct when update")
{
    predTableHits.init(0, numPredictors-1, 1);
    updateTableHits.init(0, numPredictors-1, 1);
    updateResetUCtrInc.init(1, numPredictors, 1);
    updateResetUCtrDec.init(1, numPredictors, 1);
    updateTableMispreds.init(numPredictors);
}

void
FTBTAGE::TageBankStats::updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred)
{
    bool hit = pred.mainFound;
    unsigned hit_table = pred.table;
    bool useAlt = pred.useAlt;
    if (when_pred) {
        if (hit) {
            predTableHits.sample(hit_table, 1);
        } else {
            predNoHitUseBim++;
        }
        if (!hit || useAlt) {
            predUseAlt++;
        }
    } else {
        if (hit) {
            updateTableHits.sample(hit_table, 1);
        } else {
            updateNoHitUseBim++;
        }
        if (!hit || useAlt) {
            updateUseAlt++;
        }
    }
}

void
FTBTAGE::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

} // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5

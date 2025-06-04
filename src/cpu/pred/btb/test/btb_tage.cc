#include "cpu/pred/btb/test/btb_tage.hh"
#include <sys/types.h>


namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

namespace test
{

// Constructor: Initialize TAGE predictor with given parameters
BTBTAGE::BTBTAGE(unsigned numPredictors, unsigned numWays, unsigned tableSize) :
    TimedBaseBTBPredictor(),
    numPredictors(numPredictors), numWays(numWays), tageStats(numPredictors)
{
    setNumDelay(1);
    DPRINTF(TAGE, "BTBTAGE constructor\n");
    for (unsigned i = 0; i < numPredictors; i++) {
        tableSizes.push_back(tableSize);
        tableTagBits.push_back(8);
        tablePcShifts.push_back(1);
    }
    histLengths = {8, 13, 32, 62}; // max hist < 64 in test
    maxHistLen = 970;
    numTablesToAlloc = 1;
    enableSC = false;

    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    // baseTable.resize(2048); // need modify
    for (unsigned int i = 0; i < numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
        }

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(tableIndexBits[i], true);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(tableTagBits[i], true);

        assert(tablePcShifts.size() >= numPredictors);

        tagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i], 16));
        altTagFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableTagBits[i]-1, 16));
        indexFoldedHist.push_back(FoldedHist((int)histLengths[i], (int)tableIndexBits[i], 16));
    }
    // for (unsigned i = 0; i < baseTable.size(); ++i) {
    //     baseTable[i].resize(numBr);
    // }
    usefulResetCnt = 0;

    useAlt.resize(128);
}

BTBTAGE::~BTBTAGE()
{
}

void
BTBTAGE::tick() {}

void
BTBTAGE::tickStart() {}

/**
 * @brief Helper method to record useful bit in all TAGE tables
 *
 * @param startPC The starting PC address for lookup
 */
void
BTBTAGE::recordUsefulMask(const Addr &startPC) {
    // Initialize all usefulMasks
    meta.usefulMask.resize(numWays);
    for (unsigned way = 0; way < numWays; way++) {
        meta.usefulMask[way].resize(numPredictors);
    }

    // Look up entries in all TAGE tables
    for (int i = 0; i < numPredictors; ++i) {
        Addr index = getTageIndex(startPC, i);
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = tageTable[i][index][way];
            // Save useful bit to metadata
            meta.usefulMask[way][i] = entry.useful;
        }
    }
    if (debugFlagOn) {
        std::string buf;
        for (unsigned way = 0; way < numWays; way++) {
            boost::to_string(meta.usefulMask[way], buf);
            DPRINTF(TAGEUseful, "meta.usefulMask[%u] = %s\n", way, buf.c_str());
        }
    }
}

/**
 * @brief Generate prediction for a single BTB entry by searching TAGE tables
 *
 * @param btb_entry The BTB entry to generate prediction for
 * @param startPC The starting PC address for calculating indices and tags
 * @return TagePrediction containing main and alternative predictions
 */
BTBTAGE::TagePrediction
BTBTAGE::generateSinglePrediction(const BTBEntry &btb_entry, 
                                 const Addr &startPC) {
    DPRINTF(TAGE, "generateSinglePrediction for btbEntry: %#lx, always taken %d\n",
        btb_entry.pc, btb_entry.alwaysTaken);
    
    // Find main and alternative predictions
    bool provided = false;
    bool alt_provided = false;
    TageTableInfo main_info, alt_info;

    // Search from highest to lowest table for matches
    for (int i = numPredictors - 1; i >= 0; --i) {
        Addr index = getTageIndex(startPC, i);
        Addr tag = getTageTag(startPC, i); // use for tag comparison
        bool match = false; // for each table, only one way can be matched
        TageEntry matching_entry;
        unsigned matching_way = 0;

        // Search all ways for a matching entry
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = tageTable[i][index][way];
            // entry valid, tag match, branch pc/pos match!
            if (entry.valid && tag == entry.tag && btb_entry.pc == entry.pc) {
                matching_entry = entry;
                matching_way = way;
                match = true;

                // Update LRU counters
                updateLRU(i, index, way);

                DPRINTF(TAGE, "hit  table %d[%lu][%u]: valid %d, tag %lu, ctr %d, useful %d, btb_pc %#lx\n",
                    i, index, way, entry.valid, entry.tag, entry.counter, entry.useful, btb_entry.pc);
                break;  // only one way can be matched, aviod multi hit, TODO: RTL how to do this?
            }
        }

        if (match) {
            // Save match information for later recovery
            if (!provided) {
                meta.hitWay = matching_way;
                meta.hitFound = true;
            }

            if (!provided) {
                // First match becomes main prediction
                main_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                provided = true;
            } else if (!alt_provided) {
                // Second match becomes alternative prediction
                alt_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                alt_provided = true;
                break;
            }
        } else {
            DPRINTF(TAGE, "miss table %d[%lu] for tag %lu, btb_pc %#lx\n",
                i, index, tag, btb_entry.pc);
        }
    }

    // Generate final prediction
    bool main_taken = main_info.taken();
    bool alt_taken = alt_info.taken();
    bool base_taken = btb_entry.ctr >= 0;
    bool alt_pred = alt_provided ? alt_taken : base_taken; // if alt provided, use alt prediction, otherwise use base
    bool use_alt = main_info.entry.counter == 0 ||
                   main_info.entry.counter == -1 ||
                   !provided;   // main prediction is weak or not provided
    bool taken = use_alt ? alt_pred : main_taken;

    DPRINTF(TAGE, "tage predict %#lx taken %d\n", btb_entry.pc, taken);
    DPRINTF(TAGE, "tage use_alt %d ? (alt_provided %d ? alt_taken %d : base_taken %d) : main_taken %d\n",
        use_alt, alt_provided, alt_taken, base_taken, main_taken);

    return TagePrediction(btb_entry.pc, main_info, alt_info, use_alt, taken);
}

/**
 * @brief Look up predictions in TAGE tables for a stream of instructions
 * 
 * @param startPC The starting PC address for the instruction stream
 * @param btbEntries Vector of BTB entries to make predictions for
 * @return Map of branch PC addresses to their predicted outcomes
 */
std::map<Addr, bool>
BTBTAGE::lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries)
{
    // Clear old prediction metadata and save current history state
    meta.preds.clear();
    meta.tagFoldedHist = tagFoldedHist;
    meta.altTagFoldedHist = altTagFoldedHist;
    meta.indexFoldedHist = indexFoldedHist;
    // record useful bit to meta.usefulMask
    recordUsefulMask(startPC);

    DPRINTF(TAGE, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    std::map<Addr, bool> cond_takens;
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto pred = generateSinglePrediction(btb_entry, startPC);
            meta.preds[btb_entry.pc] = pred;
            tageStats.updateStatsWithTagePrediction(pred, true);
            cond_takens[btb_entry.pc] = pred.taken || btb_entry.alwaysTaken;
        }
    }
    return cond_takens;
}

/**
 * @brief Makes predictions for a stream of instructions using TAGE predictor
 * 
 * This function is called during the prediction stage and:
 * 1. Uses lookupHelper to get predictions for all BTB entries
 * 2. Stores predictions in the stage prediction structure
 * 3. Handles multiple prediction stages with different delays
 * 
 * @param stream_start Starting PC of the instruction stream
 * @param history Current branch history
 * @param stagePreds Vector of predictions for different pipeline stages
 */
void
BTBTAGE::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    DPRINTF(TAGE, "putPCHistory startAddr: %#lx\n", stream_start);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it
    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        auto cond_takens = lookupHelper(stream_start, stage_pred.btbEntries);
        stage_pred.condTakens = cond_takens;
    }

}

std::shared_ptr<void>
BTBTAGE::getPredictionMeta() {
    std::shared_ptr<void> meta_void_ptr = std::make_shared<TageMeta>(meta);
    return meta_void_ptr;
}

/**
 * @brief Prepare BTB entries for update by filtering and processing
 * 
 * @param stream The fetch stream containing update information
 * @return Vector of BTB entries that need to be updated
 */
std::vector<BTBEntry>
BTBTAGE::prepareUpdateEntries(const FetchStream &stream) {
    auto all_entries = stream.updateBTBEntries;
    
    // Filter out non-conditional and always-taken branches
    auto remove_it = std::remove_if(all_entries.begin(), all_entries.end(),
        [](const BTBEntry &e) { return !e.isCond && !e.alwaysTaken; });
    all_entries.erase(remove_it, all_entries.end());

    // Handle potential new BTB entry
    auto &potential_new_entry = stream.updateNewBTBEntry;
    if (!stream.updateIsOldEntry && potential_new_entry.isCond && 
        !potential_new_entry.alwaysTaken) {
        all_entries.push_back(potential_new_entry);
    }

    return all_entries;
}

/**
 * @brief Update predictor state for a single entry
 * 
 * @param entry The BTB entry being updated
 * @param actual_taken The actual outcome of the branch
 * @param pred The prediction made for this entry
 * @param stream The fetch stream containing update information
 * @return true if need to allocate new entry
 */
bool
BTBTAGE::updatePredictorStateAndCheckAllocation(const BTBEntry &entry,
                             bool actual_taken,
                             const TagePrediction &pred,
                             const FetchStream &stream) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    auto &alt_info = pred.altInfo;
    bool used_alt = pred.useAlt;
    bool alt_taken = alt_info.found ? alt_info.taken() : entry.ctr >= 0;

    // Update main prediction provider
    if (main_info.found) {
        DPRINTF(TAGE, "prediction provided by table %d, idx %lu, way %u, updating corresponding entry\n",
            main_info.table, main_info.index, main_info.way);

        auto &way = tageTable[main_info.table][main_info.index][main_info.way];

        // Update useful bit if predictions differ
        
        if (main_info.taken() != alt_taken) {
            way.useful = actual_taken == main_info.taken();
        }
        DPRINTF(TAGE, "useful bit set to %d\n", way.useful);
        
        // Update prediction counter
        updateCounter(actual_taken, 3, way.counter);

        // Update LRU counter
        updateLRU(main_info.table, main_info.index, main_info.way);
    }

    // Update alternative prediction provider
    if (used_alt && alt_info.found) {
        auto &way = tageTable[alt_info.table][alt_info.index][alt_info.way];
        updateCounter(actual_taken, 3, way.counter);
        updateLRU(alt_info.table, alt_info.index, alt_info.way);
    }

    // Update statistics
    if (used_alt) {
        bool alt_correct = alt_taken == actual_taken;
        if (alt_correct) {
            tageStats.updateUseAltCorrect++;
        } else {
            tageStats.updateUseAltWrong++;
        }
        if (main_info.found && main_info.taken() != alt_taken) {
            tageStats.updateAltDiffers++;
        }
    }

    // check if need to allocate new entry
    bool this_cond_mispred = stream.squashType == SquashType::SQUASH_CTRL && 
                               stream.squashPC == entry.pc;
    if (this_cond_mispred) {
        tageStats.updateMispred++;
        if (!used_alt && main_info.found) {
            tageStats.updateTableMispreds[main_info.table]++;
        }
    }

    // check if we used alt prediction and main was correct(main is weak so used alt, no need to allocate new entry)
    bool use_alt_on_main_found_correct = used_alt && main_info.found &&
                                        main_info.taken() == actual_taken;

    // return true if need to allocate new entry = mispred and main was incorrect
    return this_cond_mispred && !use_alt_on_main_found_correct;
}


/**
 * @brief Handle useful bit reset mechanism
 * 
 * @param useful_mask The vector of useful masks
 * @param way The way to process
 * @param found Whether a hit was found
 */
void
BTBTAGE::handleUsefulBitReset(const std::vector<bitset> &useful_mask, unsigned way, bool found) {
    // Use the provided way or default to way 0 if there was no hit
    unsigned way_to_use = (found) ? way : 0;

    if (way_to_use >= useful_mask.size()) {
        way_to_use = 0;
    }

    const bitset &mask_to_use = useful_mask[way_to_use];

    if (debugFlagOn) {
        std::string useful_str;
        boost::to_string(mask_to_use, useful_str);
        DPRINTF(TAGEUseful, "useful mask for way %u: %s\n", way_to_use, useful_str.c_str());
    }

    // Calculate tables that can be allocated
    int num_tables_can_allocate = (~mask_to_use).count(); // useful bit = 0
    int total_tables_to_allocate = mask_to_use.size();
    // number of tables is allocated, useful bit = 1
    int num_tables_is_allocated = total_tables_to_allocate - num_tables_can_allocate;

    // more than half of the tables is allocated, resetCounter++
    bool incUsefulResetCounter = num_tables_can_allocate < num_tables_is_allocated;
    bool decUsefulResetCounter = num_tables_can_allocate > num_tables_is_allocated;
    int changeVal = std::abs(num_tables_can_allocate - num_tables_is_allocated);

    // Update reset counter
    if (incUsefulResetCounter) {
        // tageStats.updateResetUCtrInc.sample(changeVal, 1);
        usefulResetCnt = std::min(usefulResetCnt + changeVal, 128); // max is 128
        DPRINTF(TAGEUseful, "incUsefulResetCounter, changeVal %d, usefulResetCnt %d\n", 
                changeVal, usefulResetCnt);
    } else if (decUsefulResetCounter) {
        // tageStats.updateResetUCtrDec.sample(changeVal, 1);
        usefulResetCnt = std::max(usefulResetCnt - changeVal, 0); // min is 0
        DPRINTF(TAGEUseful, "decUsefulResetCounter, changeVal %d, usefulResetCnt %d\n", 
                changeVal, usefulResetCnt);
    }

    // Reset all useful bits if counter reaches threshold
    if (usefulResetCnt == 128) {
        tageStats.updateResetU++;
        DPRINTF(TAGEUseful, "reset useful bit of all entries\n");
        for (auto &table : tageTable) {
            for (auto &set : table) {
                for (auto &way : set) {
                    way.useful = 0;
                }
            }
        }
        usefulResetCnt = 0;
    }
}

/**
 * @brief Generate allocation mask for new entries
 * 
 * @param useful_mask The mask of useful bits
 * @param start_table The starting table for allocation
 * @return AllocationResult containing allocation information
 */
BTBTAGE::AllocationResult
BTBTAGE::generateAllocationMask(const bitset &useful_mask,
                               unsigned start_table) {
    AllocationResult result;
    
    // Check if allocation is possible
    auto flipped_usefulMask = ~useful_mask;
    result.allocate_valid = flipped_usefulMask.any();
    
    if (!result.allocate_valid) {
        DPRINTF(TAGEUseful, "no valid table to allocate, all of them are useful\n");
        return result;
    }

    // Generate allocation mask using LFSR
    unsigned alloc_table_num = numPredictors - start_table; // number of tables to allocate
    unsigned maskMaxNum = std::pow(2, alloc_table_num); // max number of masks
    unsigned mask = allocLFSR.get() % maskMaxNum; // generate a random mask
    
    bitset allocateLFSR(alloc_table_num, mask); // generate a mask using LFSR
    bitset masked = allocateLFSR & flipped_usefulMask; // apply the mask to the useful mask
    result.allocate_mask = masked.any() ? masked : flipped_usefulMask; // if there is any 1 in the mask, use the mask, otherwise use the useful mask
    if (debugFlagOn) {
        std::string buf;
        boost::to_string(allocateLFSR, buf);
        DPRINTF(TAGEUseful, "allocateLFSR %s, size %lu\n", buf.c_str(), allocateLFSR.size());
        boost::to_string(masked, buf);
        DPRINTF(TAGEUseful, "masked %s, size %lu\n", buf.c_str(), masked.size());
        boost::to_string(result.allocate_mask, buf);
        DPRINTF(TAGEUseful, "allocate %s, size %lu\n", buf.c_str(), result.allocate_mask.size());
    }

    return result;
}

/**
 * @brief Handle allocation of new entries
 * 
 * @param startPC The starting PC address
 * @param entry The BTB entry being updated
 * @param actual_taken The actual outcome of the branch
 * @param useful_mask The vector of useful masks
 * @param start_table The starting table for allocation
 * @param meta The metadata of the predictor
 */
void
BTBTAGE::handleNewEntryAllocation(const Addr &startPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 const std::vector<bitset> &useful_mask,
                                 unsigned start_table,
                                 std::shared_ptr<TageMeta> meta) {
    // Select which usefulMask to use based on whether a hit was found
    unsigned way_to_use = meta->hitFound ? meta->hitWay : 0;

    if (way_to_use >= useful_mask.size()) {
        way_to_use = 0;
    }

    // Get a copy of the selected mask
    bitset working_mask = useful_mask[way_to_use];

    // Adjust mask for the starting table if needed
    if (start_table > 0) {
        working_mask >>= start_table; // only allocate tables after the starting table
        working_mask.resize(numPredictors - start_table);
    }

    // Generate allocation mask
    auto alloc_result = generateAllocationMask(working_mask, start_table);

    if (!alloc_result.allocate_valid) {
        tageStats.updateAllocFailure++;
        return;
    }

    DPRINTF(TAGE, "allocate new entry\n");
    tageStats.updateAllocSuccess++;

    // Try to allocate in each table according to allocation mask
    for (unsigned ti = start_table; ti < numPredictors; ti++) {
        if (!alloc_result.allocate_mask[ti - start_table]) {
            continue;
        }

        // Get necessary history for index and tag computation
        auto &updateTagFoldedHist = meta->tagFoldedHist;
        auto &updateAltTagFoldedHist = meta->altTagFoldedHist;
        auto &updateIndexFoldedHist = meta->indexFoldedHist;

        // Compute index and tag for the new entry
        Addr newIndex = getTageIndex(startPC, ti, updateIndexFoldedHist[ti].get());
        Addr newTag = getTageTag(startPC, ti, updateTagFoldedHist[ti].get(), updateAltTagFoldedHist[ti].get());

        // Find a way to allocate (invalid entry or LRU victim)
        unsigned way = getLRUVictim(ti, newIndex);

        // Update the entry
        auto &entry_to_update = tageTable[ti][newIndex][way];
        short newCounter = actual_taken ? 0 : -1;

        DPRINTF(TAGE, "allocating entry in table %d[%lu][%u], tag %lu, counter %d\n",
            ti, newIndex, way, newTag, newCounter);

        entry_to_update = TageEntry(newTag, newCounter, entry.pc);

        // Reset LRU counter for the new entry
        updateLRU(ti, newIndex, way);

        break;  // allocate only 1 entry
    }
}

/**
 * @brief Updates the TAGE predictor state based on actual branch execution results
 * 
 * @param stream The fetch stream containing branch execution information
 */
void
BTBTAGE::update(const FetchStream &stream) {
    Addr startAddr = stream.getRealStartPC();
    DPRINTF(TAGE, "update startAddr: %#lx\n", startAddr);

    // Prepare BTB entries to update
    auto entries_to_update = prepareUpdateEntries(stream);
    
    // Get prediction metadata
    auto meta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    auto &preds = meta->preds;
    
    // Process each BTB entry
    for (auto &btb_entry : entries_to_update) {
        bool actual_taken = stream.exeTaken && stream.exeBranchInfo == btb_entry;
        auto pred_it = preds.find(btb_entry.pc);
        
        if (pred_it == preds.end()) {
            continue;
        }

        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(btb_entry, actual_taken, pred_it->second, stream);

        // Handle new entry allocation if needed
        if (need_allocate) {
            // Handle useful bit reset
            handleUsefulBitReset(meta->usefulMask, meta->hitWay, meta->hitFound);

            // Handle allocation of new entries
            uint start_table = 0;
            auto main_info = pred_it->second.mainInfo;
            if (main_info.found) {
                start_table = main_info.table + 1; // start from the table after the main prediction table
            }
            handleNewEntryAllocation(startAddr, btb_entry, actual_taken, 
                                   meta->usefulMask,
                                   start_table, meta);
        }
    }

    DPRINTF(TAGE, "end update\n");
}

// Update prediction counter with saturation
void
BTBTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width-1)) - 1;
    int min = -(1 << (width-1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

// Calculate TAGE tag with folded history
Addr
BTBTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist)
{
    // Create mask for tableTagBits[t]
    uint64_t mask = (tableTagBits[t] == 64) ? UINT64_MAX : ((1ULL << tableTagBits[t]) - 1);

    // Extract lower bits of PC
    uint64_t pcBits = (pc >> floorLog2(predictWidth)) & mask;

    // Prepare alt tag: shift left by 1 and mask
    uint64_t altTagBits = (altFoldedHist << 1) & mask;

    // XOR all components
    return (pcBits ^ (foldedHist & mask) ^ altTagBits) & mask;
}

Addr
BTBTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get());
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist)
{
    // Create mask for tableIndexBits[t]
    uint64_t mask = (tableIndexBits[t] == 64) ? UINT64_MAX : ((1ULL << tableIndexBits[t]) - 1);

    // Extract lower bits of PC and XOR with folded history
    uint64_t pcBits = (pc >> floorLog2(predictWidth)) & mask;
    return (pcBits ^ (foldedHist & mask)) & mask;
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t)
{
    return getTageIndex(pc, t, indexFoldedHist[t].get());
}

bool
BTBTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
BTBTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

Addr
BTBTAGE::getUseAltIdx(Addr pc) {
    return (pc >> instShiftAmt) & (useAlt.size() - 1); // need modify
}

/**
 * @brief Updates branch history for speculative execution
 * 
 * This function updates three types of folded histories:
 * - Tag folded history: Used for tag computation
 * - Alternative tag folded history: Used for alternative tag computation
 * - Index folded history: Used for table index computation
 * 
 * @param history The current branch history
 * @param shamt The number of bits to shift
 * @param taken Whether the branch was taken
 */
void
BTBTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken)
{
    if (debugFlagOn) {
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(TAGE, "in doUpdateHist, shamt %d, taken %d, history %s\n", shamt, taken, buf.c_str());
    }
    if (shamt == 0) {
        DPRINTF(TAGE, "shamt is 0, returning\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.update(history, shamt, taken);
        }
    }
}

/**
 * @brief Updates branch history for speculative execution
 * 
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 * 
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 * 
 * @param history The current branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBTAGE::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getHistInfo();
    doUpdateHist(history, shamt, cond_taken);
}

/**
 * @brief Recovers branch history state after a misprediction
 * 
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 * 
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBTAGE::recoverHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken);
}

// Check folded history after speculative update and recovery
void
BTBTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    DPRINTF(TAGE, "checking folded history when %s\n", when);
    if (debugFlagOn) {
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(TAGE, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {

            // DPRINTF(TAGE, "t: %d, type: %d\n", t, type);
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

BTBTAGE::TageStats::TageStats(int numPredictors) : 
    predTableHits(0),
    predNoHitUseBim(0),
    predUseAlt(0),
    updateTableHits(0),
    updateNoHitUseBim(0),
    updateUseAlt(0),
    updateUseAltCorrect(0),
    updateUseAltWrong(0),
    updateAltDiffers(0),
    updateUseAltOnNaUpdated(0),
    updateUseAltOnNaInc(0),
    updateUseAltOnNaDec(0),
    updateProviderNa(0),
    updateUseNaCorrect(0),
    updateUseNaWrong(0),
    updateUseAltOnNa(0),
    updateUseAltOnNaCorrect(0),
    updateUseAltOnNaWrong(0),
    updateAllocFailure(0),
    updateAllocSuccess(0),
    updateMispred(0),
    updateResetU(0),
    updateResetUCtrInc(0),
    updateResetUCtrDec(0),
    numPredictors(numPredictors) {
    updateTableMispreds.resize(numPredictors, 0);
}


// Update statistics based on TAGE prediction
void
BTBTAGE::TageStats::updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred)
{
    bool hit = pred.mainInfo.found;
    unsigned hit_table = pred.mainInfo.table;
    bool useAlt = pred.useAlt;
    if (when_pred) {
        if (hit) {
            // predTableHits.sample(hit_table, 1);
        } else {
            predNoHitUseBim++;
        }
        if (!hit || useAlt) {
            predUseAlt++;
        }
    } else {
        if (hit) {
            // updateTableHits.sample(hit_table, 1);
        } else {
            updateNoHitUseBim++;
        }
        if (!hit || useAlt) {
            updateUseAlt++;
        }
    }
}

// Update LRU counters for a set
void
BTBTAGE::updateLRU(int table, Addr index, unsigned way)
{
    // Increment LRU counters for all entries in the set
    for (unsigned i = 0; i < numWays; i++) {
        if (i != way && tageTable[table][index][i].valid) {
            tageTable[table][index][i].lruCounter++;
        }
    }
    // Reset LRU counter for the accessed entry
    tageTable[table][index][way].lruCounter = 0;
}

// Find the LRU victim in a set
unsigned
BTBTAGE::getLRUVictim(int table, Addr index)
{
    unsigned victim = 0;
    unsigned maxLRU = 0;

    // Find the entry with the highest LRU counter
    for (unsigned i = 0; i < numWays; i++) {
        if (!tageTable[table][index][i].valid) {
            return i; // Use invalid entry if available
        }
        if (tageTable[table][index][i].lruCounter > maxLRU) {
            maxLRU = tageTable[table][index][i].lruCounter;
            victim = i;
        }
    }
    return victim;
}

// void
// BTBTAGE::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
// {
// }

} // namespace test

} // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

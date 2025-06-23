/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "cpu/pred/btb/btb_ubtb.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/Fetch.hh"
#include "stream_struct.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

UBTB::UBTB(const Params &p)
    : TimedBaseBTBPredictor(p),
      lastPred(),
      meta(),
      ubtb(),
      mruList(),
      numEntries(p.numEntries),
      tagBits(p.tagBits),
      tagMask((1UL << p.tagBits) - 1),
      ubtbStats(this)
{
    if (!isPowerOf2(numEntries)) {
        fatal("uBTB entries is not a power of 2!");
    }

    // Initialize uBTB structure and MRU tracking
    ubtb.resize(numEntries);
    mruList.clear();  // Start with empty list
    for (auto it = ubtb.begin(); it != ubtb.end(); it++) {
        it->valid = false;
        mruList.push_back(it);
    }
    std::make_heap(mruList.begin(), mruList.end(), older());

    hasDB = true;
    dbName = "ubtb";
}


void
UBTB::setTrace()
{
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("pc", UINT64),  std::make_pair("brType", UINT64), std::make_pair("target", UINT64),
            std::make_pair("idx", UINT64), std::make_pair("mode", UINT64),   std::make_pair("hit", UINT64)};
        ubtbTrace = _db->addAndGetTrace("uBTBTrace", fields_vec);
        ubtbTrace->init_table();
    }
}

void
UBTB::PredStatistics(const TickedUBTBEntry entry, Addr startAddr)
{
    if (entry.valid) {
        Addr mbtb_end = (startAddr + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
        //assert(entry.pc >= startAddr && entry.pc < mbtb_end);
        DPRINTF(UBTB, "UBTB: lookup hit: \n");
        ubtbStats.predHit += 1;
        printTickedUBTBEntry(entry);
    } else {
        ubtbStats.predMiss++;
        DPRINTF(BTB, "uBTB: lookup miss\n");
    }
    return;
}

void
UBTB::fillStagePredictions(const TickedUBTBEntry &entry, std::vector<FullBTBPrediction> &stagePreds)
{
    FillStageLoop(s) {
        DPRINTF(UBTB, "UBTB: assigning prediction for stage %d\n", s);

        // Copy uBTB entries to stage prediction
        stagePreds[s].btbEntries.clear();
        stagePreds[s].condTakens.clear();  // TODO: consider moving this to another place -- the uBTB shouldn't need to
                                           // take care of this
        // Set predictions for each branch
        stagePreds[s].predTick = curTick();
    }

    if (entry.valid) {
        // push back dummy conditional branches before the taken branch, to create the correct speculative history
        // information, these dummy entries are not taken, thanks to them not being in stagePreds.condTakens.
        for (int i = 0; i < entry.numNTConds; i++) {
            auto dummy = BTBEntry();
            dummy.valid = true;
            dummy.isCond = true;
            dummy.pc = 0xdeadbeef;  // a magic number to indicate a dummy entry
            FillStageLoop(s) stagePreds[s].btbEntries.push_back(dummy);
        }

        FillStageLoop(s) stagePreds[s].btbEntries.push_back(BTBEntry(entry));
        if (entry.isCond) {
            // the always taken field of BTBEntry is ignored in uBTB
            // uBTB always assumes present entries to be taken
            FillStageLoop(s) stagePreds[s].condTakens.push_back({entry.pc, true});
        } else if (entry.isIndirect) {
            // Set predicted target for indirect branches
            DPRINTF(UBTB, "setting indirect target for pc %#lx to %#lx\n", entry.pc, entry.target);
            FillStageLoop(s) stagePreds[s].indirectTargets.push_back({entry.pc, entry.target});
            if (entry.isReturn) {
                FillStageLoop(s) stagePreds[s].returnTarget = entry.target;
            }
        }
    }
}

// Helper function to construct a FullBTBPrediction from BranchInfo (for 2nd prediction)
void
UBTB::fillSecondPrediction(const BranchInfo &branchInfo, Addr bbStart, FullBTBPrediction &prediction)
{
    // According to 2-taken design rules, the second branch should never be conditional
    assert(!branchInfo.isCond && "Second prediction should never be conditional branch");

    prediction.btbEntries.clear();
    prediction.condTakens.clear();
    prediction.indirectTargets.clear();
    prediction.bbStart = bbStart;
    prediction.predTick = curTick();
    prediction.predSource = 0; // uBTB is stage 0

    // Create BTBEntry from BranchInfo
    BTBEntry entry(branchInfo);
    prediction.btbEntries.push_back(entry);

    // Handle indirect branches (including returns and calls)
    // TODO: I tend to think indirect branches should not be allowed in the 2nd prediction
    // not even return, since the second branch will not be validated by RAS
    if (entry.isIndirect) {
        DPRINTF(UBTB, "setting indirect target for 2nd prediction pc %#lx to %#lx\n", entry.pc, entry.target);
        prediction.indirectTargets.push_back({entry.pc, entry.target});
        if (entry.isReturn) {
            prediction.returnTarget = entry.target;
        }
    }
    // For direct unconditional branches, no additional setup needed beyond the BTBEntry
}

void
UBTB::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                   std::vector<FullBTBPrediction> &stagePreds)
{
    // Reuse existing lookup and prediction logic
    meta = std::make_shared<UBTBMeta>();
    int hit_index = lookup(startAddr);
    auto& entry = meta->hit_entry;
    entry = (hit_index != -1) ? ubtb[hit_index] : TickedUBTBEntry();

    PredStatistics(entry, startAddr);

    // Fill predictions for each pipeline stage
    fillStagePredictions(entry, stagePreds);

    // Update metadata for later stages
    lastPred.hit_index = hit_index;
}

std::pair<int, bool>
UBTB::getTwoTakenPrediction(Addr startAddr, const boost::dynamic_bitset<> &history,
                           std::vector<FullBTBPrediction> &stagePreds,
                           FullBTBPrediction &secondPrediction)
{
    // Reuse existing lookup and prediction logic
    meta = std::make_shared<UBTBMeta>();
    int hit_index = lookup(startAddr);
    auto& entry = meta->hit_entry;
    entry = (hit_index != -1) ? ubtb[hit_index] : TickedUBTBEntry();

    //PredStatistics(entry, startAddr);

    // Fill primary prediction for each pipeline stage
    fillStagePredictions(entry, stagePreds);

    // Update metadata for later stages
    lastPred.hit_index = hit_index;

    bool has_second_prediction = false;

    // Check if we have a second prediction to provide
    if (entry.valid && entry.valid_2nd) {
        DPRINTF(UBTB, "uBTB: Found second prediction in entry, constructing 2nd FB\n");

        // Calculate target address for second prediction (where the second prediction should start)
        Addr second_bb_start = stagePreds[0].getTarget(predictWidth);

        // Construct the second prediction from the stored branch info
        fillSecondPrediction(entry.branch_info_2nd, second_bb_start, secondPrediction);

        // Validate range: the second branch should be within its own fetch block
        if (secondPrediction.btbEntries.size() > 0) {
            assert(secondPrediction.isTaken()); // this is guaranteed by the 2-taken design rules
            Addr control_addr = secondPrediction.controlAddr();
            Addr fall_through = secondPrediction.getFallThrough(predictWidth);

            if (control_addr >= second_bb_start && control_addr < fall_through) {
                has_second_prediction = true;
                DPRINTF(UBTB, "uBTB: Valid second prediction - bbStart: %#lx, controlAddr: %#lx, target: %#lx\n",
                       second_bb_start, control_addr, secondPrediction.getTarget(predictWidth));
            } else {
                // Range check failed, discard second prediction
                secondPrediction.btbEntries.clear();
                DPRINTF(UBTB,
                "uBTB: Second prediction failed range check - bbStart: %#lx, controlAddr: %#lx, fallThrough: %#lx\n",
                       second_bb_start, control_addr, fall_through);
            }
        }
    }

    return std::make_pair(hit_index, has_second_prediction);
}

int
UBTB::lookup(Addr startAddr)
{
    if (startAddr & 0x1) {
        return -1;  // ignore false hit when lowest bit is 1
    }

    Addr current_tag = getTag(startAddr);

    DPRINTF(UBTB, "UBTB: Doing tag comparison for tag %#lx\n", current_tag);

    // Find the matching entry and return its index
    for (size_t i = 0; i < ubtb.size(); ++i) {
        if (ubtb[i].valid && ubtb[i].tag == current_tag) {
            // Found a hit - verify no duplicates
            for (size_t j = i + 1; j < ubtb.size(); ++j) {
                assert(!(ubtb[j].valid && ubtb[j].tag == current_tag) &&
                       "Multiple hits found in uBTB for the same tag!");
            }

            // Update timestamp for MRU
            ubtb[i].tick = curTick();

            // the following line might be unnecessary, considering the
            // heap is updated on every LRU replacement, TODO: confirm this
            // std::make_heap(mruList.begin(), mruList.end(), older());

            DPRINTF(UBTB, "UBTB: Hit at index %zu for tag %#lx\n", i, current_tag);
            return static_cast<int>(i);
        }
    }

    DPRINTF(UBTB, "UBTB: Miss for tag %#lx\n", current_tag);
    return -1;  // Miss
}


void
UBTB::replaceOldEntry(int entryIndex, FullBTBPrediction & newPrediction)
{
    assert(entryIndex >= 0 && entryIndex < static_cast<int>(ubtb.size()));
    assert(newPrediction.getTakenEntry().valid);

    TickedUBTBEntry newEntry = TickedUBTBEntry(newPrediction.getTakenEntry(), curTick());
    // important! this is so that target set by RAS or ITTAGE is used
    newEntry.target = newPrediction.getTarget(predictWidth);
    // important: update tag (mbtb and ubtb have different tags, even different tag length)
    newEntry.tag = getTag(newPrediction.bbStart);
    /*  save the number of conditional branches before the taken branch
     *  this is useful in the prediction phase: to generate the correct speculative history information
     */
    newEntry.numNTConds = newPrediction.getHistInfo().first;
    if (newPrediction.getTakenEntry().isCond) {
        newEntry.numNTConds--;
        assert(newEntry.numNTConds >= 0);
    }

    ubtb[entryIndex] = newEntry;

    DPRINTF(UBTB, "UBTB: Replaced entry at index %d with new prediction for PC %#lx\n",
           entryIndex, newPrediction.controlAddr());
}

void
UBTB::addSecondPredictionToEntry(int entryIndex, FullBTBPrediction* secondPred)
{
    assert(entryIndex >= 0 && entryIndex < static_cast<int>(ubtb.size()));
    assert(secondPred != nullptr && "Second prediction must not be null");
    assert(secondPred->getTakenEntry().valid && "Second prediction must be valid for 2-taken");

    auto& entry = ubtb[entryIndex];
    assert(entry.valid && "Entry must be valid to add second prediction");

    // Only add if not already present
    if (!entry.valid_2nd) {
        entry.valid_2nd = true;
        auto s3TakenEntry = secondPred->getTakenEntry();

        // Copy branch info (BTBEntry inherits from BranchInfo)
        entry.branch_info_2nd = s3TakenEntry;
        // Override target with the one from prediction (may be set by RAS/ITTAGE)
        entry.branch_info_2nd.target = secondPred->getTarget(predictWidth);

        DPRINTF(UBTB, "UBTB: Added second prediction to entry at index %d: secondary PC %#lx\n",
               entryIndex, secondPred->controlAddr());
    } else {
        DPRINTF(UBTB, "UBTB: Entry at index %d already has second prediction, skipping\n", entryIndex);
    }
}


void
UBTB::updateUsingS3Pred(FullBTBPrediction &s3Pred)
{
    DPRINTF(UBTB, "1-taken updateUsingS3Pred: hit_index=%d, s3Pred.bbStart=%#lx\n",
           lastPred.hit_index, s3Pred.bbStart);

    // Use the common helper function with the hit index from lastPred (no second prediction)
    updateEntryAtIndex(lastPred.hit_index, s3Pred, nullptr);
}


bool
UBTB::check2TakenConditions(FullBTBPrediction& dff, const FullBTBPrediction& s3Pred)
{
    assert(dff.getTarget(predictWidth) == s3Pred.bbStart);

    // 1. Both predictions must have at least one branch.
    if (dff.btbEntries.empty() || s3Pred.btbEntries.empty()) {
        return false;
    }

    auto dffEntry = dff.getTakenEntry();
    auto& s3PredEntry = s3Pred.btbEntries[0];

    // 2. The first branch must be taken for a 2-taken sequence to form.
    if (!dff.isTaken()) {
        return false;
    }

    // 3. Check branch type compatibility based on spec table.

    // Rule: 'multi-target indirect' as 1st branch is not allowed.
    if (dffEntry.isIndirect) {
        return false;
    }

    // Rule: 'multi-target indirect' as 2nd branch is not allowed.
    if (s3PredEntry.isIndirect) {
        return false;
    }

    // Rule: 'cond' as 2nd branch is not allowed.
    if (s3PredEntry.isCond) {
        return false;
    }

    // Rule: 'ret -> ret' is not allowed to avoid multiple RAS reads.
    if (dffEntry.isReturn && s3PredEntry.isReturn) {
        return false;
    }

    // Rule: 'call -> call' is not allowed to avoid multiple RAS writes.
    if (dffEntry.isCall && s3PredEntry.isCall) {
        return false;
    }

    // (call -> ret is allowed, so no check needed)

    // All conditions passed.
    return true;
}

// theoretically pred is a const reference, but certain functions
// like getTakenEntry() are factually const but not declared as const
void
UBTB::updateEntryAtIndex(int entry_index, FullBTBPrediction& pred, FullBTBPrediction* secondPred)
{
    DPRINTF(UBTB, "updateEntryAtIndex: entry_index=%d, pred.bbStart=%#lx, secondPred=%s\n",
           entry_index, pred.bbStart, secondPred ? "provided" : "null");

    auto s3TakenEntry = pred.getTakenEntry();

    if (entry_index >= 0) {
        // Hit case: We have a valid entry at entry_index
        assert(entry_index < static_cast<int>(ubtb.size()));
        auto& entry = ubtb[entry_index];
        assert(entry.valid && "Hit entry should be valid");
        assert(entry.tag == getTag(pred.bbStart));

        if (!s3TakenEntry.valid) {
            // S0 has a hit entry, but S3 predicts fall through
            updateUCtr(entry.uctr, false);
            if (entry.uctr == 0) {
                entry.valid = false;
                DPRINTF(UBTB, "updateEntryAtIndex: Invalidated entry at index %d (fall through)\n", entry_index);
            }
        } else {
            // Both S0 and S3 predict taken - check if they match
            if (entry.pc != pred.controlAddr() || entry.target != pred.getTarget(predictWidth)) {
                // S0 and S3 predict different branch instruction
                updateUCtr(entry.uctr, false);
                if (entry.uctr == 0) {
                    // Replace the old entry with the new one
                    replaceOldEntry(entry_index, const_cast<FullBTBPrediction&>(pred));
                    // Add second prediction if provided
                    if (secondPred != nullptr) {
                        addSecondPredictionToEntry(entry_index, secondPred);
                    }
                    DPRINTF(UBTB, "updateEntryAtIndex: Replaced entry at index %d (mismatch)\n", entry_index);
                }
            } else {
                // S0 and S3 predict the same (brpc and target)
                updateUCtr(entry.uctr, true);

                // Add second prediction if provided
                if (secondPred != nullptr) {
                    addSecondPredictionToEntry(entry_index, secondPred);
                }

                DPRINTF(UBTB, "updateEntryAtIndex: Reinforced entry at index %d (match)\n", entry_index);
            }
        }
    } else {
        // Miss case: entry_index == -1
        if (s3TakenEntry.valid) {
            /* S0 misses, but S3 predicts taken,
             * generate new entry and replace another using LRU
             */
            // check if the new entry exist in the uBTB
            for (size_t i = 0; i < ubtb.size(); ++i) {
                if (ubtb[i].tag == getTag(pred.bbStart)) {
                    //warn("updateEntryAtIndex: New entry already exists in uBTB\n");
                    return;
                }
            }

            int toBeReplacedIndex = -1;

            // First try to find an invalid entry
            for (size_t i = 0; i < ubtb.size(); ++i) {
                if (!ubtb[i].valid) {
                    toBeReplacedIndex = static_cast<int>(i);
                    break;
                }
            }

            // If no invalid entry found, use LRU policy
            if (toBeReplacedIndex == -1) {
                // Find the least recently used entry
                std::make_heap(mruList.begin(), mruList.end(), older());
                UBTBIter lru_iter = mruList.front();
                toBeReplacedIndex = lru_iter - ubtb.begin();
            }

            // Replace the entry with the new prediction
            replaceOldEntry(toBeReplacedIndex, const_cast<FullBTBPrediction&>(pred));
            // Add second prediction if provided
            if (secondPred != nullptr) {
                addSecondPredictionToEntry(toBeReplacedIndex, secondPred);
            }
            DPRINTF(UBTB, "updateEntryAtIndex: Created new entry at index %d (miss->hit)\n", toBeReplacedIndex);
        } else {
            // Both S0 and S3 predict fall through - do nothing
            DPRINTF(UBTB, "updateEntryAtIndex: No action needed (miss->fall through)\n");
        }
    }
}

void
UBTB::updateUsingS3Pred(FullBTBPrediction &dff_pred,
                        FullBTBPrediction &s3_pred,
                        int hit_index) // hit index is the index stored in dff, along with dff_pred
{
    DPRINTF(UBTB, "2-taken updateUsingS3Pred: hit_index=%d, dff_pred.bbStart=%#lx, s3_pred.bbStart=%#lx\n",
           hit_index, dff_pred.bbStart, s3_pred.bbStart);

    // Validate consecutive FB condition
    if (dff_pred.getTarget(predictWidth) != s3_pred.bbStart) {
        DPRINTF(UBTB, "2-taken training rejected: FBs are not consecutive (%#lx -> %#lx vs %#lx)\n",
               dff_pred.bbStart, dff_pred.getTarget(predictWidth), s3_pred.bbStart);
        // Fall back to training only with dff_pred using the correct entry (previous cycle's hit)
        updateEntryAtIndex(hit_index, dff_pred, nullptr);
        return;
    }

    // Check 2-taken conditions
    if (!check2TakenConditions(dff_pred, s3_pred)) {
        DPRINTF(UBTB, "2-taken training rejected: conditions not met\n");
        // Fall back to training only with dff_pred using the correct entry (previous cycle's hit)
        updateEntryAtIndex(hit_index, dff_pred, nullptr);
        return;
    }

    // Train as 2-taken: pass s3_pred as second prediction
    updateEntryAtIndex(hit_index, dff_pred, &s3_pred);
}


void
UBTB::update(const FetchStream &stream)
{
    auto meta = std::static_pointer_cast<UBTBMeta>(stream.predMetas[getComponentIdx()]);
    // hit entries whose corresponding insts are acutally executed
    Addr end_inst_pc = stream.updateEndInstPC;

    auto pred_hit_entry = meta->hit_entry;

    if (stream.exeTaken) {
        if (!pred_hit_entry.valid || pred_hit_entry != stream.exeBranchInfo) {
            DPRINTF(BTB, "update miss detected, pc %#lx, predTick %lu\n", stream.exeBranchInfo.pc, stream.predTick);
            ubtbStats.updateMiss++;
        }
    }

    // Verify uBTB state
    assert(ubtb.size() <= numEntries);
}

void
UBTB::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
    auto meta = std::static_pointer_cast<UBTBMeta>(stream.predMetas[getComponentIdx()]);
    auto &hit_entry = meta->hit_entry;
    auto pc = inst->getPC();
    auto npc = inst->getNPC();
    bool this_branch_hit = hit_entry.pc == pc;

    bool cond_not_taken = inst->isCondCtrl() && !inst->branching();
    bool this_branch_taken = stream.exeTaken && stream.getControlPC() == pc;  // all uncond should be taken
    Addr this_branch_target = npc;
    if (this_branch_hit) {
        ubtbStats.allBranchHits++;
        if (this_branch_taken) {
            ubtbStats.allBranchHitTakens++;
        } else {
            ubtbStats.allBranchHitNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ubtbStats.condHits++;
            if (this_branch_taken) {
                ubtbStats.condHitTakens++;
            } else {
                ubtbStats.condHitNotTakens++;
            }
            // TODO: for now we assume uBTB hit means the branch is taken, this might change later
            // bool pred_taken = hit_entry.ctr >= 0;
            if (this_branch_taken) {
                ubtbStats.condPredCorrect++;
            } else {
                ubtbStats.condPredWrong++;
            }
        }
        if (inst->isUncondCtrl()) {
            ubtbStats.uncondHits++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ubtbStats.indirectHits++;
                Addr pred_target = hit_entry.target;
                if (pred_target == this_branch_target) {
                    ubtbStats.indirectPredCorrect++;
                } else {
                    ubtbStats.indirectPredWrong++;
                }
            }
            if (inst->isCall()) {
                ubtbStats.callHits++;
            }
            if (inst->isReturn()) {
                ubtbStats.returnHits++;
            }
        }
    } else {
        ubtbStats.allBranchMisses++;
        if (this_branch_taken) {
            ubtbStats.allBranchMissTakens++;
        } else {
            ubtbStats.allBranchMissNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ubtbStats.condMisses++;
            if (this_branch_taken) {
                ubtbStats.condMissTakens++;
                ubtbStats.condPredWrong++;
            } else {
                ubtbStats.condMissNotTakens++;
                ubtbStats.condPredCorrect++;
            }
        }
        if (inst->isUncondCtrl()) {
            ubtbStats.uncondMisses++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ubtbStats.indirectMisses++;
                ubtbStats.indirectPredWrong++;
            }
            if (inst->isCall()) {
                ubtbStats.callMisses++;
            }
            if (inst->isReturn()) {
                ubtbStats.returnMisses++;
            }
        }
    }
}

// Initialize uBTB statistics
UBTB::UBTBStats::UBTBStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(predMiss, statistics::units::Count::get(), "misses encountered on prediction"),
      ADD_STAT(predHit, statistics::units::Count::get(), "hits encountered on prediction"),
      ADD_STAT(updateMiss, statistics::units::Count::get(), "misses encountered on update"),


      ADD_STAT(allBranchHits, statistics::units::Count::get(),
               "all types of branches committed that was predicted hit"),
      ADD_STAT(allBranchHitTakens, statistics::units::Count::get(),
               "all types of taken branches committed was that predicted hit"),
      ADD_STAT(allBranchHitNotTakens, statistics::units::Count::get(),
               "all types of not taken branches committed was that predicted hit"),
      ADD_STAT(allBranchMisses, statistics::units::Count::get(),
               "all types of branches committed that was predicted miss"),
      ADD_STAT(allBranchMissTakens, statistics::units::Count::get(),
               "all types of taken branches committed was that predicted miss"),
      ADD_STAT(allBranchMissNotTakens, statistics::units::Count::get(),
               "all types of not taken branches committed was that predicted miss"),
      ADD_STAT(condHits, statistics::units::Count::get(), "conditional branches committed that was predicted hit"),
      ADD_STAT(condHitTakens, statistics::units::Count::get(),
               "taken conditional branches committed was that predicted hit"),
      ADD_STAT(condHitNotTakens, statistics::units::Count::get(),
               "not taken conditional branches committed was that predicted hit"),
      ADD_STAT(condMisses, statistics::units::Count::get(), "conditional branches committed that was predicted miss"),
      ADD_STAT(condMissTakens, statistics::units::Count::get(),
               "taken conditional branches committed was that predicted miss"),
      ADD_STAT(condMissNotTakens, statistics::units::Count::get(),
               "not taken conditional branches committed was that predicted miss"),
      ADD_STAT(condPredCorrect, statistics::units::Count::get(),
               "conditional branches committed was that correctly predicted by btb"),
      ADD_STAT(condPredWrong, statistics::units::Count::get(),
               "conditional branches committed was that mispredicted by btb"),
      ADD_STAT(uncondHits, statistics::units::Count::get(), "unconditional branches committed that was predicted hit"),
      ADD_STAT(uncondMisses, statistics::units::Count::get(),
               "unconditional branches committed that was predicted miss"),
      ADD_STAT(indirectHits, statistics::units::Count::get(), "indirect branches committed that was predicted hit"),
      ADD_STAT(indirectMisses, statistics::units::Count::get(), "indirect branches committed that was predicted miss"),
      ADD_STAT(indirectPredCorrect, statistics::units::Count::get(),
               "indirect branches committed whose target was correctly predicted by btb"),
      ADD_STAT(indirectPredWrong, statistics::units::Count::get(),
               "indirect branches committed whose target was mispredicted by btb"),
      ADD_STAT(callHits, statistics::units::Count::get(), "calls committed that was predicted hit"),
      ADD_STAT(callMisses, statistics::units::Count::get(), "calls committed that was predicted miss"),
      ADD_STAT(returnHits, statistics::units::Count::get(), "returns committed that was predicted hit"),
      ADD_STAT(returnMisses, statistics::units::Count::get(), "returns committed that was predicted miss")
{
}


}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

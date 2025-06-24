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
        assert(entry.pc >= startAddr && entry.pc < mbtb_end);
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

void
UBTB::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history, std::vector<FullBTBPrediction> &stagePreds)
{
    meta = std::make_shared<UBTBMeta>();
    auto it = lookup(startAddr);
    auto& entry = meta->hit_entry;
    entry = (it != ubtb.end()) ? *it : TickedUBTBEntry();

    PredStatistics(entry, startAddr);

    // Fill predictions for each pipeline stage
    fillStagePredictions(entry, stagePreds);

    // Update metadata for later stages
    lastPred.hit_entry = it;
}

UBTB::UBTBIter
UBTB::lookup(Addr startAddr)
{
    if (startAddr & 0x1) {
        return ubtb.end();  // ignore false hit when lowest bit is 1
    }

    Addr current_tag = getTag(startAddr);

    DPRINTF(UBTB, "UBTB: Doing tag comparison for tag %#lx\n", current_tag);

    auto it = std::find_if(ubtb.begin(), ubtb.end(),
                           [current_tag](const TickedUBTBEntry &way) { return way.valid && way.tag == current_tag; });

    if (it != ubtb.end()) {
        // Found a hit - verify no duplicates
        auto duplicate = std::find_if(std::next(it), ubtb.end(), [current_tag](const TickedUBTBEntry &way) {
            return way.valid && way.tag == current_tag;
        });
        assert(duplicate == ubtb.end() && "Multiple hits found in uBTB for the same tag!");

        // go on to update the mruList
        it->tick = curTick();  // Update timestamp for MRU
        // might be unnecessary, considering the heap is updated on every reaplacement
        std::make_heap(mruList.begin(), mruList.end(), older());
    }

    return it;
}


void
UBTB::replaceOldEntry(UBTBIter oldEntryIter, FullBTBPrediction &newPrediction)
{
    assert(newPrediction.getTakenEntry().valid);
    TickedUBTBEntry newEntry = TickedUBTBEntry(newPrediction.getTakenEntry(), curTick());
    // important! this is so that target set by RAS or ITTAGE is used
    newEntry.target = newPrediction.getTarget(predictWidth);
    // important: update tag (mbtb and ubtb have different tags, even diffferent tag length)
    newEntry.tag = getTag(newPrediction.bbStart);
    /*  save the number of conditional branches before the taken branch
     *  this is useful in the prediction phase: to generate the correct speculative history information
     */
    newEntry.numNTConds = newPrediction.getHistInfo().first;
    if (newPrediction.getTakenEntry().isCond) {
        newEntry.numNTConds--;
        assert(newEntry.numNTConds >= 0);
    }
    *oldEntryIter = newEntry;
}


void
UBTB::updateUsingS3Pred(FullBTBPrediction &s3Pred)
{


    UBTBIter s0EntryIter = lastPred.hit_entry;
    if (s0EntryIter != ubtb.end()) {
        assert(s0EntryIter->valid); //lookup() should only return valid entry
    }
    auto s3TakenEntry = s3Pred.getTakenEntry();
    if (s0EntryIter != ubtb.end() && !s3TakenEntry.valid) {
        // S0 has a hit entry, but S3 predicts fall through
        updateUCtr(s0EntryIter->uctr, false);
        if (s0EntryIter->uctr == 0) {
            s0EntryIter->valid = false;
        }
    } else if (s0EntryIter == ubtb.end() && s3TakenEntry.valid) {
        /* S0 misses, but S3 predicts taken,
         * generate new entry and replace another using LRU
         */
        UBTBIter toBeReplacedIter;
        // First try to find an invalid entry in the set
        bool foundInvalidEntry = false;

        for (auto it = ubtb.begin(); it != ubtb.end(); ++it) {
            if (!it->valid) {
                toBeReplacedIter = it;
                foundInvalidEntry = true;
                break;
            }
        }

        // If no invalid entry found, use LRU policy
        // TODO: consider using LRU only among the entries with the least confidence(smallest uctr)
        if (!foundInvalidEntry) {
            // Find the least recently used entry
            std::make_heap(mruList.begin(), mruList.end(), older());
            toBeReplacedIter = mruList.front();
        }

        // Replace the entry with the new prediction
        replaceOldEntry(toBeReplacedIter, s3Pred);

    } else if (s0EntryIter != ubtb.end() && s3TakenEntry.valid) {
        // both S0 and S3 predict taken
        if (s0EntryIter->pc != s3Pred.controlAddr() || s0EntryIter->target != s3Pred.getTarget(predictWidth)) {
            // S0 and S3 predict different branch instruction
            updateUCtr(s0EntryIter->uctr, false);
            if (s0EntryIter->uctr == 0) {
                // replace the old entry with the new one
                replaceOldEntry(s0EntryIter, s3Pred);
            }
        } else {
            // S0 and S3 predict the same (brpc and target)
            updateUCtr(s0EntryIter->uctr, true);
        }
    } else {
        // both S0 and S3 predict fall through, do nothing
    }
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

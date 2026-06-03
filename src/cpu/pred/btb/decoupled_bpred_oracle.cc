#include <sqlite3.h>

#include <cinttypes>

#include "base/intmath.hh"
#include "base/output.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

namespace
{

uint64_t
sqliteUint64(sqlite3_stmt *stmt, int column)
{
    return static_cast<uint64_t>(sqlite3_column_int64(stmt, column));
}

} // anonymous namespace

void
DecoupledBPUWithBTB::initSelectiveOracle(const Params &params)
{
    selectiveOraclePanicOnMismatch = params.selectiveOraclePanicOnMismatch;

    for (auto pc : params.selectiveOracleBranchPCs) {
        selectiveOracleBranchPCs.insert(pc);
    }

    if (!params.selectiveOracleReplayDBFile.empty()) {
        loadSelectiveOracleReplayDB(params.selectiveOracleReplayDBFile);
        selectiveOracleReplaying = true;
        warn("Loaded selective oracle replay DB %s\n",
             params.selectiveOracleReplayDBFile.c_str());
    }
}

void
DecoupledBPUWithBTB::initSelectiveOracleTrace()
{
    std::vector<std::pair<std::string, DataType>> fields_vec = {
        std::make_pair("blockId", UINT64),
        std::make_pair("tid", UINT64),
        std::make_pair("ftqId", UINT64),
        std::make_pair("startPC", UINT64),
        std::make_pair("outcomeIdx", UINT64),
        std::make_pair("branchPC", UINT64),
        std::make_pair("taken", UINT64),
        std::make_pair("target", UINT64),
        std::make_pair("fallThruPC", UINT64),
        std::make_pair("size", UINT64)
    };
    selectiveOracleTraceManager =
        bpdb.addAndGetTrace("SELECTIVEORACLETRACE", fields_vec);
    selectiveOracleTraceManager->init_table();
    removeGivenSwitch(bpDBSwitches, std::string("oracle"));
    selectiveOracleRecording = true;
    someDBenabled = true;
}

void
DecoupledBPUWithBTB::loadSelectiveOracleReplayDB(const std::string &path)
{
    const auto resolved_path = simout.resolve(path);
    sqlite3 *db = nullptr;
    int rc = sqlite3_open(resolved_path.c_str(), &db);
    panic_if(rc != SQLITE_OK, "Failed to open selective oracle replay DB %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));

    const char *query =
        "SELECT blockId, tid, ftqId, startPC, outcomeIdx, branchPC, taken, "
        "target, fallThruPC, size FROM SELECTIVEORACLETRACE "
        "ORDER BY blockId, outcomeIdx;";
    sqlite3_stmt *stmt = nullptr;
    rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
    panic_if(rc != SQLITE_OK, "Failed to query SELECTIVEORACLETRACE in %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));

    bool have_block = false;
    uint64_t current_block_id = 0;
    uint64_t next_outcome_idx = 0;
    SelectiveOracleBlock current_block;
    std::vector<SelectiveOracleBlock> blocks;
    std::array<std::unordered_set<Addr>, MaxThreads> selected_start_pcs;

    auto keep_block = [&blocks](const SelectiveOracleBlock &block) {
        if (block.outcomes.empty()) {
            return;
        }
        blocks.push_back(block);
    };

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const uint64_t block_id = sqliteUint64(stmt, 0);
        const auto tid = static_cast<ThreadID>(sqliteUint64(stmt, 1));
        const uint64_t ftq_id = sqliteUint64(stmt, 2);
        const Addr start_pc = sqliteUint64(stmt, 3);
        const uint64_t outcome_idx = sqliteUint64(stmt, 4);

        if (!have_block || block_id != current_block_id) {
            if (have_block) {
                keep_block(current_block);
            }
            have_block = true;
            current_block_id = block_id;
            next_outcome_idx = 0;
            current_block = SelectiveOracleBlock();
            current_block.tid = tid;
            current_block.recordFtqId = ftq_id;
            current_block.startPC = start_pc;
        } else {
            panic_if(current_block.tid != tid ||
                     current_block.recordFtqId != ftq_id ||
                     current_block.startPC != start_pc,
                     "Inconsistent selective oracle block %" PRIu64, block_id);
        }

        panic_if(outcome_idx != next_outcome_idx,
                 "Unexpected selective oracle outcome index %" PRIu64
                 " for block %" PRIu64 ", expected %" PRIu64,
                 outcome_idx, block_id, next_outcome_idx);
        next_outcome_idx++;

        SelectiveOracleOutcome outcome;
        outcome.tid = tid;
        outcome.ftqId = ftq_id;
        outcome.startPC = start_pc;
        outcome.branchPC = sqliteUint64(stmt, 5);
        outcome.taken = sqliteUint64(stmt, 6) != 0;
        outcome.target = sqliteUint64(stmt, 7);
        outcome.fallThrough = sqliteUint64(stmt, 8);
        outcome.size = static_cast<unsigned>(sqliteUint64(stmt, 9));

        if (selectiveOracleEnabledForPC(outcome.branchPC)) {
            current_block.hasSelectedPC = true;
        }
        panic_if(!current_block.outcomes.empty() &&
                 current_block.outcomes.back().taken,
                 "Selective oracle block %" PRIu64
                 " has outcomes after a taken branch", block_id);
        current_block.outcomes.push_back(outcome);
    }

    panic_if(rc != SQLITE_DONE, "Failed while reading selective oracle replay DB %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));
    if (have_block) {
        keep_block(current_block);
    }

    for (const auto &block : blocks) {
        panic_if(block.tid >= MaxThreads, "Bad selective oracle replay tid %u",
                 block.tid);
        if (selectiveOracleBranchPCs.empty() || block.hasSelectedPC) {
            selected_start_pcs[block.tid].insert(block.startPC);
        }
    }

    for (const auto &block : blocks) {
        panic_if(block.tid >= MaxThreads, "Bad selective oracle replay tid %u",
                 block.tid);
        if (!selectiveOracleBranchPCs.empty() &&
            selected_start_pcs[block.tid].count(block.startPC) == 0) {
            continue;
        }

        selectiveOracleReplayBlocks[block.tid][block.startPC].push_back(block);
        selectiveOracleReplayBlocksLoadedCount++;
        selectiveOracleReplayOutcomesLoadedCount += block.outcomes.size();
        if (!selectiveOracleBranchPCs.empty()) {
            selectiveOracleReplayStartPCs[block.tid].insert(block.startPC);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void
DecoupledBPUWithBTB::publishSelectiveOracleLoadedStat()
{
    if (dbpBtbStats.selectiveOracleReplayBlocksLoaded.value() != 0 ||
        dbpBtbStats.selectiveOracleReplayOutcomesLoaded.value() != 0) {
        return;
    }
    dbpBtbStats.selectiveOracleReplayBlocksLoaded +=
        selectiveOracleReplayBlocksLoadedCount;
    dbpBtbStats.selectiveOracleReplayOutcomesLoaded +=
        selectiveOracleReplayOutcomesLoadedCount;
}

bool
DecoupledBPUWithBTB::selectiveOracleEnabledForPC(Addr branchPC) const
{
    return selectiveOracleBranchPCs.empty() ||
           selectiveOracleBranchPCs.count(branchPC) != 0;
}

bool
DecoupledBPUWithBTB::selectiveOracleReplayEnabledForStartPC(
    ThreadID tid,
    Addr startPC) const
{
    panic_if(tid >= MaxThreads, "Bad selective oracle replay tid %u", tid);
    return selectiveOracleBranchPCs.empty() ||
           selectiveOracleReplayStartPCs[tid].count(startPC) != 0;
}

void
DecoupledBPUWithBTB::recordSelectiveOracleOutcome(
    uint64_t ftqId,
    const FetchTarget &entry,
    const DynInstPtr &inst,
    bool taken,
    Addr target,
    Addr fallThrough)
{
    if (!selectiveOracleRecording || !inst->isCondCtrl()) {
        return;
    }

    const Addr branch_pc = inst->pcState().instAddr();
    auto &block = selectiveOracleRecordBlocks[inst->threadNumber][ftqId];
    if (block.outcomes.empty()) {
        block.tid = inst->threadNumber;
        block.recordFtqId = ftqId;
        block.startPC = entry.startPC;
        block.endPC = entry.predEndPC;
    }

    SelectiveOracleOutcome outcome;
    outcome.tid = inst->threadNumber;
    outcome.ftqId = ftqId;
    outcome.startPC = entry.startPC;
    outcome.branchPC = branch_pc;
    outcome.taken = taken;
    outcome.target = target;
    outcome.fallThrough = fallThrough;
    outcome.size = fallThrough - branch_pc;

    if (selectiveOracleEnabledForPC(branch_pc)) {
        block.hasSelectedPC = true;
    }
    block.outcomes.push_back(outcome);
}

Addr
DecoupledBPUWithBTB::selectiveOracleBlockFallThrough(Addr startPC) const
{
    return (startPC + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
}

void
DecoupledBPUWithBTB::writeSelectiveOracleRecordTrace(
    const SelectiveOracleBlock &block)
{
    if (block.outcomes.empty()) {
        return;
    }

    panic_if(!selectiveOracleTraceManager,
             "Selective oracle trace manager is not initialized");

    const uint64_t block_id = selectiveOracleNextRecordBlockId++;
    for (size_t idx = 0; idx < block.outcomes.size(); ++idx) {
        const auto &outcome = block.outcomes[idx];
        Record record;
        record._tick = curTick();
        record._uint64_data["blockId"] = block_id;
        record._uint64_data["tid"] = block.tid;
        record._uint64_data["ftqId"] = block.recordFtqId;
        record._uint64_data["startPC"] = block.startPC;
        record._uint64_data["outcomeIdx"] = idx;
        record._uint64_data["branchPC"] = outcome.branchPC;
        record._uint64_data["taken"] = outcome.taken ? 1 : 0;
        record._uint64_data["target"] = outcome.target;
        record._uint64_data["fallThruPC"] = outcome.fallThrough;
        record._uint64_data["size"] = outcome.size;
        selectiveOracleTraceManager->write_record(record);
    }

    dbpBtbStats.selectiveOracleRecordBlocks++;
    dbpBtbStats.selectiveOracleRecordOutcomes += block.outcomes.size();
}

void
DecoupledBPUWithBTB::appendSelectiveOracleRecordBlock(
    SelectiveOracleRecordBuilder &builder,
    const SelectiveOracleBlock &block)
{
    if (builder.complete) {
        return;
    }

    if (block.startPC >= builder.limitPC) {
        builder.complete = true;
        return;
    }

    for (const auto &outcome : block.outcomes) {
        if (outcome.branchPC < builder.nextPC) {
            continue;
        }
        if (outcome.branchPC >= builder.limitPC) {
            builder.complete = true;
            return;
        }

        auto logical_outcome = outcome;
        logical_outcome.ftqId = builder.block.recordFtqId;
        logical_outcome.startPC = builder.block.startPC;
        if (selectiveOracleEnabledForPC(logical_outcome.branchPC)) {
            builder.block.hasSelectedPC = true;
        }
        builder.block.outcomes.push_back(logical_outcome);

        builder.nextPC = logical_outcome.taken ?
            logical_outcome.target : logical_outcome.fallThrough;
        if (logical_outcome.taken || builder.nextPC >= builder.limitPC) {
            builder.complete = true;
            return;
        }
    }

    if (!block.fallThroughToEnd) {
        builder.complete = true;
        return;
    }

    if (block.endPC <= builder.nextPC) {
        builder.complete = true;
        return;
    }

    builder.nextPC = block.endPC;
    if (builder.nextPC >= builder.limitPC) {
        builder.complete = true;
    }
}

void
DecoupledBPUWithBTB::flushSelectiveOracleRecordBuilders()
{
    if (!selectiveOracleRecording) {
        return;
    }

    for (auto &builders : selectiveOracleActiveRecordBuilders) {
        while (!builders.empty()) {
            writeSelectiveOracleRecordTrace(builders.front().block);
            builders.pop_front();
        }
    }
}

void
DecoupledBPUWithBTB::writeSelectiveOracleRecordBlock(
    ThreadID tid,
    FetchTargetId targetId,
    const FetchTarget &target)
{
    if (!selectiveOracleRecording) {
        return;
    }

    auto &record_blocks = selectiveOracleRecordBlocks[tid];
    auto it = record_blocks.find(targetId);
    if (it == record_blocks.end()) {
        SelectiveOracleBlock block;
        block.tid = tid;
        block.recordFtqId = targetId;
        block.startPC = target.startPC;
        it = record_blocks.emplace(targetId, block).first;
    }

    auto raw_block = it->second;
    raw_block.endPC = target.getTaken() ? target.getTakenTarget() :
        target.predEndPC;
    raw_block.fallThroughToEnd = !target.getTaken();
    auto &builders = selectiveOracleActiveRecordBuilders[tid];
    std::deque<SelectiveOracleRecordBuilder> remaining_builders;
    while (!builders.empty()) {
        auto builder = builders.front();
        builders.pop_front();

        if (builder.nextPC == raw_block.startPC) {
            appendSelectiveOracleRecordBlock(builder, raw_block);
        } else {
            builder.complete = true;
        }

        if (builder.complete) {
            writeSelectiveOracleRecordTrace(builder.block);
        } else {
            remaining_builders.push_back(builder);
        }
    }
    builders.swap(remaining_builders);

    SelectiveOracleRecordBuilder builder;
    builder.block.tid = tid;
    builder.block.recordFtqId = targetId;
    builder.block.startPC = target.startPC;
    builder.nextPC = target.startPC;
    builder.limitPC = selectiveOracleBlockFallThrough(target.startPC);
    appendSelectiveOracleRecordBlock(builder, raw_block);

    if (builder.complete) {
        writeSelectiveOracleRecordTrace(builder.block);
    } else {
        builders.push_back(builder);
    }

    record_blocks.erase(it);
}

bool
DecoupledBPUWithBTB::getSelectiveOracleBlock(
    ThreadID tid,
    Addr startPC,
    SelectiveOracleBlock &block)
{
    auto &blocks_by_start_pc = selectiveOracleReplayBlocks[tid];
    auto it = blocks_by_start_pc.find(startPC);
    if (it == blocks_by_start_pc.end() || it->second.empty()) {
        dbpBtbStats.selectiveOracleReplayTraceMissing++;
        if (selectiveOraclePanicOnMismatch) {
            panic("Missing selective oracle block for tid %u startPC %#lx",
                  tid, startPC);
        }
        return false;
    }

    auto &queue = it->second;
    block = queue.front();
    queue.pop_front();
    return true;
}

void
DecoupledBPUWithBTB::restoreSelectiveOracleBlock(
    ThreadID tid,
    FetchTargetId targetId)
{
    if (!selectiveOracleReplaying) {
        return;
    }

    auto &consumed_blocks = selectiveOracleConsumedBlocks[tid];
    auto it = consumed_blocks.find(targetId);
    if (it == consumed_blocks.end()) {
        return;
    }

    auto &blocks = it->second;
    for (auto block_it = blocks.rbegin(); block_it != blocks.rend(); ++block_it) {
        selectiveOracleReplayBlocks[tid][block_it->startPC].push_front(*block_it);
        dbpBtbStats.selectiveOracleReplayBlocksRestored++;
    }
    consumed_blocks.erase(it);
}

void
DecoupledBPUWithBTB::commitSelectiveOracleBlock(
    ThreadID tid,
    FetchTargetId targetId)
{
    if (!selectiveOracleReplaying) {
        return;
    }
    selectiveOracleConsumedBlocks[tid].erase(targetId);
}

void
DecoupledBPUWithBTB::setOracleCondTaken(
    FullBTBPrediction &pred,
    BTBEntry &entry,
    const SelectiveOracleOutcome &outcome)
{
    auto branch_pc = entry.pc;
    auto it = CondTakens_find(pred.condTakens, branch_pc);
    if (it == pred.condTakens.end()) {
        pred.condTakens.push_back(std::make_pair(entry.pc, outcome.taken));
    } else {
        it->second = outcome.taken;
    }

    if (outcome.size != 0) {
        entry.size = outcome.size;
    }
    if (outcome.taken) {
        entry.target = outcome.target;
    }
}

bool
DecoupledBPUWithBTB::getCurrentCondTaken(
    const FullBTBPrediction &pred,
    Addr branchPC) const
{
    auto it = CondTakens_find(pred.condTakens, branchPC);
    return it != pred.condTakens.end() && it->second;
}

void
DecoupledBPUWithBTB::applySelectiveOracle(ThreadID tid, FetchTargetId targetId)
{
    if (!selectiveOracleReplaying) {
        return;
    }
    publishSelectiveOracleLoadedStat();

    auto &thread = threads[tid];
    auto &final_pred = thread.finalPred;
    if (!selectiveOracleReplayEnabledForStartPC(tid, thread.s0PC)) {
        dbpBtbStats.selectiveOracleReplayNoEligibleBranch++;
        return;
    }

    const auto &blocks_by_start_pc = selectiveOracleReplayBlocks[tid];
    const auto trace_it = blocks_by_start_pc.find(thread.s0PC);
    if (trace_it == blocks_by_start_pc.end()) {
        dbpBtbStats.selectiveOracleReplayNoEligibleBranch++;
        if (selectiveOracleBranchPCs.empty() && selectiveOraclePanicOnMismatch) {
            panic("Missing selective oracle startPC %#lx for tid %u",
                  thread.s0PC, tid);
        }
        return;
    }

    dbpBtbStats.selectiveOracleReplayAttempts++;

    SelectiveOracleBlock block;
    if (!getSelectiveOracleBlock(tid, thread.s0PC, block)) {
        return;
    }

    auto &consumed_blocks = selectiveOracleConsumedBlocks[tid][targetId];
    consumed_blocks.clear();
    consumed_blocks.push_back(block);
    dbpBtbStats.selectiveOracleReplayBlocksConsumed++;

    size_t outcome_idx = 0;
    for (auto &entry : final_pred.btbEntries) {
        if (!entry.valid) {
            continue;
        }
        if (!entry.isCond) {
            break;
        }

        while (outcome_idx < block.outcomes.size() &&
               block.outcomes[outcome_idx].branchPC < entry.pc) {
            if (block.outcomes[outcome_idx].taken) {
                return;
            }
            dbpBtbStats.selectiveOracleReplaySkippedOutcomes++;
            outcome_idx++;
        }

        if (outcome_idx < block.outcomes.size() &&
            block.outcomes[outcome_idx].branchPC == entry.pc) {
            const auto &outcome = block.outcomes[outcome_idx++];
            setOracleCondTaken(final_pred, entry, outcome);
            dbpBtbStats.selectiveOracleReplayApplied++;
            if (outcome.taken) {
                dbpBtbStats.selectiveOracleReplayTaken++;
                break;
            }
            dbpBtbStats.selectiveOracleReplayNotTaken++;
            continue;
        }

        const bool block_falls_through =
            block.outcomes.empty() || !block.outcomes.back().taken;
        const bool trace_passed_this_entry =
            (outcome_idx < block.outcomes.size() &&
             block.outcomes[outcome_idx].branchPC > entry.pc) ||
            (outcome_idx == block.outcomes.size() &&
             block_falls_through &&
             entry.pc < selectiveOracleBlockFallThrough(block.startPC));
        if (trace_passed_this_entry && getCurrentCondTaken(final_pred, entry.pc)) {
            SelectiveOracleOutcome not_taken;
            not_taken.tid = tid;
            not_taken.ftqId = targetId;
            not_taken.startPC = thread.s0PC;
            not_taken.branchPC = entry.pc;
            not_taken.taken = false;
            not_taken.target = entry.getEnd();
            not_taken.fallThrough = entry.getEnd();
            not_taken.size = entry.size;
            setOracleCondTaken(final_pred, entry, not_taken);
            dbpBtbStats.selectiveOracleReplayApplied++;
            dbpBtbStats.selectiveOracleReplayNotTaken++;
        }
    }
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

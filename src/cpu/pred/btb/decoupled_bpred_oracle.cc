#include <sqlite3.h>

#include <algorithm>
#include <cinttypes>
#include <sstream>
#include <unordered_map>

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

uint64_t
parseOracleUint64(const std::string &value)
{
    size_t parsed = 0;
    const auto result = std::stoull(value, &parsed, 0);
    panic_if(parsed != value.size(), "Bad selective oracle integer token %s",
             value.c_str());
    return result;
}

std::vector<std::string>
splitOracleCSVLine(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

std::vector<std::string>
splitOracleToken(const std::string &text, char delim)
{
    std::vector<std::string> tokens;
    std::stringstream ss(text);
    std::string token;
    while (std::getline(ss, token, delim)) {
        tokens.push_back(token);
    }
    if (!text.empty() && text.back() == delim) {
        tokens.emplace_back();
    }
    return tokens;
}

} // anonymous namespace

void
DecoupledBPUWithBTB::initSelectiveOracle(const Params &params)
{
    selectiveOraclePanicOnMismatch = params.selectiveOraclePanicOnMismatch;
    selectiveOracleReplayLookahead = params.selectiveOracleReplayLookahead;
    selectiveOracleRecordCSVFile = params.selectiveOracleRecordCSVFile;
    selectiveOracleReplayCSVFile = params.selectiveOracleReplayCSVFile;

    for (auto pc : params.selectiveOracleBranchPCs) {
        selectiveOracleBranchPCs.insert(pc);
    }

    panic_if(!params.selectiveOracleReplayDBFile.empty() &&
             !selectiveOracleReplayCSVFile.empty(),
             "selectiveOracleReplayDBFile and selectiveOracleReplayCSVFile "
             "cannot both be enabled");

    if (!selectiveOracleRecordCSVFile.empty()) {
        initSelectiveOracleCSVRecord(selectiveOracleRecordCSVFile);
    }

    if (!params.selectiveOracleReplayDBFile.empty()) {
        loadSelectiveOracleReplayDB(params.selectiveOracleReplayDBFile);
        selectiveOracleReplaying = true;
        warn("Loaded selective oracle replay DB %s\n",
             params.selectiveOracleReplayDBFile.c_str());
    }

    if (!selectiveOracleReplayCSVFile.empty()) {
        loadSelectiveOracleReplayCSV(selectiveOracleReplayCSVFile);
        selectiveOracleReplaying = true;
        warn("Loaded selective oracle replay CSV %s\n",
             selectiveOracleReplayCSVFile.c_str());
    }
}

void
DecoupledBPUWithBTB::initSelectiveOracleTrace()
{
    std::vector<std::pair<std::string, DataType>> block_fields_vec = {
        std::make_pair("blockId", UINT64),
        std::make_pair("tid", UINT64),
        std::make_pair("ftqId", UINT64),
        std::make_pair("startPC", UINT64),
        std::make_pair("outcomeCount", UINT64)
    };
    selectiveOracleBlockTraceManager =
        bpdb.addAndGetTrace("ORACLE_BLOCK", block_fields_vec);
    selectiveOracleBlockTraceManager->init_table();

    std::vector<std::pair<std::string, DataType>> outcome_fields_vec = {
        std::make_pair("blockId", UINT64),
        std::make_pair("outcomeIdx", UINT64),
        std::make_pair("branchPC", UINT64),
        std::make_pair("taken", UINT64),
        std::make_pair("target", UINT64),
        std::make_pair("fallThruPC", UINT64),
        std::make_pair("size", UINT64)
    };
    selectiveOracleOutcomeTraceManager =
        bpdb.addAndGetTrace("ORACLE_OUTCOME", outcome_fields_vec);
    selectiveOracleOutcomeTraceManager->init_table();
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

    const char *block_query =
        "SELECT blockId, tid, ftqId, startPC, outcomeCount "
        "FROM ORACLE_BLOCK ORDER BY blockId;";
    sqlite3_stmt *stmt = nullptr;
    rc = sqlite3_prepare_v2(db, block_query, -1, &stmt, nullptr);
    panic_if(rc != SQLITE_OK, "Failed to query ORACLE_BLOCK in %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));

    std::vector<SelectiveOracleBlock> blocks;
    std::unordered_map<uint64_t, size_t> block_indices;
    std::unordered_map<uint64_t, uint64_t> expected_outcome_counts;
    std::array<std::unordered_set<Addr>, MaxThreads> selected_start_pcs;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const uint64_t block_id = sqliteUint64(stmt, 0);
        const auto tid = static_cast<ThreadID>(sqliteUint64(stmt, 1));
        const uint64_t ftq_id = sqliteUint64(stmt, 2);
        const Addr start_pc = sqliteUint64(stmt, 3);
        const uint64_t outcome_count = sqliteUint64(stmt, 4);

        panic_if(tid >= MaxThreads, "Bad selective oracle replay tid %u", tid);
        panic_if(block_indices.count(block_id) != 0,
                 "Duplicated selective oracle block %" PRIu64, block_id);

        SelectiveOracleBlock block;
        block.tid = tid;
        block.recordFtqId = ftq_id;
        block.startPC = start_pc;
        block_indices[block_id] = blocks.size();
        expected_outcome_counts[block_id] = outcome_count;
        blocks.push_back(block);
    }

    panic_if(rc != SQLITE_DONE, "Failed while reading ORACLE_BLOCK from %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));
    sqlite3_finalize(stmt);

    const char *outcome_query =
        "SELECT blockId, outcomeIdx, branchPC, taken, target, fallThruPC, size "
        "FROM ORACLE_OUTCOME ORDER BY blockId, outcomeIdx;";
    stmt = nullptr;
    rc = sqlite3_prepare_v2(db, outcome_query, -1, &stmt, nullptr);
    panic_if(rc != SQLITE_OK, "Failed to query ORACLE_OUTCOME in %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));

    uint64_t current_block_id = 0;
    uint64_t next_outcome_idx = 0;
    bool have_outcome_block = false;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const uint64_t block_id = sqliteUint64(stmt, 0);
        const uint64_t outcome_idx = sqliteUint64(stmt, 1);
        auto index_it = block_indices.find(block_id);
        panic_if(index_it == block_indices.end(),
                 "Selective oracle outcome references unknown block %" PRIu64,
                 block_id);

        if (!have_outcome_block || block_id != current_block_id) {
            have_outcome_block = true;
            current_block_id = block_id;
            next_outcome_idx = 0;
        }

        panic_if(outcome_idx != next_outcome_idx,
                 "Unexpected selective oracle outcome index %" PRIu64
                 " for block %" PRIu64 ", expected %" PRIu64,
                 outcome_idx, block_id, next_outcome_idx);
        next_outcome_idx++;

        auto &block = blocks[index_it->second];
        panic_if(!block.outcomes.empty() && block.outcomes.back().taken,
                 "Selective oracle block %" PRIu64
                 " has outcomes after a taken branch", block_id);
        panic_if(block.outcomes.size() >= expected_outcome_counts[block_id],
                 "Selective oracle block %" PRIu64
                 " has more outcomes than expected", block_id);

        SelectiveOracleOutcome outcome;
        outcome.tid = block.tid;
        outcome.ftqId = block.recordFtqId;
        outcome.startPC = block.startPC;
        outcome.branchPC = sqliteUint64(stmt, 2);
        outcome.taken = sqliteUint64(stmt, 3) != 0;
        outcome.target = sqliteUint64(stmt, 4);
        outcome.fallThrough = sqliteUint64(stmt, 5);
        outcome.size = static_cast<unsigned>(sqliteUint64(stmt, 6));

        if (selectiveOracleEnabledForPC(outcome.branchPC)) {
            block.hasSelectedPC = true;
        }
        block.outcomes.push_back(outcome);
    }

    panic_if(rc != SQLITE_DONE, "Failed while reading ORACLE_OUTCOME from %s: %s",
             resolved_path.c_str(), sqlite3_errmsg(db));
    sqlite3_finalize(stmt);

    for (const auto &entry : block_indices) {
        const auto block_id = entry.first;
        const auto &block = blocks[entry.second];
        const auto expected = expected_outcome_counts[block_id];
        panic_if(block.outcomes.size() != expected,
                 "Selective oracle block %" PRIu64 " has %zu outcomes, "
                 "expected %" PRIu64,
                 block_id, block.outcomes.size(), expected);
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

        selectiveOracleReplayStream[block.tid].push_back(block);
        selectiveOracleReplayBlocksLoadedCount++;
        selectiveOracleReplayOutcomesLoadedCount += block.outcomes.size();
        if (!selectiveOracleBranchPCs.empty()) {
            selectiveOracleReplayStartPCs[block.tid].insert(block.startPC);
        }
    }

    sqlite3_close(db);
}

void
DecoupledBPUWithBTB::initSelectiveOracleCSVRecord(const std::string &path)
{
    const auto resolved_path = simout.resolve(path);
    selectiveOracleCSVStream.open(resolved_path);
    panic_if(!selectiveOracleCSVStream.is_open(),
             "Failed to open selective oracle CSV record file %s",
             resolved_path.c_str());

    selectiveOracleCSVStream << "tid,ftqId,startPC,outcomes\n";
    selectiveOracleCSVRecording = true;
    selectiveOracleRecording = true;
}

void
DecoupledBPUWithBTB::loadSelectiveOracleReplayCSV(const std::string &path)
{
    const auto resolved_path = simout.resolve(path);
    std::ifstream input(resolved_path);
    panic_if(!input.is_open(),
             "Failed to open selective oracle replay CSV %s",
             resolved_path.c_str());

    std::string line;
    if (std::getline(input, line)) {
        if (line != "tid,ftqId,startPC,outcomes") {
            input.clear();
            input.seekg(0);
        }
    }

    std::vector<SelectiveOracleBlock> blocks;
    std::array<std::unordered_set<Addr>, MaxThreads> selected_start_pcs;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        auto fields = splitOracleCSVLine(line);
        panic_if(fields.size() < 4,
                 "Bad selective oracle CSV line: %s", line.c_str());

        SelectiveOracleBlock block;
        block.tid = static_cast<ThreadID>(parseOracleUint64(fields[0]));
        block.recordFtqId = parseOracleUint64(fields[1]);
        block.startPC = parseOracleUint64(fields[2]);
        panic_if(block.tid >= MaxThreads, "Bad selective oracle replay tid %u",
                 block.tid);

        if (!fields[3].empty()) {
            auto outcome_tokens = splitOracleToken(fields[3], ';');
            for (const auto &outcome_text : outcome_tokens) {
                if (outcome_text.empty()) {
                    continue;
                }
                auto values = splitOracleToken(outcome_text, ':');
                panic_if(values.size() != 5,
                         "Bad selective oracle CSV outcome: %s",
                         outcome_text.c_str());

                SelectiveOracleOutcome outcome;
                outcome.tid = block.tid;
                outcome.ftqId = block.recordFtqId;
                outcome.startPC = block.startPC;
                outcome.branchPC = parseOracleUint64(values[0]);
                outcome.taken = parseOracleUint64(values[1]) != 0;
                outcome.target = parseOracleUint64(values[2]);
                outcome.fallThrough = parseOracleUint64(values[3]);
                outcome.size = static_cast<unsigned>(
                    parseOracleUint64(values[4]));

                if (selectiveOracleEnabledForPC(outcome.branchPC)) {
                    block.hasSelectedPC = true;
                }
                block.outcomes.push_back(outcome);
                panic_if(block.outcomes.size() > 1 &&
                         block.outcomes[block.outcomes.size() - 2].taken,
                         "Selective oracle CSV block has outcomes after taken "
                         "branch, startPC %#lx", block.startPC);
            }
        }

        blocks.push_back(block);
    }

    for (const auto &block : blocks) {
        if (selectiveOracleBranchPCs.empty() || block.hasSelectedPC) {
            selected_start_pcs[block.tid].insert(block.startPC);
        }
    }

    for (const auto &block : blocks) {
        if (!selectiveOracleBranchPCs.empty() &&
            selected_start_pcs[block.tid].count(block.startPC) == 0) {
            continue;
        }

        selectiveOracleReplayStream[block.tid].push_back(block);
        selectiveOracleReplayBlocksLoadedCount++;
        selectiveOracleReplayOutcomesLoadedCount += block.outcomes.size();
        if (!selectiveOracleBranchPCs.empty()) {
            selectiveOracleReplayStartPCs[block.tid].insert(block.startPC);
        }
    }
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
    if (selectiveOracleCSVRecording) {
        writeSelectiveOracleRecordCSV(block);
    }

    if (enableSelectiveOracleTrace) {
        panic_if(!selectiveOracleBlockTraceManager ||
                 !selectiveOracleOutcomeTraceManager,
                 "Selective oracle trace managers are not initialized");

        const uint64_t block_id = selectiveOracleNextRecordBlockId++;
        Record block_record;
        block_record._tick = curTick();
        block_record._uint64_data["blockId"] = block_id;
        block_record._uint64_data["tid"] = block.tid;
        block_record._uint64_data["ftqId"] = block.recordFtqId;
        block_record._uint64_data["startPC"] = block.startPC;
        block_record._uint64_data["outcomeCount"] = block.outcomes.size();
        selectiveOracleBlockTraceManager->write_record(block_record);

        for (size_t idx = 0; idx < block.outcomes.size(); ++idx) {
            const auto &outcome = block.outcomes[idx];
            Record record;
            record._tick = curTick();
            record._uint64_data["blockId"] = block_id;
            record._uint64_data["outcomeIdx"] = idx;
            record._uint64_data["branchPC"] = outcome.branchPC;
            record._uint64_data["taken"] = outcome.taken ? 1 : 0;
            record._uint64_data["target"] = outcome.target;
            record._uint64_data["fallThruPC"] = outcome.fallThrough;
            record._uint64_data["size"] = outcome.size;
            selectiveOracleOutcomeTraceManager->write_record(record);
        }
    }

    dbpBtbStats.selectiveOracleRecordBlocks++;
    dbpBtbStats.selectiveOracleRecordOutcomes += block.outcomes.size();
}

void
DecoupledBPUWithBTB::writeSelectiveOracleRecordCSV(
    const SelectiveOracleBlock &block)
{
    panic_if(!selectiveOracleCSVStream.is_open(),
             "Selective oracle CSV record file is not initialized");

    selectiveOracleCSVStream << std::dec << block.tid << ","
        << block.recordFtqId << ",0x" << std::hex << block.startPC << ",";

    for (size_t idx = 0; idx < block.outcomes.size(); ++idx) {
        const auto &outcome = block.outcomes[idx];
        if (idx != 0) {
            selectiveOracleCSVStream << ";";
        }
        selectiveOracleCSVStream << "0x" << std::hex << outcome.branchPC
            << ":" << std::dec << (outcome.taken ? 1 : 0)
            << ":0x" << std::hex << outcome.target
            << ":0x" << outcome.fallThrough
            << ":" << std::dec << outcome.size;
    }
    selectiveOracleCSVStream << "\n";
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

    if (selectiveOracleCSVStream.is_open()) {
        selectiveOracleCSVStream.flush();
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
    std::vector<SelectiveOracleBlock> &blocks)
{
    auto &stream = selectiveOracleReplayStream[tid];
    if (stream.empty()) {
        dbpBtbStats.selectiveOracleReplayTraceMissing++;
        if (selectiveOraclePanicOnMismatch) {
            panic("Missing selective oracle block for tid %u startPC %#lx",
                  tid, startPC);
        }
        return false;
    }

    const auto search_limit = std::min<size_t>(
        stream.size(), selectiveOracleReplayLookahead + 1);
    size_t match_idx = search_limit;
    for (size_t idx = 0; idx < search_limit; ++idx) {
        if (stream[idx].startPC == startPC) {
            match_idx = idx;
            break;
        }
    }

    if (match_idx == search_limit) {
        dbpBtbStats.selectiveOracleReplayTraceMissing++;
        if (selectiveOraclePanicOnMismatch) {
            panic("Missing selective oracle block for tid %u startPC %#lx "
                  "within lookahead %u",
                  tid, startPC, selectiveOracleReplayLookahead);
        }
        return false;
    }

    blocks.clear();
    for (size_t idx = 0; idx <= match_idx; ++idx) {
        blocks.push_back(stream.front());
        stream.pop_front();
    }
    dbpBtbStats.selectiveOracleReplaySkippedOutcomes += match_idx;
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
        selectiveOracleReplayStream[tid].push_front(*block_it);
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

    dbpBtbStats.selectiveOracleReplayAttempts++;

    std::vector<SelectiveOracleBlock> consumed;
    if (!getSelectiveOracleBlock(tid, thread.s0PC, consumed)) {
        return;
    }

    auto &consumed_blocks = selectiveOracleConsumedBlocks[tid][targetId];
    consumed_blocks = consumed;
    dbpBtbStats.selectiveOracleReplayBlocksConsumed += consumed_blocks.size();
    const auto &block = consumed_blocks.back();

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

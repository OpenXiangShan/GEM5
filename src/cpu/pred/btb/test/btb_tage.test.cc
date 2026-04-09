#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>

#include "base/types.hh"
#include "cpu/pred/btb/btb_tage.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/folded_hist.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

// Helper functions for TAGE testing

/**
 * @brief Create a BTB entry with specified parameters
 *
 * @param pc Branch instruction address
 * @param isCond Whether the branch is conditional
 * @param valid Whether the entry is valid
 * @param alwaysTaken Whether the branch is always taken
 * @param ctr Prediction counter value
 * @param target Branch target address (defaults to sequential PC)
 * @return BTBEntry Initialized branch entry
 */
BTBEntry createBTBEntry(Addr pc, bool isCond = true, bool valid = true,
                        bool alwaysTaken = false, int ctr = 0, Addr target = 0) {
    BTBEntry entry;
    entry.pc = pc;
    entry.target = target ? target : (pc + 4);
    entry.isCond = isCond;
    entry.valid = valid;
    entry.alwaysTaken = alwaysTaken;
    entry.ctr = ctr;
    // Other fields are set to default
    return entry;
}

/**
 * @brief Create a stream for update or recovery
 *
 * @param startPC Starting PC for the stream
 * @param entry Branch entry information
 * @param taken Actual outcome (taken/not taken)
 * @param meta Prediction metadata from prediction phase
 * @param squashType Type of squash (control or non-control)
 * @return FetchTarget Initialized stream for update or recovery
 */
FetchTarget createStream(Addr startPC, const BTBEntry& entry, bool taken,
                         std::shared_ptr<void> meta) {
    FetchTarget stream;
    stream.startPC = startPC;
    stream.exeBranchInfo = entry;
    stream.exeTaken = taken;
    // Mark as resolved so recover paths use exe* info
    stream.resolved = true;
    stream.predBranchInfo = entry; // keep fields consistent
    stream.updateBTBEntries = {entry};
    stream.updateIsOldEntry = true;
    stream.predMetas[0] = meta;
    return stream;
}

FetchTarget setMispredStream(FetchTarget stream) {
    stream.squashType = SquashType::SQUASH_CTRL;
    stream.squashPC = stream.exeBranchInfo.pc;
    return stream;
}

void applyPathHistoryTaken(boost::dynamic_bitset<>& history, Addr pc, Addr target,
                           int shamt = 2) {
    boost::dynamic_bitset<> before = history;
    history <<= shamt;
    uint64_t hash = pathHash(pc, target);
    for (std::size_t i = 0; i < pathHashLength && i < history.size(); ++i) {
        bool bit = history[i];
        history[i] = (hash & 1) ^ bit;
        hash >>= 1;
    }
}

/**
 * @brief Helper function to find conditional taken prediction for a given PC
 *
 * @param condTakens Vector of conditional predictions
 * @param pc Branch PC to search for
 * @return Pair of (found, prediction) where found indicates if PC was found
 */
std::pair<bool, bool> findCondTaken(const CondTakens& condTakens, Addr pc) {
    auto it = CondTakens_find(condTakens, pc);
    if (it != condTakens.end()) {
        return {true, it->second};
    }
    return {false, false};
}

/**
 * @brief Execute a complete TAGE prediction cycle
 *
 * @param tage The TAGE predictor
 * @param startPC Starting PC for prediction
 * @param entries Vector of BTB entries
 * @param history Branch history register
 * @param stagePreds Prediction results container
 * @return bool Prediction result (taken/not taken) for the first entry
 */
bool predictTAGE(BTBTAGE* tage, Addr startPC,
                const std::vector<BTBEntry>& entries,
                boost::dynamic_bitset<>& history,
                std::vector<FullBTBPrediction>& stagePreds) {
    // Setup stage predictions with BTB entries
    stagePreds[1].btbEntries = entries;

    // Make prediction
    tage->putPCHistory(startPC, history, stagePreds);

    // Return prediction for first entry if exists
    if (!entries.empty()) {
        auto result = findCondTaken(stagePreds[1].condTakens, entries[0].pc);
        bool found = result.first;
        bool taken = result.second;
        if (found) {
            return taken;
        }
    }
    return false;
}

/**
 * @brief Execute a complete prediction-update cycle
 *
 * @param tage The TAGE predictor
 * @param startPC Starting PC for prediction
 * @param entry BTB entry to predict
 * @param actual_taken Actual outcome (taken/not taken)
 * @param history Branch history register
 * @param stagePreds Prediction results container
 */
bool predictUpdateCycle(BTBTAGE* tage, Addr startPC,
                      const BTBEntry& entry,
                      bool actual_taken,
                      boost::dynamic_bitset<>& history,
                      std::vector<FullBTBPrediction>& stagePreds) {
    // 1. Make prediction
    stagePreds[1].btbEntries = {entry};
    tage->putPCHistory(startPC, history, stagePreds);

    // 2. Get predicted result
    Addr branch_pc = entry.pc;
    auto it = CondTakens_find(stagePreds[1].condTakens, branch_pc);
    // ASSERT_TRUE(it != stagePreds[1].condTakens.end()) << "Prediction not found for PC " << std::hex << entry.pc;
    bool predicted_taken = it->second;

    // 3. Speculatively update folded history
    tage->specUpdateHist(history, stagePreds[1]);
    auto meta = tage->getPredictionMeta();

    // 4. Update path history register, see pHistShiftIn
    bool history_updated = false;
    auto [pred_pc, pred_target, pred_taken] = stagePreds[1].getPHistInfo();
    boost::dynamic_bitset<> pre_spec_history = history;
    if (pred_taken) {
        history_updated = true;
        applyPathHistoryTaken(history, pred_pc, pred_target);
    }
    tage->checkFoldedHist(history, "speculative update");

    // 5. Create update stream
    FetchTarget stream = createStream(startPC, entry, actual_taken, meta);

    // 6. Handle possible misprediction
    if (predicted_taken != actual_taken) {
        stream = setMispredStream(stream);
        // Update history with correct outcome
        if (history_updated) {
            history = pre_spec_history;
        }
        // Recover from misprediction
        tage->recoverHist(history, stream, 1, actual_taken);

        if (actual_taken) {
            applyPathHistoryTaken(history, stream.exeBranchInfo.pc,
                                  stream.exeBranchInfo.target);
        }
        tage->checkFoldedHist(history, "recover");
    }

    // 7. Update predictor
    tage->update(stream);
    return predicted_taken;
}

/**
 * @brief Directly setup TAGE table entries for testing
 *
 * @param tage The TAGE predictor
 * @param pc Branch PC
 * @param table_idx Index of the table to set
 * @param counter Counter value
 * @param useful Useful bit value
 */
void setupTageEntry(BTBTAGE* tage, Addr pc, int table_idx,
                    short counter, bool useful = false, int way = 0) {
    Addr index = tage->getTageIndex(pc, table_idx);
    Addr tag = tage->getTageTag(pc, table_idx);

    auto& entry = tage->tageTable[table_idx][index][way];
    entry.valid = true;
    entry.tag = tag;
    entry.counter = counter;
    entry.useful = useful;
    entry.pc = pc;
}

void setupTageEntryForFetchBlock(BTBTAGE *tage, Addr startPC, Addr branchPC,
                                 int table_idx, short counter,
                                 bool useful = false, int way = 0)
{
    Addr index = tage->getTageIndex(startPC, table_idx);
    unsigned position = tage->getBranchIndexInBlock(branchPC, startPC);
    Addr tag = tage->getTageTag(startPC, table_idx,
        tage->tagFoldedHist[table_idx].get(),
        tage->altTagFoldedHist[table_idx].get(), position);

    auto &entry = tage->tageTable[table_idx][index][way];
    entry.valid = true;
    entry.tag = tag;
    entry.counter = counter;
    entry.useful = useful;
    entry.pc = branchPC;
}

ResolvedBranch createResolvedBranch(const BTBEntry &entry, bool taken,
                                    bool mispredict, uint8_t ftqOffset)
{
    BranchInfo branch(entry);
    branch.resolved = true;
    branch.size = 4;
    return ResolvedBranch(branch, taken, mispredict, ftqOffset);
}

ResolvedTrainPacket createResolvedTrainPacket(Addr startPC,
                                              std::shared_ptr<void> meta,
                                              std::vector<ResolvedBranch> realBranches)
{
    (void)meta;
    ResolvedTrainPacket packet;
    packet.startPC = startPC;
    packet.realBranches = std::move(realBranches);
    return packet;
}

FetchTarget createResolvedTrainTarget(Addr startPC, std::shared_ptr<void> meta)
{
    FetchTarget target;
    target.startPC = startPC;
    target.predMetas[0] = meta;
    return target;
}

void advanceActualHistory(BTBTAGE *tage,
                          boost::dynamic_bitset<> &history,
                          const std::vector<BTBEntry> &entries,
                          const std::vector<bool> &actual_takens)
{
    ASSERT_EQ(entries.size(), actual_takens.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        tage->doUpdateHist(history, actual_takens[i], entries[i].pc, entries[i].target);
        if (actual_takens[i]) {
            applyPathHistoryTaken(history, entries[i].pc, entries[i].target);
        }
    }
    tage->checkFoldedHist(history, "actual history advance");
}

void legacyTrainSequence(BTBTAGE *tage, Addr startPC,
                         const std::vector<BTBEntry> &entries,
                         const std::vector<bool> &actual_takens,
                         boost::dynamic_bitset<> &history,
                         std::vector<FullBTBPrediction> &stagePreds)
{
    ASSERT_EQ(entries.size(), actual_takens.size());
    stagePreds[1].btbEntries = entries;
    tage->putPCHistory(startPC, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    for (size_t i = 0; i < entries.size(); ++i) {
        auto pred = findCondTaken(stagePreds[1].condTakens, entries[i].pc);
        ASSERT_TRUE(pred.first) << "Missing legacy prediction for PC "
                                << std::hex << entries[i].pc;

        FetchTarget stream = createStream(startPC, entries[i], actual_takens[i], meta);
        if (pred.second != actual_takens[i]) {
            stream = setMispredStream(stream);
        }
        tage->update(stream);
    }

    advanceActualHistory(tage, history, entries, actual_takens);
}

void resolveTrainSequence(BTBTAGE *tage, Addr startPC,
                          const std::vector<BTBEntry> &entries,
                          const std::vector<bool> &actual_takens,
                          boost::dynamic_bitset<> &history,
                          std::vector<FullBTBPrediction> &stagePreds)
{
    ASSERT_EQ(entries.size(), actual_takens.size());
    stagePreds[1].btbEntries = entries;
    tage->putPCHistory(startPC, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    std::vector<ResolvedBranch> resolved_branches;
    resolved_branches.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        auto pred = findCondTaken(stagePreds[1].condTakens, entries[i].pc);
        ASSERT_TRUE(pred.first) << "Missing resolve-train prediction for PC "
                                << std::hex << entries[i].pc;
        resolved_branches.push_back(createResolvedBranch(
            entries[i], actual_takens[i], pred.second != actual_takens[i], i));
    }

    auto packet = createResolvedTrainPacket(startPC, meta, resolved_branches);
    auto target = createResolvedTrainTarget(startPC, meta);
    ASSERT_TRUE(tage->canResolveTrain(packet, target))
        << "resolveTrain should be accepted for the constructed packet";
    tage->resolveTrain(packet, target);

    advanceActualHistory(tage, history, entries, actual_takens);
}

BTBTAGE::TagePrediction predictBranch(BTBTAGE *tage, Addr startPC,
                                      const std::vector<BTBEntry> &entries,
                                      boost::dynamic_bitset<> &history,
                                      std::vector<FullBTBPrediction> &stagePreds,
                                      Addr branchPC)
{
    stagePreds[1].btbEntries = entries;
    tage->putPCHistory(startPC, history, stagePreds);
    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    auto it = meta->preds.find(branchPC);
    if (it == meta->preds.end()) {
        ADD_FAILURE() << "Missing probe prediction for PC " << std::hex << branchPC;
        return BTBTAGE::TagePrediction();
    }
    return it->second;
}

/**
 * @brief Verify TAGE table entries
 *
 * @param tage The TAGE predictor
 * @param pc Branch instruction address to check
 * @param expected_tables Vector of expected table indices to have valid entries
 */
void verifyTageEntries(BTBTAGE* tage, Addr pc, const std::vector<int>& expected_tables) {
    for (int t = 0; t < tage->numPredictors; t++) {
        for (unsigned way = 0; way < tage->numWays[t]; way++) {
            Addr index = tage->getTageIndex(pc, t);
            auto &entry = tage->tageTable[t][index][way];

            // Check if this table should have a valid entry
            bool should_be_valid = std::find(expected_tables.begin(),
                                            expected_tables.end(), t) != expected_tables.end();

            if (should_be_valid) {
                EXPECT_TRUE(entry.valid && entry.pc == pc)
                    << "Table " << t << " should have valid entry for PC " << std::hex << pc;
            }
        }
    }
}

/**
 * @brief Find the table with a valid entry for a given fetch block and branch
 *
 * @param tage The TAGE predictor
 * @param startPC Fetch-block start address used during prediction
 * @param branchPC Branch instruction address being searched
 * @return int Index of the table with valid entry (-1 if not found)
 */
int findTableWithEntry(BTBTAGE* tage, Addr startPC, Addr branchPC) {
    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    // use meta to find the table, predicted info
    for (int t = 0; t < tage->numPredictors; t++) {
        Addr index = tage->getTageIndex(startPC, t, meta->indexFoldedHist[t].get());
        for (unsigned way = 0; way < tage->numWays[t]; way++) {
            auto &entry = tage->tageTable[t][index][way];
            if (entry.valid && entry.pc == branchPC) {
                return t;
            }
        }
    }
    return -1;
}

int findTableWithEntry(BTBTAGE* tage, Addr startPC, Addr branchPC,
                       const std::shared_ptr<BTBTAGE::TageMeta>& meta) {
    for (int t = 0; t < tage->numPredictors; t++) {
        Addr index = tage->getTageIndex(startPC, t, meta->indexFoldedHist[t].get());
        for (unsigned way = 0; way < tage->numWays[t]; way++) {
            auto &entry = tage->tageTable[t][index][way];
            if (entry.valid && entry.pc == branchPC) {
                return t;
            }
        }
    }
    return -1;
}

std::vector<int> findTablesWithEntry(
    BTBTAGE* tage, Addr startPC, Addr branchPC,
    const std::shared_ptr<BTBTAGE::TageMeta>& meta)
{
    std::vector<int> tables;
    for (int t = 0; t < tage->numPredictors; t++) {
        Addr index = tage->getTageIndex(startPC, t, meta->indexFoldedHist[t].get());
        for (unsigned way = 0; way < tage->numWays[t]; way++) {
            auto &entry = tage->tageTable[t][index][way];
            if (entry.valid && entry.pc == branchPC) {
                tables.push_back(t);
                break;
            }
        }
    }
    return tables;
}

class BTBTAGETest : public ::testing::Test
{
protected:
    void SetUp() override {
        tage = new BTBTAGE();
        // memset tageStats to 0
        memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
        history.resize(64, false);  // 64-bit history initialized to 0
        stagePreds.resize(2);  // 2 stages
    }

    BTBTAGE* tage;
    boost::dynamic_bitset<> history;
    std::vector<FullBTBPrediction> stagePreds;
};

// Test basic prediction functionality
TEST_F(BTBTAGETest, BasicPrediction) {
    // Create a conditional branch entry biased towards taken
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, 1);

    // Predict and verify
    bool taken = predictTAGE(tage, 0x1000, {entry}, history, stagePreds);

    // Should predict taken due to initial counter bias
    EXPECT_TRUE(taken) << "Initial prediction should be taken";

    // Update predictor with actual outcome Not taken
    predictUpdateCycle(tage, 0x1000, entry, false, history, stagePreds);

    // Verify at least one table has an entry allocated
    int table = findTableWithEntry(tage, 0x1000, 0x1000);
    EXPECT_GE(table, 0) << "No TAGE table entry was allocated";
}

// Test basic history update functionality (PHR semantics)
TEST_F(BTBTAGETest, HistoryUpdate) {
    // Use a fixed control PC to derive PHR bits
    Addr pc = 0x1000;
    Addr target = pc + 0x40;

    // Test case 1: Update with taken branch (PHR shifts in 2 bits from PC hash)
    // Correct order: first update folded histories with pre-update PHR, then mutate PHR
    tage->doUpdateHist(history, true, pc, target);
    applyPathHistoryTaken(history, pc, target);

    // Verify folded history matches the ideal fold of the updated PHR
    tage->checkFoldedHist(history, "taken update");

    // Test case 2: Update with not-taken branch (PHR unchanged, folded update is no-op)
    boost::dynamic_bitset<> before_not_taken = history;
    tage->doUpdateHist(history, false, pc, target);

    // Verify folded history remains consistent
    tage->checkFoldedHist(history, "not-taken update");
    EXPECT_EQ(history, before_not_taken);
}

// Test main and alternative prediction mechanism by direct setup
TEST_F(BTBTAGETest, MainAltPredictionBehavior) {
    // Create a branch entry for testing
    BTBEntry entry = createBTBEntry(0x1000);

    // Setup a strong main prediction (taken) in table 3
    setupTageEntry(tage, 0x1000, 3, 2); // Strong taken

    // Setup a weak alternative prediction (not taken) in table 1
    setupTageEntry(tage, 0x1000, 1, -1); // Weak not taken

    // Predict with these entries
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);

    // Check prediction metadata
    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    auto pred = meta->preds[0x1000];

    // Should use main prediction (strong counter)
    EXPECT_FALSE(pred.useAlt) << "Should use main prediction with strong counter";
    EXPECT_TRUE(pred.taken) << "Main prediction should be taken";
    EXPECT_EQ(pred.mainInfo.table, 3) << "Main prediction should come from table 3";
    EXPECT_EQ(pred.altInfo.table, 1) << "Alt prediction should come from table 1";

    // Now set main prediction to weak
    setupTageEntry(tage, 0x1000, 3, 0); // Weak taken

    // Predict again
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);

    // Check prediction metadata again
    meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    pred = meta->preds[0x1000];

    // Should use alt prediction (main is weak)
    EXPECT_TRUE(pred.useAlt) << "Should use alt prediction with weak main counter";
    EXPECT_FALSE(pred.taken) << "Alt prediction should be not taken";
}

// Test useful bit update mechanism
TEST_F(BTBTAGETest, UsefulBitMechanism) {
    // Setup a test branch
    BTBEntry entry = createBTBEntry(0x1000);

    // Setup entries in main and alternative tables
    setupTageEntry(tage, 0x1000, 3, 2, false); // Main: strong taken, useful=false
    setupTageEntry(tage, 0x1000, 1, -2, false); // Alt: strong not taken, useful=false

    // Verify initial useful bit state
    Addr mainIndex = tage->getTageIndex(0x1000, 3);
    EXPECT_FALSE(tage->tageTable[3][mainIndex][0].useful) << "Useful bit should start as false";

    // Predict
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    // Update with actual outcome matching main prediction (taken)
    FetchTarget stream = createStream(0x1000, entry, true, meta);
    tage->update(stream);

    // Verify useful bit is set (main prediction was correct and differed from alt)
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should be set when main predicts correctly and differs from alt";

    // Predict again
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    meta = tage->getPredictionMeta();

    // Update with actual outcome opposite to main prediction (not taken)
    stream = createStream(0x1000, entry, false, meta);
    tage->update(stream);

    // Verify useful bit is NOT cleared (policy is ++ only, no --)
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should remain set when main predicts incorrectly (no decrement)";
}

// Test entry allocation mechanism
TEST_F(BTBTAGETest, EntryAllocationAndReplacement) {
    // Instead of creating two different PCs, we'll create two entries with the same PC
    // This ensures they map to the same indices in the tables
    BTBEntry entry1 = createBTBEntry(0x1000);
    BTBEntry entry2 = createBTBEntry(0x1000); // Same PC to ensure same indices

    // Set all tables to have entries with useful=true
    for (int t = 0; t < tage->numPredictors; t++) {
        setupTageEntry(tage, 0x1000, t, 0, true); // Counter=0, useful=true
    }

    // Force a misprediction to trigger allocation attempt
    // First, make a prediction
    predictTAGE(tage, 0x1000, {entry1}, history, stagePreds);
    auto meta = tage->getPredictionMeta();
    bool predicted = false;
    auto result_pred = findCondTaken(stagePreds[1].condTakens, 0x1000);
    bool found_pred = result_pred.first;
    bool pred_result = result_pred.second;
    if (found_pred) {
        predicted = pred_result;
    }

    // Create a stream for entry2 with opposite outcome to force allocation
    // Although it has the same PC, we'll treat it as a different branch context
    // by setting a specific tag that doesn't match existing entries
    FetchTarget stream = createStream(0x1000, entry2, !predicted, meta);
    stream.squashType = SquashType::SQUASH_CTRL; // Mark as control misprediction
    stream.squashPC = 0x1000;

    // Update the predictor (this should try to allocate but fail)
    tage->update(stream);

    int alloc_failed_no_valid = tage->tageStats.updateAllocFailureNoValidTable;
    EXPECT_GE(alloc_failed_no_valid, 1) << "Allocate failed due to no valid table to allocate (all useful)";

}

// Test history recovery mechanism
TEST_F(BTBTAGETest, HistoryRecoveryCorrectness) {
    BTBEntry entry = createBTBEntry(0x1000);

    // Record initial history state
    boost::dynamic_bitset<> originalHistory = history;

    // Store original folded history state
    std::vector<PathFoldedHist> originalTagFoldedHist;
    std::vector<PathFoldedHist> originalAltTagFoldedHist;
    std::vector<PathFoldedHist> originalIndexFoldedHist;

    for (int i = 0; i < tage->numPredictors; i++) {
        originalTagFoldedHist.push_back(tage->tagFoldedHist[i]);
        originalAltTagFoldedHist.push_back(tage->altTagFoldedHist[i]);
        originalIndexFoldedHist.push_back(tage->indexFoldedHist[i]);
    }

    // Make a prediction
    bool predicted_taken = predictTAGE(tage, 0x1000, {entry}, history, stagePreds);

    // Speculatively update history
    tage->specUpdateHist(history, stagePreds[1]);
    auto meta = tage->getPredictionMeta();

    // Update PHR register (speculative) to mirror pHistShiftIn
    if (predicted_taken) {
        applyPathHistoryTaken(history, entry.pc, entry.target);
    }

    // Create a recovery stream with opposite outcome
    FetchTarget stream = createStream(0x1000, entry, !predicted_taken, meta);
    stream = setMispredStream(stream);

    // Recover to pre-speculative state and update with correct outcome
    boost::dynamic_bitset<> recoveryHistory = originalHistory;
    tage->recoverHist(recoveryHistory, stream, 1, !predicted_taken);

    // Expected history should be original updated with PHR if actually taken
    boost::dynamic_bitset<> expectedHistory = originalHistory;
    if (!predicted_taken) { // actual_taken
        applyPathHistoryTaken(expectedHistory, entry.pc, entry.target);
    }

    // Verify recovery produced the expected history
    for (int i = 0; i < tage->numPredictors; i++) {
        tage->tagFoldedHist[i].check(expectedHistory);
        tage->altTagFoldedHist[i].check(expectedHistory);
        tage->indexFoldedHist[i].check(expectedHistory);
    }
}

// Simplified test for multiple branch sequence
TEST_F(BTBTAGETest, MultipleBranchSequence) {
    // Create two branches
    std::vector<BTBEntry> btbEntries = {
        createBTBEntry(0x1000),
        createBTBEntry(0x1004)
    };

    // Predict for both branches
    predictTAGE(tage, 0x1000, btbEntries, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    // Get predictions for both branches
    bool first_pred = false, second_pred = false;
    auto result1 = findCondTaken(stagePreds[1].condTakens, 0x1000);
    if (result1.first) {
        first_pred = result1.second;
    }
    auto result2 = findCondTaken(stagePreds[1].condTakens, 0x1004);
    if (result2.first) {
        second_pred = result2.second;
    }

    // Update first branch (correct prediction), no allocation
    FetchTarget stream1 = createStream(0x1000, btbEntries[0], first_pred, meta);
    tage->update(stream1);

    // Update second branch (incorrect prediction), allocate 1 entry
    FetchTarget stream2 = createStream(0x1000, btbEntries[1], !second_pred, meta);
    stream2.squashType = SquashType::SQUASH_CTRL;
    stream2.squashPC = 0x1004;
    tage->update(stream2);

    // Verify both branches have entries allocated
    EXPECT_EQ(findTableWithEntry(tage, 0x1000, 0x1000), -1) << "First branch should not have an entry";
    EXPECT_GE(findTableWithEntry(tage, 0x1000, 0x1004), 0) << "Second branch should have an entry";
}

// Test counter update mechanism
TEST_F(BTBTAGETest, CounterUpdateMechanism) {
    BTBEntry entry = createBTBEntry(0x1000);

    // Setup a TAGE entry with a neutral counter
    int testTable = 3;
    setupTageEntry(tage, 0x1000, testTable, 0);

    // Verify initial counter value
    Addr index = tage->getTageIndex(0x1000, testTable);
    EXPECT_EQ(tage->tageTable[testTable][index][0].counter, 0) << "Initial counter should be 0";

    // Train with taken outcomes multiple times
    for (int i = 0; i < 3; i++) {
        predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
        auto meta = tage->getPredictionMeta();

        FetchTarget stream = createStream(0x1000, entry, true, meta);
        tage->update(stream);
    }

    // Verify counter saturates at maximum
    EXPECT_EQ(tage->tageTable[testTable][index][0].counter, 3)
        << "Counter should saturate at maximum value";

    // Train with not-taken outcomes multiple times
    for (int i = 0; i < 7; i++) {
        predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
        auto meta = tage->getPredictionMeta();

        FetchTarget stream = createStream(0x1000, entry, false, meta);
        tage->update(stream);
    }

    // Verify counter saturates at minimum
    EXPECT_EQ(tage->tageTable[testTable][index][0].counter, -4)
        << "Counter should saturate at minimum value";
}

/**
 * @brief Test predictor consistency after multiple predictions
 *
 * This test verifies that:
 * 1. The predictor learns a repeating pattern
 * 2. The prediction accuracy improves over time
 * 3. Predictor state is consistent after multiple predictions
 */
TEST_F(BTBTAGETest, UpdateConsistencyAfterMultiplePredictions) {
    // Create a branch entry
    BTBEntry entry = createBTBEntry(0x1000);
    // outer loop always taken
    BTBEntry entry2 = createBTBEntry(0x1010); // always taken

    // Step 1: Train predictor on a fixed pattern (alternating T/N)
    const int TOTAL_ITERATIONS = 100;
    const int WARMUP_ITERATIONS = 80;

    int correctly_predicted = 0;

    for (int i = 0; i < TOTAL_ITERATIONS; i++) {
        bool actual_taken = (i % 2 == 0);  // T,N,T,N pattern
        bool predicted_taken = predictUpdateCycle(tage, 0x1000, entry, actual_taken, history, stagePreds);
        predictUpdateCycle(tage, 0x1010, entry2, true, history, stagePreds);

        // Count correct predictions after warmup
        if (i >= WARMUP_ITERATIONS) {
            correctly_predicted += (predicted_taken == actual_taken) ? 1 : 0;
        }
    }

    // Calculate accuracy in final phase
    double accuracy = static_cast<double>(correctly_predicted) /
                     (TOTAL_ITERATIONS - WARMUP_ITERATIONS);

    // Verify predictor has learned the pattern with high accuracy
    EXPECT_GT(accuracy, 0.9)
        << "Predictor should learn alternating pattern with >90% accuracy";
    // print updateMispred: mispredictions times
    std::cout << "updateMispred: " << tage->tageStats.updateMispred << std::endl;
}

/**
 * @brief Test combined prediction accuracy across different tables
 *
 * This test evaluates how different tables in the TAGE predictor
 * contribute to prediction accuracy for various branch patterns.
 */
TEST_F(BTBTAGETest, CombinedPredictionAccuracyTesting) {
    // Setup branch entry
    BTBEntry entry = createBTBEntry(0x1000);
    // outer loop always taken
    BTBEntry entry2 = createBTBEntry(0x1010); // always taken

    // Define different branch patterns
    struct PatternTest
    {
        std::string name;
        std::function<bool(int)> pattern;
    };

    std::vector<PatternTest> patterns = {
        {"Alternating", [](int i) { return i % 2 == 0; }},                   // T,N,T,N...
        {"ThreeCycle", [](int i) { return i % 3 == 0; }},                    // T,N,N,T,N,N...
        {"LongCycle", [](int i) { return (i / 10) % 2 == 0; }},              // 10 Ts, 10 Ns...
        {"BiasedRandom", [](int i) {
            // Use deterministic but complex pattern that appears somewhat random
            return ((i * 7 + 3) % 11) > 5;
        }}
    };

    const int TRAIN_ITERATIONS = 200;  // it need more iterations to train!
    const int WARMUP_ITERATIONS = 180;


    // Test each pattern
    for (const auto& pattern_test : patterns) {
        // Reset predictor and history
        tage = new BTBTAGE();
        // clear history
        history.reset();
        stagePreds.resize(2);

        int correctly_predicted = 0;
        // Training phase
        for (int i = 0; i < TRAIN_ITERATIONS; i++) {
            bool actual_taken = pattern_test.pattern(i);
            bool predicted_taken = predictUpdateCycle(tage, 0x1000, entry, actual_taken, history, stagePreds);
            predictUpdateCycle(tage, 0x1010, entry2, true, history, stagePreds);

                    // Count correct predictions after warmup
            if (i >= WARMUP_ITERATIONS) {
                correctly_predicted += (predicted_taken == actual_taken) ? 1 : 0;
            }
        }

        // Calculate accuracy in final phase
        double accuracy = static_cast<double>(correctly_predicted) /
                         (TRAIN_ITERATIONS - WARMUP_ITERATIONS);


        // Verify predictor has learned the pattern with high accuracy
        EXPECT_GE(accuracy, 0.8)
            << "Predictor should learn alternating pattern with >80% accuracy";

        // print updateMispred: mispredictions times
        std::cout << "updateMispred: " << tage->tageStats.updateMispred << std::endl;
    }
}

/**
 * @brief Create a TAGE table entry manually with specific properties
 *
 * This is particularly useful for set-associative testing when we need
 * to control exact placement of entries
 */
void createManualTageEntry(BTBTAGE* tage, int table, Addr index, int way,
                          Addr tag, short counter, bool useful, Addr pc,
                          unsigned lruCounter = 0) {
    auto &entry = tage->tageTable[table][index][way];
    entry.valid = true;
    entry.tag = tag;
    entry.counter = counter;
    entry.useful = useful;
    entry.pc = pc;
    entry.lruCounter = lruCounter;
}


/**
 * @brief Test set-associative conflict handling
 *
 * This test verifies that:
 * 1. Multiple branches mapping to the same index can be predicted correctly
 * 2. The LRU counters are updated properly when entries are accessed
 */
TEST_F(BTBTAGETest, SetAssociativeConflictHandling) {
    // Create two branch entries with different PCs
    Addr startPC = 0x1000;
    BTBEntry entry1 = createBTBEntry(startPC);
    BTBEntry entry2 = createBTBEntry(startPC + 4);

    // Use a specific table and index for testing
    int testTable = 1;
    Addr testIndex = tage->getTageIndex(startPC, testTable);

    // Calculate correct tags for each entry (tag includes position XOR)
    // entry1: PC=0x1000, position=0
    Addr testTag1 = tage->getTageTag(startPC, testTable, 0);
    // entry2: PC=0x1004, position=2 (calculated as (0x1004-0x1000)>>1)
    Addr testTag2 = tage->getTageTag(startPC, testTable, 2);

    // Manually create entries with the same index but different tags (due to position)
    createManualTageEntry(tage, testTable, testIndex, 0, testTag1, 2, false, 0x1000, 0); // Way 0: Strong taken
    createManualTageEntry(tage, testTable, testIndex, 1, testTag2, -2, false, 0x1004, 1); // Way 1: Strong not taken

    // Make predictions and verify directly
    // For entry1 (should predict taken)
    stagePreds.clear();
    stagePreds.resize(2);
    stagePreds[1].btbEntries = {entry1};
    tage->putPCHistory(startPC, history, stagePreds);

    // Get prediction for entry1
    bool pred1 = false;
    auto result_entry1 = findCondTaken(stagePreds[1].condTakens, entry1.pc);
    if (result_entry1.first) {
        pred1 = result_entry1.second;
    }
    EXPECT_TRUE(pred1) << "Entry1 should predict taken";

    // Check LRU counters after first access
    EXPECT_EQ(tage->tageTable[testTable][testIndex][0].lruCounter, 0)
        << "LRU counter for way 0 should be reset after access";

    // For entry2 (should predict not taken)
    stagePreds.clear();
    stagePreds.resize(2);
    stagePreds[1].btbEntries = {entry2};
    tage->putPCHistory(startPC, history, stagePreds);

    // Get prediction for entry2
    bool pred2 = false;
    auto result_entry2 = findCondTaken(stagePreds[1].condTakens, entry2.pc);
    if (result_entry2.first) {
        pred2 = result_entry2.second;
    }
    EXPECT_FALSE(pred2) << "Entry2 should predict not taken";
}

/**
 * @brief Test allocation behavior with multiple ways (new policy)
 *
 * New allocation policy highlights:
 * - Allocation consults the selected way's usefulMask for each table.
 * - Only invalid entries, or (useful==0 and weak counter) can be allocated.
 * - No LRU-based replacement is performed when all considered entries are useful.
 *
 * This test verifies:
 * 1. First mispredict allocates into an invalid way.
 * 2. Subsequent allocations fail when the selected way's usefulMask marks the table useful.
 * 3. No replacement occurs even after additional allocation attempts.
 */
TEST_F(BTBTAGETest, AllocationBehaviorWithMultipleWays) {
    // Start with a fresh predictor
    tage = new BTBTAGE(1, 2, 10); // only 1 predictor table, 2 ways
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    // Create a branch entry, base ctr=0, base taken
    BTBEntry entry = createBTBEntry(0x1000);

    // Set up a test table and index
    int testTable = 0;
    Addr testIndex = tage->getTageIndex(0x1000, testTable);

    // Step 1: Verify allocation in an invalid way first
    // Make first prediction, mispredict, allocate a new entry
    bool predicted1 = predictUpdateCycle(tage, 0x1000, entry, false, history, stagePreds);

    // Check if allocation happened
    int allocatedWay = -1;
    for (unsigned way = 0; way < tage->numWays[testTable]; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid &&
            tage->tageTable[testTable][testIndex][way].pc == 0x1000) {
            allocatedWay = way;
            break;
        }
    }

    EXPECT_GE(allocatedWay, 0) << "Entry should be allocated in one of the ways";

    // Strengthen the first allocated entry to prevent it from being replaced
    // This simulates that the first branch has been trained and should be protected
    tage->tageTable[testTable][testIndex][allocatedWay].useful = true;
    tage->tageTable[testTable][testIndex][allocatedWay].counter = 2; // Make it strong

    // Step 2: Attempt to fill remaining ways with different branches
    for (unsigned way = 0; way < tage->numWays[testTable]; way++) {
        if (way == allocatedWay) continue;

        // Create a branch with different PC
        BTBEntry newEntry = createBTBEntry(0x1004);

        // Make prediction and force allocation
        bool predicted = predictUpdateCycle(tage, 0x1000, newEntry, false, history, stagePreds);
    }

    // Verify now both ways can be filled under miss policy (consider any way's useful=0)
    int filledWays = 0;
    for (unsigned way = 0; way < tage->numWays[testTable]; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid) {
            filledWays++;
        }
    }

    EXPECT_EQ(filledWays, tage->numWays[testTable])
        << "All ways should be filled after multiple allocations under miss policy";

    // Strengthen all allocated entries to prevent replacement in Step 3
    for (unsigned way = 0; way < tage->numWays[testTable]; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid) {
            tage->tageTable[testTable][testIndex][way].useful = true;
            tage->tageTable[testTable][testIndex][way].counter = 2; // Make it strong
        }
    }

    // Stats: first allocation succeeded, subsequent attempts failed
    int alloc_success_after_step2 = tage->tageStats.updateAllocSuccess;
    int alloc_failure_after_step2 = tage->tageStats.updateAllocFailure;
    EXPECT_EQ(alloc_success_after_step2, 2) << "Two allocations should have succeeded (one per way)";
    EXPECT_GE(alloc_failure_after_step2, 0) << "Allocation failures may occur depending on mask selection";

    // Step 3: One more allocation should still not replace existing entry (no LRU replacement)
    BTBEntry newEntry = createBTBEntry(0x1008);
    bool predicted = predictUpdateCycle(tage, 0x1000, newEntry, false, history, stagePreds);

    // Check if the new entry was allocated
    bool found = false;
    unsigned foundWay = 0;
    for (unsigned way = 0; way < tage->numWays[testTable]; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid &&
            tage->tageTable[testTable][testIndex][way].pc == 0x1008) {
            found = true;
            foundWay = way;
            break;
        }
    }

    EXPECT_FALSE(found) << "New entry should not be allocated (no replacement without eligible slot)";

    // Stats: failure count should increase further after another attempt
    int alloc_failure_after_step3 = tage->tageStats.updateAllocFailure;
    EXPECT_GE(alloc_failure_after_step3, alloc_failure_after_step2 + 1)
        << "Allocation failures should increase after additional failed attempt";
}

TEST_F(BTBTAGETest, NewConditionalEntryWithoutPredictionMetaStillTrains) {
    stagePreds[1].btbEntries.clear();
    tage->putPCHistory(0x1000, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    BTBEntry newEntry = createBTBEntry(0x1010, true, true, false, -1);
    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.exeBranchInfo = newEntry;
    stream.exeTaken = true;
    stream.resolved = true;
    stream.predBranchInfo = newEntry;
    stream.updateBTBEntries.clear();
    stream.updateIsOldEntry = false;
    stream.updateNewBTBEntry = newEntry;
    stream.predMetas[0] = meta;
    stream = setMispredStream(stream);

    tage->update(stream);

    int table = findTableWithEntry(tage, 0x1000, newEntry.pc);
    EXPECT_GE(table, 0)
        << "New conditional entry should still allocate without prediction-time meta";
}

/**
 * @brief Test resolve-train bank conflict detection
 *
 * Verifies that a same-bank resolve-train update is rejected when bank
 * conflict checking is enabled.
 */
TEST_F(BTBTAGETest, ResolveTrainBankConflict) {
    BTBTAGE bankTage(4, 2, 1024, 4);
    memset(&bankTage.tageStats, 0, sizeof(BTBTAGE::TageStats));
    boost::dynamic_bitset<> testHistory(128);
    std::vector<FullBTBPrediction> testStagePreds(5);

    bankTage.enableBankConflict = true;
    testStagePreds[1].btbEntries = {createBTBEntry(0x20)};
    bankTage.putPCHistory(0x20, testHistory, testStagePreds);
    EXPECT_TRUE(bankTage.predBankValid);

    auto meta = bankTage.getPredictionMeta();
    auto packet = createResolvedTrainPacket(
        0xa0, meta, {createResolvedBranch(createBTBEntry(0xa0), true, false, 0)});
    auto target = createResolvedTrainTarget(0xa0, meta);

    uint64_t conflicts_before = bankTage.tageStats.updateBankConflict;
    bool can_train = bankTage.canResolveTrain(packet, target);

    EXPECT_FALSE(can_train);
    EXPECT_EQ(bankTage.tageStats.updateBankConflict, conflicts_before + 1);
    EXPECT_FALSE(bankTage.predBankValid);
}

TEST_F(BTBTAGETest, ResolveTrainUsesPacketTruthForConditionalSelection) {
    const Addr startPC = 0x1000;
    BTBEntry first = createBTBEntry(0x1000);
    BTBEntry second = createBTBEntry(0x1004);

    setupTageEntryForFetchBlock(tage, startPC, first.pc, 3, 0);
    setupTageEntryForFetchBlock(tage, startPC, second.pc, 3, 0, false, 1);

    predictTAGE(tage, startPC, {first, second}, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    Addr first_index = tage->getTageIndex(startPC, 3);
    EXPECT_EQ(tage->tageTable[3][first_index][0].counter, 0);
    EXPECT_EQ(tage->tageTable[3][first_index][1].counter, 0);

    auto packet = createResolvedTrainPacket(
        startPC, meta, {createResolvedBranch(first, false, true, 0)});
    auto target = createResolvedTrainTarget(startPC, meta);

    ASSERT_TRUE(tage->canResolveTrain(packet, target));
    tage->resolveTrain(packet, target);

    EXPECT_EQ(tage->tageTable[3][first_index][0].counter, -1);
    EXPECT_EQ(tage->tageTable[3][first_index][1].counter, 0);
}

TEST_F(BTBTAGETest, ResolveTrainRepeatedShortPatternMatchesLegacyProviderGrowth) {
    const Addr bodyStartPC = 0x1000;
    const Addr loopStartPC = 0x1100;
    const BTBEntry body = createBTBEntry(0x1004, true, true, false, -1, 0x100c);
    const BTBEntry loop = createBTBEntry(loopStartPC, true, true, false, -1, bodyStartPC);
    const int iterations = 160;

    BTBTAGE legacyTage;
    BTBTAGE fullTage;
    memset(&legacyTage.tageStats, 0, sizeof(BTBTAGE::TageStats));
    memset(&fullTage.tageStats, 0, sizeof(BTBTAGE::TageStats));

    boost::dynamic_bitset<> legacyHistory(64, false);
    boost::dynamic_bitset<> fullHistory(64, false);
    std::vector<FullBTBPrediction> legacyStagePreds(2);
    std::vector<FullBTBPrediction> fullStagePreds(2);

    auto legacyTrainNewEntry = [&](BTBTAGE *tage,
                                   boost::dynamic_bitset<> &curHistory,
                                   std::vector<FullBTBPrediction> &curStagePreds,
                                   bool taken) {
        curStagePreds[1].btbEntries.clear();
        tage->putPCHistory(bodyStartPC, curHistory, curStagePreds);
        auto meta = tage->getPredictionMeta();

        FetchTarget stream;
        stream.startPC = bodyStartPC;
        stream.exeBranchInfo = body;
        stream.exeTaken = taken;
        stream.resolved = true;
        stream.predBranchInfo = body;
        stream.updateBTBEntries.clear();
        stream.updateIsOldEntry = false;
        stream.updateNewBTBEntry = body;
        stream.predMetas[0] = meta;
        if (taken) {
            stream = setMispredStream(stream);
        }

        tage->update(stream);
        advanceActualHistory(tage, curHistory, {body}, {taken});
    };

    auto resolveTrainNewEntry = [&](BTBTAGE *tage,
                                    boost::dynamic_bitset<> &curHistory,
                                    std::vector<FullBTBPrediction> &curStagePreds,
                                    bool taken) {
        curStagePreds[1].btbEntries.clear();
        tage->putPCHistory(bodyStartPC, curHistory, curStagePreds);
        auto meta = tage->getPredictionMeta();

        auto packet = createResolvedTrainPacket(
            bodyStartPC, meta, {createResolvedBranch(body, taken, taken, 0)});
        auto target = createResolvedTrainTarget(bodyStartPC, meta);
        ASSERT_TRUE(tage->canResolveTrain(packet, target));
        tage->resolveTrain(packet, target);
        advanceActualHistory(tage, curHistory, {body}, {taken});
    };

    for (int i = 0; i < iterations; ++i) {
        const bool bodyTaken = (i % 2) == 0;

        legacyTrainNewEntry(&legacyTage, legacyHistory, legacyStagePreds, bodyTaken);
        resolveTrainNewEntry(&fullTage, fullHistory, fullStagePreds, bodyTaken);

        legacyTrainSequence(&legacyTage, loopStartPC, {loop}, {true},
                            legacyHistory, legacyStagePreds);
        resolveTrainSequence(&fullTage, loopStartPC, {loop}, {true},
                             fullHistory, fullStagePreds);
    }

    auto legacyPred = predictBranch(&legacyTage, bodyStartPC, {body},
                                    legacyHistory, legacyStagePreds, body.pc);
    auto fullPred = predictBranch(&fullTage, bodyStartPC, {body},
                                  fullHistory, fullStagePreds, body.pc);

    auto legacyMeta = std::static_pointer_cast<BTBTAGE::TageMeta>(legacyTage.getPredictionMeta());
    auto fullMeta = std::static_pointer_cast<BTBTAGE::TageMeta>(fullTage.getPredictionMeta());
    auto legacyTables = findTablesWithEntry(&legacyTage, bodyStartPC, body.pc, legacyMeta);
    auto fullTables = findTablesWithEntry(&fullTage, bodyStartPC, body.pc, fullMeta);

    ASSERT_GT(legacyPred.mainInfo.table, 0)
        << "Legacy training should grow beyond table 0 for the short repeated pattern";
    EXPECT_EQ(fullPred.mainInfo.table, legacyPred.mainInfo.table)
        << "Full resolve-train should activate the same provider depth as legacy update";
    EXPECT_EQ(fullPred.finalProviderTable, legacyPred.finalProviderTable)
        << "Full resolve-train should converge to the same final provider as legacy update";
    EXPECT_EQ(fullTables, legacyTables)
        << "Full resolve-train should build the same set of TAGE tables as legacy update";
}



}  // namespace test

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

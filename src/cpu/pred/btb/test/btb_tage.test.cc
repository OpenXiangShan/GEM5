#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>

#include "base/types.hh"
#include "cpu/pred/btb/btb_tage.hh"
#include "cpu/pred/btb/btb_tage_ub.hh"
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
std::pair<bool, bool> findCondTaken(const gem5::branch_prediction::btb_pred::CondTakens& condTakens, Addr pc) {
    auto it = CondTakens_find(condTakens, pc);
    if (it != condTakens.end()) {
        return {true, it->second};
    }
    return {false, false};
}

void syncLegacyMirrorForTest(BTBTAGE::TageEntry &entry, Addr pc)
{
    entry.counter = entry.slots[0].valid ? entry.slots[0].counter : 0;
    entry.useful = entry.slots[0].valid ? entry.slots[0].useful : false;
    entry.pc = pc;
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

void forceMispredPredictUpdateCycle(BTBTAGE* tage, Addr startPC,
                                    const BTBEntry& entry,
                                    boost::dynamic_bitset<>& history,
                                    std::vector<FullBTBPrediction>& stagePreds) {
    stagePreds[1].btbEntries = {entry};
    tage->putPCHistory(startPC, history, stagePreds);

    bool predicted_taken = false;
    auto result = findCondTaken(stagePreds[1].condTakens, entry.pc);
    if (result.first) {
        predicted_taken = result.second;
    }

    predictUpdateCycle(tage, startPC, entry, !predicted_taken, history, stagePreds);
}

void forceAllocationUpdateCycle(BTBTAGE* tage, Addr startPC,
                                const BTBEntry& entry,
                                bool actual_taken,
                                boost::dynamic_bitset<>& history,
                                std::vector<FullBTBPrediction>& stagePreds) {
    stagePreds[1].btbEntries = {entry};
    tage->putPCHistory(startPC, history, stagePreds);
    auto meta = tage->getPredictionMeta();
    FetchTarget stream = createStream(startPC, entry, actual_taken, meta);
    stream = setMispredStream(stream);
    tage->update(stream);
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
                    short counter, bool useful = false, int way = 0,
                    Addr startPC = 0, unsigned slot = 0, int position = -1) {
    if (startPC == 0) {
        const Addr blockMask = (Addr(1) << tage->blockWidth) - 1;
        startPC = pc & ~blockMask;
    }
    if (position < 0) {
        position = static_cast<int>(tage->getBranchIndexInBlock(pc, startPC));
    }

    Addr index = tage->getTageIndex(startPC, table_idx);
    Addr tag = tage->getTageTag(startPC, table_idx);

    auto& entry = tage->tageTable[table_idx][index][way];
    entry.valid = true;
    entry.tag = tag;
    entry.ownerBlockBase = startPC;
    for (auto &slot_ref : entry.slots) {
        slot_ref = BTBTAGE::TageSlot();
    }
    entry.slots[slot] = BTBTAGE::TageSlot(true, position, counter, useful);

    // Keep legacy mirrors only for transition compatibility.
    syncLegacyMirrorForTest(entry, pc);
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
        const Addr blockMask = (Addr(1) << tage->blockWidth) - 1;
        Addr startPC = pc & ~blockMask;
        Addr index = tage->getTageIndex(startPC, t);
        Addr tag = tage->getTageTag(startPC, t);
        unsigned position = tage->getBranchIndexInBlock(pc, startPC);
        bool has_slot_match = false;

        for (unsigned way = 0; way < tage->numWays[t]; way++) {
            auto &entry = tage->tageTable[t][index][way];
            if (!entry.valid || entry.tag != tag) {
                continue;
            }
            for (const auto &slot : entry.slots) {
                if (slot.valid && slot.position == position) {
                    has_slot_match = true;
                    break;
                }
            }
        }

        bool should_be_valid = std::find(expected_tables.begin(),
                                        expected_tables.end(), t) != expected_tables.end();
        if (should_be_valid) {
            EXPECT_TRUE(has_slot_match)
                << "Table " << t << " should have a valid slot for PC " << std::hex << pc;
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
    if (!meta) {
        return -1;
    }

    // use meta to find the table, predicted info
    const unsigned position = tage->getBranchIndexInBlock(branchPC, startPC);
    for (int t = 0; t < tage->numPredictors; t++) {
        Addr index = tage->getTageIndex(startPC, t, meta->indexFoldedHist[t].get());
        Addr tag = tage->getTageTag(startPC, t, meta->tagFoldedHist[t].get(),
                                    meta->altTagFoldedHist[t].get());
        for (unsigned way = 0; way < tage->numWays[t]; way++) {
            auto &entry = tage->tageTable[t][index][way];
            if (!entry.valid || entry.tag != tag) {
                continue;
            }
            for (const auto &slot : entry.slots) {
                if (slot.valid && slot.position == position) {
                    return t;
                }
            }
        }
    }
    return -1;
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
    EXPECT_FALSE(tage->tageTable[3][mainIndex][0].slots[0].useful)
        << "Useful bit should start as false";

    // Predict
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    // Update with actual outcome matching main prediction (taken)
    FetchTarget stream = createStream(0x1000, entry, true, meta);
    tage->update(stream);

    // Verify useful bit is set (main prediction was correct and differed from alt)
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].slots[0].useful)
        << "Useful bit should be set when main predicts correctly and differs from alt";

    // Predict again
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    meta = tage->getPredictionMeta();

    // Update with actual outcome opposite to main prediction (not taken)
    stream = createStream(0x1000, entry, false, meta);
    tage->update(stream);

    // Verify useful bit is NOT cleared (policy is ++ only, no --)
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].slots[0].useful)
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
    EXPECT_EQ(tage->tageTable[testTable][index][0].slots[0].counter, 0)
        << "Initial counter should be 0";

    // Train with taken outcomes multiple times
    for (int i = 0; i < 3; i++) {
        predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
        auto meta = tage->getPredictionMeta();

        FetchTarget stream = createStream(0x1000, entry, true, meta);
        tage->update(stream);
    }

    // Verify counter saturates at maximum
    EXPECT_EQ(tage->tageTable[testTable][index][0].slots[0].counter, 3)
        << "Counter should saturate at maximum value";

    // Train with not-taken outcomes multiple times
    for (int i = 0; i < 7; i++) {
        predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
        auto meta = tage->getPredictionMeta();

        FetchTarget stream = createStream(0x1000, entry, false, meta);
        tage->update(stream);
    }

    // Verify counter saturates at minimum
    EXPECT_EQ(tage->tageTable[testTable][index][0].slots[0].counter, -4)
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
                          unsigned lruCounter = 0, unsigned slot = 0,
                          Addr startPC = 0, bool reset_slots = true) {
    if (startPC == 0) {
        const Addr blockMask = (Addr(1) << tage->blockWidth) - 1;
        startPC = pc & ~blockMask;
    }
    const unsigned position = tage->getBranchIndexInBlock(pc, startPC);

    auto &entry = tage->tageTable[table][index][way];
    if (reset_slots) {
        entry.valid = true;
        entry.tag = tag;
        entry.ownerBlockBase = startPC;
        for (auto &slot_ref : entry.slots) {
            slot_ref = BTBTAGE::TageSlot();
        }
    }
    entry.slots[slot] = BTBTAGE::TageSlot(true, position, counter, useful);

    // Keep legacy mirrors only for transition compatibility.
    syncLegacyMirrorForTest(entry, pc);
    entry.lruCounter = lruCounter;
}

std::vector<Addr> findCollidingBlockStarts(BTBTAGE* tage, Addr baseStartPC,
                                           int table, std::size_t count) {
    std::vector<Addr> blocks;
    const Addr baseIndex = tage->getTageIndex(baseStartPC, table);
    const Addr baseTag = tage->getTageTag(baseStartPC, table);
    const Addr blockSize = Addr(1) << tage->blockWidth;

    for (Addr cand = baseStartPC + blockSize;
         blocks.size() < count && cand < baseStartPC + blockSize * 32768;
         cand += blockSize) {
        if (tage->getTageIndex(cand, table) != baseIndex) {
            continue;
        }

        Addr candTag = tage->getTageTag(cand, table);
        if (candTag == baseTag) {
            continue;
        }

        bool duplicate = false;
        for (Addr existing : blocks) {
            if (tage->getTageTag(existing, table) == candTag) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            blocks.push_back(cand);
        }
    }

    return blocks;
}

unsigned countWaysWithTag(BTBTAGE* tage, int table, Addr index, Addr tag)
{
    unsigned matches = 0;
    for (unsigned way = 0; way < tage->numWays[table]; ++way) {
        const auto &entry = tage->tageTable[table][index][way];
        if (entry.valid && entry.tag == tag) {
            ++matches;
        }
    }
    return matches;
}

int findWayWithTagAndPosition(BTBTAGE* tage, int table, Addr index,
                              Addr tag, unsigned position)
{
    for (unsigned way = 0; way < tage->numWays[table]; ++way) {
        const auto &entry = tage->tageTable[table][index][way];
        if (!entry.valid || entry.tag != tag) {
            continue;
        }
        for (const auto &slot : entry.slots) {
            if (slot.valid && slot.position == position) {
                return static_cast<int>(way);
            }
        }
    }
    return -1;
}


/**
 * @brief Test slot-aware lookup in a shared tag entry
 *
 * This test verifies stage-1 lookup semantics:
 * 1. position does not participate in tag generation.
 * 2. Two branches in one block can hit different slots in the same entry.
 */
TEST_F(BTBTAGETest, SetAssociativeConflictHandling) {
    // Create two branch entries with different PCs
    Addr startPC = 0x1000;
    BTBEntry entry1 = createBTBEntry(startPC);
    BTBEntry entry2 = createBTBEntry(startPC + 4);

    // Use a specific table and index for testing
    int testTable = 1;
    Addr testIndex = tage->getTageIndex(startPC, testTable);

    // position no longer affects tag in stage-1.
    Addr testTagFromBase = tage->getTageTag(startPC, testTable);
    Addr testTagFromSecondBranch = tage->getTageTag(entry2.pc, testTable);
    EXPECT_EQ(testTagFromBase, testTagFromSecondBranch)
        << "tag should be block-level and independent from branch position";

    // Build one shared entry with two valid slots (positions 0 and 2).
    createManualTageEntry(tage, testTable, testIndex, 0, testTagFromBase,
                          2, false, entry1.pc, 0, 0, startPC, true);   // slot 0: taken
    createManualTageEntry(tage, testTable, testIndex, 0, testTagFromBase,
                          -2, false, entry2.pc, 0, 1, startPC, false); // slot 1: not-taken

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
    auto meta1 = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    ASSERT_TRUE(meta1->preds.count(entry1.pc));
    EXPECT_EQ(meta1->preds[entry1.pc].mainInfo.way, 0u);
    EXPECT_EQ(meta1->preds[entry1.pc].mainInfo.slot, 0u);

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
    auto meta2 = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    ASSERT_TRUE(meta2->preds.count(entry2.pc));
    EXPECT_EQ(meta2->preds[entry2.pc].mainInfo.way, 0u);
    EXPECT_EQ(meta2->preds[entry2.pc].mainInfo.slot, 1u);
}

TEST_F(BTBTAGETest, SlotAwareSharedEntryLookup) {
    Addr startPC = 0x2000;
    BTBEntry entry1 = createBTBEntry(startPC);
    BTBEntry entry2 = createBTBEntry(startPC + 4);
    int testTable = 2;
    Addr testIndex = tage->getTageIndex(startPC, testTable);

    Addr tag1 = tage->getTageTag(startPC, testTable);
    Addr tag2 = tage->getTageTag(entry2.pc, testTable);
    EXPECT_EQ(tag1, tag2) << "position should not be encoded into tag";

    createManualTageEntry(tage, testTable, testIndex, 0, tag1,
                          2, false, entry1.pc, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, testIndex, 0, tag1,
                          -2, false, entry2.pc, 0, 1, startPC, false);

    stagePreds.clear();
    stagePreds.resize(2);
    stagePreds[1].btbEntries = {entry1, entry2};
    tage->putPCHistory(startPC, history, stagePreds);

    auto result1 = findCondTaken(stagePreds[1].condTakens, entry1.pc);
    auto result2 = findCondTaken(stagePreds[1].condTakens, entry2.pc);
    ASSERT_TRUE(result1.first);
    ASSERT_TRUE(result2.first);
    EXPECT_TRUE(result1.second);
    EXPECT_FALSE(result2.second);

    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    ASSERT_TRUE(meta->preds.count(entry1.pc));
    ASSERT_TRUE(meta->preds.count(entry2.pc));
    EXPECT_EQ(meta->preds[entry1.pc].mainInfo.way, 0u);
    EXPECT_EQ(meta->preds[entry2.pc].mainInfo.way, 0u);
    EXPECT_EQ(meta->preds[entry1.pc].mainInfo.slot, 0u);
    EXPECT_EQ(meta->preds[entry2.pc].mainInfo.slot, 1u);
}

TEST_F(BTBTAGETest, DifferentTagWholeEntryEvictionRequiresAllSlotsUnprotected) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startA = 0x1000;
    auto collidingBlocks = findCollidingBlockStarts(tage, startA, testTable, 2);
    ASSERT_EQ(collidingBlocks.size(), 2u) << "Need two different tags that collide on the same index";
    const Addr startB = collidingBlocks[0];
    const Addr startC = collidingBlocks[1];

    const Addr testIndex = tage->getTageIndex(startA, testTable);
    const Addr tagA = tage->getTageTag(startA, testTable);
    const Addr tagB = tage->getTageTag(startB, testTable);
    const Addr tagC = tage->getTageTag(startC, testTable);

    ASSERT_EQ(testIndex, tage->getTageIndex(startB, testTable));
    ASSERT_EQ(testIndex, tage->getTageIndex(startC, testTable));
    ASSERT_NE(tagA, tagB);
    ASSERT_NE(tagA, tagC);
    ASSERT_NE(tagB, tagC);

    // Way 0: one protected slot + one unprotected slot -> whole-entry eviction forbidden.
    createManualTageEntry(tage, testTable, testIndex, 0, tagA,
                          2, true, startA, 0, 0, startA, true);
    createManualTageEntry(tage, testTable, testIndex, 0, tagA,
                          0, false, startA + 4, 0, 1, startA, false);

    // Way 1: both slots protected -> whole-entry eviction forbidden.
    createManualTageEntry(tage, testTable, testIndex, 1, tagB,
                          2, true, startB, 0, 0, startB, true);
    createManualTageEntry(tage, testTable, testIndex, 1, tagB,
                          -2, true, startB + 4, 0, 1, startB, false);

    BTBEntry newEntry = createBTBEntry(startC);
    predictUpdateCycle(tage, startC, newEntry, false, history, stagePreds);

    EXPECT_EQ(tage->tageTable[testTable][testIndex][0].tag, tagA);
    EXPECT_EQ(tage->tageTable[testTable][testIndex][1].tag, tagB);
    EXPECT_EQ(tage->tageStats.updateAllocSuccess, 0)
        << "No different-tag whole-entry victim should exist while any slot is protected";

    auto &way0 = tage->tageTable[testTable][testIndex][0];
    way0.slots[0].counter = 0;
    way0.slots[0].useful = false;
    way0.slots[1].counter = -1;
    way0.slots[1].useful = false;
    syncLegacyMirrorForTest(way0, startA);

    predictUpdateCycle(tage, startC, newEntry, false, history, stagePreds);

    EXPECT_EQ(tage->tageTable[testTable][testIndex][0].tag, tagC)
        << "Whole-entry eviction should occur once both slots become unprotected";
    EXPECT_EQ(tage->tageTable[testTable][testIndex][1].tag, tagB)
        << "Protected entries in other ways must remain untouched";
    EXPECT_TRUE(tage->tageTable[testTable][testIndex][0].slots[0].valid);
    EXPECT_EQ(tage->tageTable[testTable][testIndex][0].slots[0].position, 0u);
    EXPECT_FALSE(tage->tageTable[testTable][testIndex][0].slots[1].valid);
    EXPECT_GE(tage->tageStats.updateAllocSuccess, 1)
        << "Evicting an unprotected whole entry should count as a successful allocation";
}

TEST_F(BTBTAGETest, DifferentTagUseInvalidWayRequiresExistingDifferentTagEntry) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startA = 0x1000;
    auto collidingBlocks = findCollidingBlockStarts(tage, startA, testTable, 1);
    ASSERT_EQ(collidingBlocks.size(), 1u);
    const Addr startB = collidingBlocks[0];
    const Addr testIndex = tage->getTageIndex(startA, testTable);
    const Addr tagA = tage->getTageTag(startA, testTable);
    const Addr tagB = tage->getTageTag(startB, testTable);
    ASSERT_EQ(testIndex, tage->getTageIndex(startB, testTable));
    ASSERT_NE(tagA, tagB);

    BTBEntry entryA = createBTBEntry(startA);
    predictUpdateCycle(tage, startA, entryA, false, history, stagePreds);

    EXPECT_EQ(tage->tageStats.allocDifferentTagUseInvalidWay, 0)
        << "Filling an empty set should not count as reusing an invalid way next to an existing different-tag entry";

    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.reset();
    stagePreds.assign(2, FullBTBPrediction{});

    createManualTageEntry(tage, testTable, testIndex, 0, tagA,
                          2, true, startA, 0, 0, startA, true);

    BTBEntry entryB = createBTBEntry(startB);
    predictUpdateCycle(tage, startB, entryB, false, history, stagePreds);

    EXPECT_EQ(tage->tageStats.allocDifferentTagUseInvalidWay, 1)
        << "The counter should increment only once the set already contains another valid different-tag entry";
}

TEST_F(BTBTAGETest, SameTagPositionMissFillsEmptySlotAndSortsByPosition) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x1000;
    const Addr existingPC = startPC + 4;
    const Addr newPC = startPC;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);

    setupTageEntry(tage, existingPC, testTable, 2, false, 0, startPC, 0);

    BTBEntry newEntry = createBTBEntry(newPC);
    forceAllocationUpdateCycle(tage, startPC, newEntry, false, history, stagePreds);

    const auto &entry = tage->tageTable[testTable][index][0];
    ASSERT_TRUE(entry.valid);
    EXPECT_EQ(entry.tag, tag);
    EXPECT_EQ(countWaysWithTag(tage, testTable, index, tag), 1u)
        << "same-tag allocation must reuse the existing entry instead of creating a duplicate";
    ASSERT_TRUE(entry.slots[0].valid);
    ASSERT_TRUE(entry.slots[1].valid);
    EXPECT_EQ(entry.slots[0].position, tage->getBranchIndexInBlock(newPC, startPC));
    EXPECT_EQ(entry.slots[0].counter, -1);
    EXPECT_EQ(entry.slots[1].position, tage->getBranchIndexInBlock(existingPC, startPC));
    EXPECT_EQ(entry.slots[1].counter, 2);
    EXPECT_FALSE(tage->tageTable[testTable][index][1].valid)
        << "same-tag fill should stay in-place and not consume another way";
}

TEST_F(BTBTAGETest, SameTagFullEntryReplacesWeakishNonUsefulSlot) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x2000;
    const Addr keepPC = startPC;
    const Addr replacePC = startPC + 4;
    const Addr newPC = startPC + 8;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);
    auto collidingBlocks = findCollidingBlockStarts(tage, startPC, testTable, 1);
    ASSERT_EQ(collidingBlocks.size(), 1u);
    const Addr otherBlock = collidingBlocks[0];
    const Addr otherTag = tage->getTageTag(otherBlock, testTable);

    createManualTageEntry(tage, testTable, index, 0, tag,
                          2, true, keepPC, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, index, 0, tag,
                          0, false, replacePC, 0, 1, startPC, false);
    createManualTageEntry(tage, testTable, index, 1, otherTag,
                          2, true, otherBlock, 0, 0, otherBlock, true);
    createManualTageEntry(tage, testTable, index, 1, otherTag,
                          -2, true, otherBlock + 4, 0, 1, otherBlock, false);

    BTBEntry newEntry = createBTBEntry(newPC);
    forceMispredPredictUpdateCycle(tage, startPC, newEntry, history, stagePreds);

    const auto &entry = tage->tageTable[testTable][index][0];
    ASSERT_TRUE(entry.valid);
    EXPECT_EQ(countWaysWithTag(tage, testTable, index, tag), 1u);
    ASSERT_TRUE(entry.slots[0].valid);
    ASSERT_TRUE(entry.slots[1].valid);
    EXPECT_EQ(entry.slots[0].position, tage->getBranchIndexInBlock(keepPC, startPC));
    EXPECT_EQ(entry.slots[0].counter, 2);
    EXPECT_TRUE(entry.slots[0].useful);
    EXPECT_EQ(entry.slots[1].position, tage->getBranchIndexInBlock(newPC, startPC))
        << "weakish non-useful slot should be replaced by the new branch slot";
    EXPECT_EQ(entry.slots[1].counter, -1);
    EXPECT_FALSE(entry.slots[1].useful);
}

TEST_F(BTBTAGETest, SameTagFullEntryWithoutReplaceableSlotSpillsToInvalidWay) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x3000;
    const Addr weakenedPC = startPC;
    const Addr protectedPC = startPC + 4;
    const Addr newPC = startPC + 8;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);

    createManualTageEntry(tage, testTable, index, 0, tag,
                          2, false, weakenedPC, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, index, 0, tag,
                          -2, true, protectedPC, 0, 1, startPC, false);
    BTBEntry newEntry = createBTBEntry(newPC);
    forceMispredPredictUpdateCycle(tage, startPC, newEntry, history, stagePreds);

    const auto &entry0 = tage->tageTable[testTable][index][0];
    const auto &entry1 = tage->tageTable[testTable][index][1];
    ASSERT_TRUE(entry0.valid);
    ASSERT_TRUE(entry1.valid);
    EXPECT_EQ(countWaysWithTag(tage, testTable, index, tag), 2u);
    EXPECT_EQ(entry0.slots[0].position, tage->getBranchIndexInBlock(weakenedPC, startPC));
    EXPECT_EQ(entry0.slots[0].counter, 2);
    EXPECT_EQ(entry0.slots[1].position, tage->getBranchIndexInBlock(protectedPC, startPC));
    EXPECT_EQ(entry0.slots[1].counter, -2);
    EXPECT_EQ(findWayWithTagAndPosition(tage, testTable, index, tag,
                                        tage->getBranchIndexInBlock(newPC, startPC)),
              1);
    EXPECT_EQ(entry1.slots[0].counter, -1);
    EXPECT_EQ(tage->tageStats.allocSameTagSpillUseInvalidWay, 1)
        << "a full non-replaceable same-tag working set should spill into an invalid way";
    EXPECT_EQ(tage->tageStats.updateAllocFailure, 0)
        << "successful same-tag spill should not count as an allocation failure";
}

TEST_F(BTBTAGETest, SameTagFullEntryWithoutReplaceableSlotSpillsByWholeEntryEviction) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x3400;
    auto collidingBlocks = findCollidingBlockStarts(tage, startPC, testTable, 1);
    ASSERT_EQ(collidingBlocks.size(), 1u);
    const Addr otherBlock = collidingBlocks[0];
    const Addr keptPC = startPC;
    const Addr protectedPC = startPC + 4;
    const Addr newPC = startPC + 8;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);
    const Addr otherTag = tage->getTageTag(otherBlock, testTable);

    createManualTageEntry(tage, testTable, index, 0, tag,
                          2, false, keptPC, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, index, 0, tag,
                          -2, true, protectedPC, 0, 1, startPC, false);

    createManualTageEntry(tage, testTable, index, 1, otherTag,
                          0, false, otherBlock, 0, 0, otherBlock, true);
    createManualTageEntry(tage, testTable, index, 1, otherTag,
                          -1, false, otherBlock + 4, 0, 1, otherBlock, false);

    BTBEntry newEntry = createBTBEntry(newPC);
    forceMispredPredictUpdateCycle(tage, startPC, newEntry, history, stagePreds);

    const auto &entry0 = tage->tageTable[testTable][index][0];
    const auto &entry1 = tage->tageTable[testTable][index][1];
    ASSERT_TRUE(entry0.valid);
    ASSERT_TRUE(entry1.valid);
    EXPECT_EQ(countWaysWithTag(tage, testTable, index, tag), 2u);
    EXPECT_EQ(entry1.tag, tag)
        << "same-tag spill should be allowed to reuse a whole-entry eviction victim";
    EXPECT_EQ(findWayWithTagAndPosition(tage, testTable, index, tag,
                                        tage->getBranchIndexInBlock(newPC, startPC)),
              1);
    EXPECT_EQ(tage->tageStats.allocSameTagSpillWholeEvict, 1);
}

TEST_F(BTBTAGETest, SameTagSpillFailureWeakensSetAndCountsAllocationFailure) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x3800;
    auto collidingBlocks = findCollidingBlockStarts(tage, startPC, testTable, 1);
    ASSERT_EQ(collidingBlocks.size(), 1u);
    const Addr otherBlock = collidingBlocks[0];
    const Addr weakenedPC = startPC;
    const Addr protectedPC = startPC + 4;
    const Addr newPC = startPC + 8;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);
    const Addr otherTag = tage->getTageTag(otherBlock, testTable);

    createManualTageEntry(tage, testTable, index, 0, tag,
                          2, false, weakenedPC, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, index, 0, tag,
                          -2, true, protectedPC, 0, 1, startPC, false);
    createManualTageEntry(tage, testTable, index, 1, otherTag,
                          2, false, otherBlock, 0, 0, otherBlock, true);
    createManualTageEntry(tage, testTable, index, 1, otherTag,
                          -2, true, otherBlock + 4, 0, 1, otherBlock, false);

    BTBEntry newEntry = createBTBEntry(newPC);
    forceAllocationUpdateCycle(tage, startPC, newEntry, false, history, stagePreds);

    const auto &entry0 = tage->tageTable[testTable][index][0];
    ASSERT_TRUE(entry0.valid);
    EXPECT_EQ(countWaysWithTag(tage, testTable, index, tag), 1u);
    EXPECT_EQ(entry0.slots[0].position, tage->getBranchIndexInBlock(weakenedPC, startPC));
    EXPECT_EQ(entry0.slots[0].counter, 1)
        << "when same-tag spill has no invalid way or whole-entry victim, the set should weaken the first strong non-useful slot";
    EXPECT_EQ(findWayWithTagAndPosition(tage, testTable, index, tag,
                                        tage->getBranchIndexInBlock(newPC, startPC)),
              -1);
    EXPECT_EQ(tage->tageStats.updateAllocFailure, 1)
        << "same-tag spill failure should still count as an allocation failure";
}

TEST_F(BTBTAGETest, UpdateUsesBranchPositionWhenSlotOrderChanges) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const Addr startPC = 0x4000;
    BTBEntry earlyEntry = createBTBEntry(startPC, true, true, false, 0);
    BTBEntry lateEntry = createBTBEntry(startPC + 4, true, true, false, 0);

    // Prediction-time state: only the later branch exists, so it is saved as slot 0.
    setupTageEntry(tage, lateEntry.pc, 0, 2, false, 0, startPC, 0);

    stagePreds[1].btbEntries = {earlyEntry, lateEntry};
    tage->putPCHistory(startPC, history, stagePreds);
    auto meta = tage->getPredictionMeta();
    auto tage_meta = std::static_pointer_cast<BTBTAGE::TageMeta>(meta);
    ASSERT_TRUE(tage_meta->preds.count(earlyEntry.pc));
    ASSERT_TRUE(tage_meta->preds.count(lateEntry.pc));
    ASSERT_FALSE(tage_meta->preds[earlyEntry.pc].mainInfo.found);
    ASSERT_TRUE(tage_meta->preds[lateEntry.pc].mainInfo.found);
    ASSERT_EQ(tage_meta->preds[lateEntry.pc].mainInfo.slot, 0u);

    // Update the earlier branch first. It allocates a new slot at position 0 and
    // reorders the shared entry, so the later branch moves from slot 0 to slot 1.
    FetchTarget stream;
    stream.startPC = startPC;
    stream.exeBranchInfo = earlyEntry;
    stream.exeTaken = false;
    stream.resolved = true;
    stream.predBranchInfo = earlyEntry;
    stream.updateBTBEntries = {earlyEntry, lateEntry};
    stream.updateIsOldEntry = true;
    stream.predMetas[0] = meta;
    stream = setMispredStream(stream);

    tage->update(stream);

    const Addr index = tage->getTageIndex(startPC, 0);
    const auto &entry = tage->tageTable[0][index][0];
    ASSERT_TRUE(entry.valid);
    ASSERT_TRUE(entry.slots[0].valid);
    ASSERT_TRUE(entry.slots[1].valid);
    EXPECT_EQ(entry.slots[0].position, tage->getBranchIndexInBlock(earlyEntry.pc, startPC));
    EXPECT_EQ(entry.slots[0].counter, -1)
        << "the newly inserted earlier branch slot should not be trained by the later branch update";
    EXPECT_EQ(entry.slots[1].position, tage->getBranchIndexInBlock(lateEntry.pc, startPC));
    EXPECT_EQ(entry.slots[1].counter, 1)
        << "the later branch update should follow branch position after slot reordering";
}

TEST_F(BTBTAGETest, DuplicateSameTagEntriesLookupByPosition) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x4400;
    const Addr entry1PC = startPC;
    const Addr entry2PC = startPC + 4;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);

    createManualTageEntry(tage, testTable, index, 0, tag,
                          2, false, entry1PC, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, index, 1, tag,
                          -2, false, entry2PC, 0, 0, startPC, true);

    stagePreds[1].btbEntries = {createBTBEntry(entry1PC), createBTBEntry(entry2PC)};
    tage->putPCHistory(startPC, history, stagePreds);

    auto result1 = findCondTaken(stagePreds[1].condTakens, entry1PC);
    auto result2 = findCondTaken(stagePreds[1].condTakens, entry2PC);
    ASSERT_TRUE(result1.first);
    ASSERT_TRUE(result2.first);
    EXPECT_TRUE(result1.second);
    EXPECT_FALSE(result2.second);

    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    ASSERT_TRUE(meta->preds.count(entry1PC));
    ASSERT_TRUE(meta->preds.count(entry2PC));
    EXPECT_EQ(meta->preds[entry1PC].mainInfo.way, 0u);
    EXPECT_EQ(meta->preds[entry2PC].mainInfo.way, 1u);
}

TEST_F(BTBTAGETest, DuplicateSameTagEntriesCountSingleSlotMiss) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const int testTable = 0;
    const Addr startPC = 0x4800;
    const Addr index = tage->getTageIndex(startPC, testTable);
    const Addr tag = tage->getTageTag(startPC, testTable);

    createManualTageEntry(tage, testTable, index, 0, tag,
                          2, false, startPC, 0, 0, startPC, true);
    createManualTageEntry(tage, testTable, index, 1, tag,
                          -2, false, startPC + 4, 0, 0, startPC, true);

    stagePreds[1].btbEntries = {createBTBEntry(startPC + 8)};
    tage->putPCHistory(startPC, history, stagePreds);

    EXPECT_EQ(tage->tageStats.predTagHitSlotMiss, 1)
        << "multiple same-tag entries without the requested position should still count as one table-level slot miss";
}

TEST_F(BTBTAGETest, UpdateUsesBranchPositionWhenProviderWayChanges) {
    tage = new BTBTAGE(1, 2, 32);
    memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
    history.resize(64, false);
    stagePreds.resize(2);

    const Addr startPC = 0x4c00;
    const Addr index = tage->getTageIndex(startPC, 0);
    const Addr tag = tage->getTageTag(startPC, 0);
    BTBEntry lateEntry = createBTBEntry(startPC + 4, true, true, false, 0);
    BTBEntry earlierEntry = createBTBEntry(startPC, true, true, false, 0);

    createManualTageEntry(tage, 0, index, 0, tag,
                          2, false, lateEntry.pc, 0, 0, startPC, true);

    stagePreds[1].btbEntries = {lateEntry};
    tage->putPCHistory(startPC, history, stagePreds);
    auto meta = tage->getPredictionMeta();
    auto tage_meta = std::static_pointer_cast<BTBTAGE::TageMeta>(meta);
    ASSERT_TRUE(tage_meta->preds.count(lateEntry.pc));
    ASSERT_EQ(tage_meta->preds[lateEntry.pc].mainInfo.way, 0u);

    createManualTageEntry(tage, 0, index, 0, tag,
                          -1, false, earlierEntry.pc, 0, 0, startPC, true);
    createManualTageEntry(tage, 0, index, 1, tag,
                          2, false, lateEntry.pc, 0, 0, startPC, true);

    FetchTarget stream;
    stream.startPC = startPC;
    stream.exeBranchInfo = lateEntry;
    stream.exeTaken = true;
    stream.resolved = true;
    stream.predBranchInfo = lateEntry;
    stream.updateBTBEntries = {lateEntry};
    stream.updateIsOldEntry = true;
    stream.predMetas[0] = meta;

    tage->update(stream);

    const auto &way0 = tage->tageTable[0][index][0];
    const auto &way1 = tage->tageTable[0][index][1];
    EXPECT_EQ(way0.slots[0].counter, -1)
        << "set-level fallback should not update the stale preferred way";
    EXPECT_EQ(way1.slots[0].counter, 3)
        << "set-level fallback should relocate to the live same-tag entry by position";
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
 * @brief Test bank conflict detection
 *
 * Verifies:
 * 1. Same bank access causes conflict and drops update (when enabled)
 * 2. Different bank access has no conflict
 * 3. Disabled flag prevents conflict detection
 */
TEST_F(BTBTAGETest, BankConflict) {
    // Create TAGE with 4 banks
    BTBTAGE *bankTage = new BTBTAGE(4, 2, 1024, 4);
    boost::dynamic_bitset<> testHistory(128);
    std::vector<FullBTBPrediction> testStagePreds(5);

    // Bank ID derives from bits [2:1] (pc >> 1) & 0x3 when instShiftAmt == 1.
    // Bank 0: ..., 0x100, 0x108 ...  Bank 1: ..., 0x102, 0x10A ...
    // Bank 2: ..., 0x104, 0x10C ...  Bank 3: ..., 0x106, 0x10E ...

    // Test 1: Same bank conflict (enabled)
    bankTage->enableBankConflict = true;
    {
        // Predict on bank 1 (0x20), then update on bank 1 (0xa0)
        testStagePreds[1].btbEntries = {createBTBEntry(0x20)};
        bankTage->putPCHistory(0x20, testHistory, testStagePreds);
        EXPECT_TRUE(bankTage->predBankValid);

        auto meta = bankTage->getPredictionMeta();
        FetchTarget stream = createStream(0xa0, createBTBEntry(0xa0), true, meta);
        setupTageEntry(bankTage, 0xa0, 0, 1, false);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(stream);

        // Should detect conflict and defer update
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before + 1);
        EXPECT_FALSE(can_update);
        EXPECT_FALSE(bankTage->predBankValid);
    }

    // Test 2: Different bank, no conflict
    {
        // Predict on bank 0 (0x100), update on bank 2 (0x104)
        testStagePreds[1].btbEntries = {createBTBEntry(0x100)};
        bankTage->putPCHistory(0x100, testHistory, testStagePreds);

        auto meta = bankTage->getPredictionMeta();
        FetchTarget stream = createStream(0x104, createBTBEntry(0x104), true, meta);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(stream);
        ASSERT_TRUE(can_update);
        bankTage->doResolveUpdate(stream);

        // Should not detect conflict
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before);
    }

    // Test 3: Disabled flag prevents conflict
    bankTage->enableBankConflict = false;
    {
        // Same bank (0x20 and 0xa0), but conflict disabled
        testStagePreds[1].btbEntries = {createBTBEntry(0x20)};
        bankTage->putPCHistory(0x20, testHistory, testStagePreds);

        auto meta = bankTage->getPredictionMeta();
        FetchTarget stream = createStream(0xa0, createBTBEntry(0xa0), true, meta);
        setupTageEntry(bankTage, 0xa0, 0, 1, false);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(stream);
        ASSERT_TRUE(can_update);
        bankTage->doResolveUpdate(stream);

        // No conflict even with same bank
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before);
    }
}

class BTBTAGEUpperBoundTest : public ::testing::Test
{
  protected:
    void SetUp() override {
        tage = new BTBTAGEUpperBound();
        memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
        history.resize(128, false);
        stagePreds.resize(2);
    }

    BTBTAGEUpperBound *tage;
    boost::dynamic_bitset<> history;
    std::vector<FullBTBPrediction> stagePreds;
};

class BTBTAGEUpperBoundPathHashTest : public ::testing::Test
{
  protected:
    void SetUp() override {
        tage = new BTBTAGEUpperBound(4, 1024, 4,
            BTBTAGEUpperBound::HistorySource::PathHash);
        memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
        outcomeHistory.resize(128, false);
        pathHistory.resize(128, false);
        stagePreds.resize(2);
    }

    BTBTAGEUpperBound *tage;
    boost::dynamic_bitset<> outcomeHistory;
    boost::dynamic_bitset<> pathHistory;
    std::vector<FullBTBPrediction> stagePreds;
};

TEST_F(BTBTAGEUpperBoundTest, ExactContextLookup) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, -1);
    boost::dynamic_bitset<> historyA(128, 0);
    boost::dynamic_bitset<> historyB(128, 0);
    historyB[0] = true;

    ASSERT_TRUE(tage->insertExactEntry(3, entry.pc, historyA, 2));
    EXPECT_TRUE(tage->hasExactEntry(3, entry.pc, historyA));
    EXPECT_FALSE(tage->hasExactEntry(3, entry.pc, historyB));

    bool predA = predictTAGE(tage, 0x1000, {entry}, historyA, stagePreds);
    bool predB = predictTAGE(tage, 0x1000, {entry}, historyB, stagePreds);

    EXPECT_TRUE(predA);
    EXPECT_FALSE(predB);
}

TEST_F(BTBTAGEUpperBoundTest, ProviderAltSelection) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, -1);

    ASSERT_TRUE(tage->insertExactEntry(3, entry.pc, history, 0));
    ASSERT_TRUE(tage->insertExactEntry(1, entry.pc, history, -2));

    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    auto pred = meta->preds[entry.pc];

    EXPECT_EQ(pred.mainInfo.table, 3u);
    EXPECT_EQ(pred.altInfo.table, 1u);
    EXPECT_TRUE(pred.useAlt);
    EXPECT_FALSE(pred.taken);
}

TEST_F(BTBTAGEUpperBoundTest, AllocationUsesPredictionTimeHistory) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, -1);
    boost::dynamic_bitset<> historyA(128, 0);
    boost::dynamic_bitset<> historyB(128, 0);
    historyB[0] = true;

    predictTAGE(tage, 0x1000, {entry}, historyA, stagePreds);
    auto meta = tage->getPredictionMeta();

    FetchTarget stream = createStream(0x1000, entry, true, meta);
    stream = setMispredStream(stream);

    tage->recoverHist(historyB, stream, 1, true);
    tage->update(stream);

    EXPECT_TRUE(tage->hasExactEntry(0, entry.pc, historyA));
    EXPECT_FALSE(tage->hasExactEntry(0, entry.pc, historyB));
}

TEST_F(BTBTAGEUpperBoundTest, NewConditionalEntryWithoutPredictionMetaStillTrains) {
    boost::dynamic_bitset<> historyA(128, 0);
    stagePreds[1].btbEntries.clear();
    tage->putPCHistory(0x1000, historyA, stagePreds);
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

    EXPECT_TRUE(tage->hasExactEntry(0, newEntry.pc, historyA));
}

TEST_F(BTBTAGEUpperBoundPathHashTest, PredictionUsesPathHashHistorySnapshot) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, -1, 0x2000);
    boost::dynamic_bitset<> pathHistoryA(128, 0);
    boost::dynamic_bitset<> pathHistoryB(128, 0);
    applyPathHistoryTaken(pathHistoryB, entry.pc, entry.target);

    ASSERT_TRUE(tage->insertExactEntry(2, entry.pc, pathHistoryB, 2));

    FullBTBPrediction pred;
    pred.btbEntries.push_back(entry);
    pred.condTakens.push_back({entry.pc, true});
    tage->specUpdatePHist(pathHistoryA, pred);

    bool predicted = predictTAGE(tage, 0x1000, {entry}, outcomeHistory, stagePreds);

    EXPECT_TRUE(predicted);
}

TEST_F(BTBTAGEUpperBoundPathHashTest, PredictionUsesIndirectOverridePathHashSnapshot) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, -1, 0x2000);
    entry.isIndirect = true;
    const Addr indirectTarget = 0x3000;

    ASSERT_NE(pathHash(entry.pc, entry.target), pathHash(entry.pc, indirectTarget));

    boost::dynamic_bitset<> pathHistoryA(128, 0);
    boost::dynamic_bitset<> pathHistoryB(128, 0);
    applyPathHistoryTaken(pathHistoryB, entry.pc, indirectTarget);

    ASSERT_TRUE(tage->insertExactEntry(2, entry.pc, pathHistoryB, 2));

    FullBTBPrediction pred;
    pred.btbEntries.push_back(entry);
    pred.condTakens.push_back({entry.pc, true});
    pred.indirectTargets.push_back({entry.pc, indirectTarget});

    tage->specUpdatePHist(pathHistoryA, pred);
    tage->checkFoldedHist(pathHistoryB, "indirect target override");

    bool predicted = predictTAGE(tage, 0x1000, {entry}, outcomeHistory, stagePreds);

    EXPECT_TRUE(predicted);
}

TEST_F(BTBTAGEUpperBoundPathHashTest, PredictionUsesReturnOverridePathHashSnapshot) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, false, -1, 0x2000);
    entry.isIndirect = true;
    entry.isReturn = true;
    const Addr returnTarget = 0x3400;

    ASSERT_NE(pathHash(entry.pc, entry.target), pathHash(entry.pc, returnTarget));

    boost::dynamic_bitset<> pathHistoryA(128, 0);
    boost::dynamic_bitset<> pathHistoryB(128, 0);
    applyPathHistoryTaken(pathHistoryB, entry.pc, returnTarget);

    ASSERT_TRUE(tage->insertExactEntry(2, entry.pc, pathHistoryB, 2));

    FullBTBPrediction pred;
    pred.btbEntries.push_back(entry);
    pred.condTakens.push_back({entry.pc, true});
    pred.returnTarget = returnTarget;

    tage->specUpdatePHist(pathHistoryA, pred);
    tage->checkFoldedHist(pathHistoryB, "return target override");

    bool predicted = predictTAGE(tage, 0x1000, {entry}, outcomeHistory, stagePreds);

    EXPECT_TRUE(predicted);
}


}  // namespace test

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

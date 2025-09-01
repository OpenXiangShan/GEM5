#include <gtest/gtest.h>
#include <algorithm>
#include "cpu/pred/btb/btb_tage.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "base/types.hh"

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
 * @return BTBEntry Initialized branch entry
 */
BTBEntry createBTBEntry(Addr pc, bool isCond = true, bool valid = true,
                        bool alwaysTaken = false, int ctr = 0) {
    BTBEntry entry;
    entry.pc = pc;
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
 * @return FetchStream Initialized stream for update or recovery
 */
FetchStream createStream(Addr startPC, const BTBEntry& entry, bool taken,
                         std::shared_ptr<void> meta) {
    FetchStream stream;
    stream.startPC = startPC;
    stream.exeBranchInfo = entry;
    stream.exeTaken = taken;
    stream.updateBTBEntries = {entry};
    stream.updateIsOldEntry = true;
    stream.predMetas[0] = meta;
    return stream;
}

FetchStream setMispredStream(FetchStream stream) {
    stream.squashType = SquashType::SQUASH_CTRL;
    stream.squashPC = stream.exeBranchInfo.pc;
    return stream;
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

    // 3. Speculatively update history
    tage->specUpdateHist(history, stagePreds[1]);
    auto meta = tage->getPredictionMeta();

    // 4. Update path history register, see pHistShiftIn
    bool history_updated = false;
    Addr pc = entry.pc; // use control PC for PHR bits
    if (predicted_taken) {
        history_updated = true;
        history <<= 2;
        history[0] = (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 1);       // pc[1] ^ pc[3] ^ pc[5] ^ pc[7]
        history[1] = (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 2) >> 1;  // pc[2] ^ pc[4] ^ pc[6] ^ pc[8]
    }
    tage->checkFoldedHist(history, "speculative update");

    // 5. Create update stream
    FetchStream stream = createStream(startPC, entry, actual_taken, meta);

    // 6. Handle possible misprediction
    if (predicted_taken != actual_taken) {
        stream = setMispredStream(stream);
        // Update history with correct outcome
        if (history_updated) {
            history >>= 2;  // Undo speculative update
        }
        // Recover from misprediction
        tage->recoverHist(history, stream, 1, actual_taken);

        if (actual_taken) {
            history <<= 2;  // Re-shift
            history[0] = (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 1);       // pc[1] ^ pc[3] ^ pc[5] ^ pc[7]
            history[1] = (((pc>>1)^(pc>>3)^(pc>>5)^(pc>>7)) & 2) >> 1;  // pc[2] ^ pc[4] ^ pc[6] ^ pc[8]
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

/**
 * @brief Verify TAGE table entries
 *
 * @param tage The TAGE predictor
 * @param pc Branch instruction address to check
 * @param expected_tables Vector of expected table indices to have valid entries
 */
void verifyTageEntries(BTBTAGE* tage, Addr pc, const std::vector<int>& expected_tables) {
    for (int t = 0; t < tage->numPredictors; t++) {
        for (int way = 0; way < tage->numWays; way++) {
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
 * @brief Find the table with a valid entry for a given PC
 *
 * @param tage The TAGE predictor
 * @param pc Branch instruction address to check
 * @return int Index of the table with valid entry (-1 if not found)
 */
int findTableWithEntry(BTBTAGE* tage, Addr pc) {
    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    // use meta to find the table, predicted info
    for (int t = 0; t < tage->numPredictors; t++) {
        Addr index = tage->getTageIndex(pc, t, meta->indexFoldedHist[t].get());
        for (int way = 0; way < tage->numWays; way++) {
            auto &entry = tage->tageTable[t][index][way];
            if (entry.valid && entry.pc == pc) {
                return t;
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
    int table = findTableWithEntry(tage, 0x1000);
    EXPECT_GE(table, 0) << "No TAGE table entry was allocated";
}

// Test basic history update functionality (PHR semantics)
TEST_F(BTBTAGETest, HistoryUpdate) {
    // Use a fixed control PC to derive PHR bits
    Addr pc = 0x1000;

    // Test case 1: Update with taken branch (PHR shifts in 2 bits from PC hash)
    auto pc_hash = ((pc >> 1) ^ (pc >> 3) ^ (pc >> 5) ^ (pc >> 7));
    history <<= 2;
    history[0] = (pc_hash & 1);          // pc[1] ^ pc[3] ^ pc[5] ^ pc[7]
    history[1] = ((pc_hash & 2) >> 1);   // pc[2] ^ pc[4] ^ pc[6] ^ pc[8]
    // Update folded histories using PHR semantics
    tage->doUpdateHist(history, true, pc);

    // Verify folded history matches the ideal fold of the updated PHR
    tage->checkFoldedHist(history, "taken update");

    // Test case 2: Update with not-taken branch (PHR unchanged, folded update is no-op)
    tage->doUpdateHist(history, false, pc);

    // Verify folded history remains consistent
    tage->checkFoldedHist(history, "not-taken update");
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
    FetchStream stream = createStream(0x1000, entry, true, meta);
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

    // Verify useful bit is cleared (main prediction was incorrect)
    EXPECT_FALSE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should be cleared when main predicts incorrectly";
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
    FetchStream stream = createStream(0x1000, entry2, !predicted, meta);
    stream.squashType = SquashType::SQUASH_CTRL; // Mark as control misprediction
    stream.squashPC = 0x1000;

    // Update the predictor (this should try to allocate but fail)
    tage->update(stream);

    int alloc_failed = tage->tageStats.updateAllocFailure;
    EXPECT_GE(alloc_failed, 1) << "Allocate failed when all useful bits are set";

}

// Test history recovery mechanism
TEST_F(BTBTAGETest, HistoryRecoveryCorrectness) {
    BTBEntry entry = createBTBEntry(0x1000);

    // Record initial history state
    boost::dynamic_bitset<> originalHistory = history;

    // Store original folded history state
    std::vector<FoldedHist> originalTagFoldedHist;
    std::vector<FoldedHist> originalAltTagFoldedHist;
    std::vector<FoldedHist> originalIndexFoldedHist;

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
        Addr pc = entry.pc;
        auto pc_hash = ((pc >> 1) ^ (pc >> 3) ^ (pc >> 5) ^ (pc >> 7));
        history <<= 2;
        history[0] = (pc_hash & 1);
        history[1] = ((pc_hash & 2) >> 1);
    }

    // Create a recovery stream with opposite outcome
    FetchStream stream = createStream(0x1000, entry, !predicted_taken, meta);
    stream = setMispredStream(stream);

    // Recover to pre-speculative state and update with correct outcome
    boost::dynamic_bitset<> recoveryHistory = originalHistory;
    tage->recoverHist(recoveryHistory, stream, 1, !predicted_taken);

    // Expected history should be original updated with PHR if actually taken
    boost::dynamic_bitset<> expectedHistory = originalHistory;
    if (!predicted_taken) { // actual_taken
        Addr pc = entry.pc;
        auto pc_hash = ((pc >> 1) ^ (pc >> 3) ^ (pc >> 5) ^ (pc >> 7));
        expectedHistory <<= 2;
        expectedHistory[0] = (pc_hash & 1);
        expectedHistory[1] = ((pc_hash & 2) >> 1);
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
    FetchStream stream1 = createStream(0x1000, btbEntries[0], first_pred, meta);
    tage->update(stream1);

    // Update second branch (incorrect prediction), allocate 1 entry
    FetchStream stream2 = createStream(0x1000, btbEntries[1], !second_pred, meta);
    stream2.squashType = SquashType::SQUASH_CTRL;
    stream2.squashPC = 0x1004;
    tage->update(stream2);

    // Verify both branches have entries allocated
    EXPECT_EQ(findTableWithEntry(tage, 0x1000), -1) << "First branch should not have an entry";
    EXPECT_GE(findTableWithEntry(tage, 0x1004), 0) << "Second branch should have an entry";
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

        FetchStream stream = createStream(0x1000, entry, true, meta);
        tage->update(stream);
    }

    // Verify counter saturates at maximum
    EXPECT_EQ(tage->tageTable[testTable][index][0].counter, 3)
        << "Counter should saturate at maximum value";

    // Train with not-taken outcomes multiple times
    for (int i = 0; i < 7; i++) {
        predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
        auto meta = tage->getPredictionMeta();

        FetchStream stream = createStream(0x1000, entry, false, meta);
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
    Addr testTag = tage->getTageTag(startPC, testTable);

    // Manually create entries with the same index same tag, but different branch PC/pos
    createManualTageEntry(tage, testTable, testIndex, 0, testTag, 2, false, 0x1000, 0); // Way 0: Strong taken
    createManualTageEntry(tage, testTable, testIndex, 1, testTag, -2, false, 0x1004, 1); // Way 1: Strong not taken

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
 * @brief Test allocation behavior with multiple ways
 *
 * This test verifies:
 * 1. New entries are allocated in invalid ways first
 * 2. When all ways are valid, the LRU way is chosen
 * 3. Multiple allocation attempts correctly manage way selection
 */
TEST_F(BTBTAGETest, AllocationBehaviorWithMultipleWays) {
    // Start with a fresh predictor
    tage = new BTBTAGE(1, 2, 10); // only 1 predictor table, 2 ways
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
    for (unsigned way = 0; way < tage->numWays; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid &&
            tage->tageTable[testTable][testIndex][way].pc == 0x1000) {
            allocatedWay = way;
            break;
        }
    }

    EXPECT_GE(allocatedWay, 0) << "Entry should be allocated in one of the ways";

    // Step 2: Fill remaining ways with different branches
    for (unsigned way = 0; way < tage->numWays; way++) {
        if (way == allocatedWay) continue;

        // Create a branch with different PC
        BTBEntry newEntry = createBTBEntry(0x1004);

        // Make prediction and force allocation
        bool predicted = predictUpdateCycle(tage, 0x1000, newEntry, false, history, stagePreds);
    }

    // Verify all ways are now filled
    int filledWays = 0;
    for (unsigned way = 0; way < tage->numWays; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid) {
            filledWays++;
        }
    }

    EXPECT_EQ(filledWays, tage->numWays) << "All ways should be filled after multiple allocations";

    // Step 3: One more allocation should replace LRU entry
    BTBEntry newEntry = createBTBEntry(0x1008);
    bool predicted = predictUpdateCycle(tage, 0x1000, newEntry, false, history, stagePreds);

    // Check if the new entry was allocated
    bool found = false;
    unsigned foundWay = 0;
    for (unsigned way = 0; way < tage->numWays; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid &&
            tage->tageTable[testTable][testIndex][way].pc == 0x1008) {
            found = true;
            foundWay = way;
            break;
        }
    }

    EXPECT_TRUE(found) << "New entry should be allocated by replacing an old entry";
    EXPECT_EQ(foundWay, allocatedWay) << "New entry should be allocated in the same way as the first allocation";
}


}  // namespace test

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

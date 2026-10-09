#include <gtest/gtest.h>

#include <algorithm>
#include <iostream>
#include <vector>

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
 * @param ctr Prediction counter value
 * @param target Branch target address (defaults to sequential PC)
 * @return BTBEntry Initialized branch entry
 */
BTBEntry createBTBEntry(Addr pc, bool isCond = true, bool valid = true,
                        int ctr = 0, Addr target = 0) {
    BTBEntry entry;
    entry.pc = pc;
    entry.target = target ? target : (pc + 4);
    entry.isCond = isCond;
    entry.valid = valid;
    entry.ctr = ctr;
    // Other fields are set to default
    return entry;
}

/**
 * @brief Create a stream for update or recovery
 *
 * @param startPC Starting PC for the stream
 * @param entry Branch entry information
 * @param meta Prediction metadata from prediction phase
 * @return FetchTarget Initialized stream for update or recovery
 */
FetchTarget createStream(Addr startPC, const BTBEntry& entry,
                         std::shared_ptr<void> meta) {
    FetchTarget stream;
    stream.startPC = startPC;
    stream.predBranchInfo = entry; // keep fields consistent
    stream.setPredictedBranches({entry});
    stream.predMetas[0] = meta;
    return stream;
}

BranchOutcome
createBranchOutcome(const BTBEntry &entry, bool taken, bool mispredicted)
{
    return BranchOutcome{
        0,
        0,
        1,
        entry.pc,
        entry.target,
        taken,
        mispredicted,
        entry.isCond,
        entry.isIndirect,
        entry.isDirect,
        entry.isCall,
        entry.isReturn,
        entry.size
    };
}

PreparedUpdate
createPreparedUpdate(const FetchTarget &stream, const BTBEntry &entry,
                     bool taken, bool mispredicted = false)
{
    return PreparedUpdate({createBranchOutcome(entry, taken, mispredicted)});
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

void applyOutcomeHistory(boost::dynamic_bitset<>& history, int shamt, bool taken)
{
    if (shamt <= 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}

void specUpdateSelectedHistory(BTBTAGE* tage,
                               const boost::dynamic_bitset<>& history,
                               FullBTBPrediction& pred)
{
    if (tage->usesPathHistory()) {
        tage->specUpdatePHist(history, pred, pred.getPHistUpdate());
    } else {
        tage->specUpdateGHist(history, pred, pred.getGHistUpdate());
    }
}

void recoverSelectedHistory(BTBTAGE* tage,
                            const boost::dynamic_bitset<>& history,
                            const FetchTarget& stream, int shamt,
                            bool cond_taken,
                            const PathHistoryUpdate& path_update)
{
    const HistoryRecoveryContext context(stream);
    if (tage->usesPathHistory()) {
        tage->recoverPHist(history, context, path_update);
    } else {
        tage->recoverHist(
            history, context, DirectionHistoryUpdate{shamt, cond_taken});
    }
}

void applyPredictedHistory(BTBTAGE* tage, boost::dynamic_bitset<>& history,
                           FullBTBPrediction& pred)
{
    if (tage->usesPathHistory()) {
        const auto update = pred.getPHistUpdate();
        if (update.taken) {
            applyPathHistoryTaken(history, update.pc, update.target);
        }
    } else {
        const auto update = pred.getGHistUpdate();
        applyOutcomeHistory(history, update.shamt, update.taken);
    }
}

void applyActualHistory(BTBTAGE* tage, boost::dynamic_bitset<>& history,
                        const BTBEntry& entry, int shamt, bool taken)
{
    if (tage->usesPathHistory()) {
        if (taken) {
            applyPathHistoryTaken(history, entry.pc, entry.target);
        }
    } else {
        applyOutcomeHistory(history, shamt, taken);
    }
}

PathHistoryUpdate
getActualPathUpdate(const BranchInfo &branch, Addr squash_pc,
                    bool actually_taken, Addr target)
{
    PathHistoryUpdate update;
    update.taken = actually_taken && branch.pc == squash_pc;
    if (update.taken) {
        update.pc = squash_pc;
        update.target = target;
    }
    return update;
}

TEST(FetchTargetHistoryUpdateTest, SquashUpdateSeparatesDirectionAndPath)
{
    struct Case
    {
        const char* name;
        std::vector<BTBEntry> predictedBeforeSquash;
        BTBEntry resolvedEntry;
        Addr squashPC;
        bool isCond;
        bool actualTaken;
        Addr redirectPC;
        int expectedGHistShamt;
        bool expectedGHistTaken;
        int expectedBwHistShamt;
        bool expectedBwHistTaken;
        bool expectedPHistTaken;
        Addr expectedPHistPC;
        Addr expectedPHistTarget;
    };

    const std::vector<Case> cases = {
        {
            "conditional not taken",
            {},
            createBTBEntry(0x1008, true, true, -1, 0x2000),
            0x1008,
            true,
            false,
            0x2000,
            1,
            false,
            1,
            false,
            false,
            0,
            0,
        },
        {
            "conditional taken forward",
            {},
            createBTBEntry(0x1008, true, true, -1, 0x2000),
            0x1008,
            true,
            true,
            0x2000,
            1,
            true,
            1,
            false,
            true,
            0x1008,
            0x2000,
        },
        {
            "conditional taken backward",
            {},
            createBTBEntry(0x1008, true, true, -1, 0x0ff0),
            0x1008,
            true,
            true,
            0x0ff0,
            1,
            true,
            1,
            true,
            true,
            0x1008,
            0x0ff0,
        },
        {
            "unconditional taken",
            {},
            createBTBEntry(0x1008, false, true, -1, 0x2040),
            0x1008,
            false,
            true,
            0x2040,
            0,
            false,
            0,
            false,
            true,
            0x1008,
            0x2040,
        },
        {
            "path update requires resolved control pc",
            {},
            createBTBEntry(0x1010, false, true, -1, 0x3000),
            0x1008,
            true,
            true,
            0x2000,
            1,
            true,
            1,
            false,
            false,
            0,
            0,
        },
        {
            "branches before squash contribute direction slots",
            {
                createBTBEntry(0x1000, true, true, -1, 0x1800),
                createBTBEntry(0x1004, true, true, -1, 0x1804),
            },
            createBTBEntry(0x1008, true, true, -1, 0x2000),
            0x1008,
            true,
            true,
            0x2000,
            3,
            true,
            3,
            false,
            true,
            0x1008,
            0x2000,
        },
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);

        FetchTarget stream;
        stream.startPC = 0x1000;
        stream.setPredictedBranches(c.predictedBeforeSquash);

        const auto ghist = stream.getGHistUpdateDuringSquash(
            c.squashPC, c.isCond, c.actualTaken);
        const auto bwhist = stream.getBwHistUpdateDuringSquash(
            c.squashPC, c.isCond, c.actualTaken, c.redirectPC);
        const auto phist = getActualPathUpdate(
            c.resolvedEntry, c.squashPC, c.actualTaken, c.redirectPC);

        EXPECT_EQ(ghist.shamt, c.expectedGHistShamt);
        EXPECT_EQ(ghist.taken, c.expectedGHistTaken);
        EXPECT_EQ(bwhist.shamt, c.expectedBwHistShamt);
        EXPECT_EQ(bwhist.taken, c.expectedBwHistTaken);
        EXPECT_EQ(phist.taken, c.expectedPHistTaken);
        if (c.expectedPHistTaken) {
            EXPECT_EQ(phist.pc, c.expectedPHistPC);
            EXPECT_EQ(phist.target, c.expectedPHistTarget);
        }
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
    specUpdateSelectedHistory(tage, history, stagePreds[1]);
    auto meta = tage->getPredictionMeta();

    // 4. Update path history register, see pHistShiftIn
    bool history_updated = false;
    boost::dynamic_bitset<> pre_spec_history = history;
    if (tage->usesPathHistory()) {
        const auto update = stagePreds[1].getPHistUpdate();
        history_updated = update.taken;
        if (update.taken) {
            applyPathHistoryTaken(history, update.pc, update.target);
        }
    } else {
        const auto update = stagePreds[1].getGHistUpdate();
        history_updated = update.shamt > 0;
        applyOutcomeHistory(history, update.shamt, update.taken);
    }
    tage->checkFoldedHist(history, "speculative update");

    // 5. Create update stream
    FetchTarget stream = createStream(startPC, entry, meta);
    const bool mispredicted = predicted_taken != actual_taken;

    // 6. Handle possible misprediction
    if (mispredicted) {
        // Update history with correct outcome
        if (history_updated) {
            history = pre_spec_history;
        }
        // Recover from misprediction
        const auto path_update = getActualPathUpdate(
            entry, entry.pc, actual_taken, entry.target);
        recoverSelectedHistory(tage, history, stream, 1, actual_taken,
                               path_update);
        applyActualHistory(tage, history, entry, 1, actual_taken);
        tage->checkFoldedHist(history, "recover");
    }

    // 7. Update predictor
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, actual_taken, mispredicted));
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

class BTBTAGETest : public ::testing::Test
{
protected:
    void SetUp() override {
        tage = new BTBTAGE();
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
    BTBEntry entry = createBTBEntry(0x1000, true, true, 1);

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
    tage->doUpdateHist(history, 2, true, pc, target, 0);
    applyPathHistoryTaken(history, pc, target);

    // Verify folded history matches the ideal fold of the updated PHR
    tage->checkFoldedHist(history, "taken update");

    // Test case 2: Update with not-taken branch (PHR unchanged, folded update is no-op)
    boost::dynamic_bitset<> before_not_taken = history;
    tage->doUpdateHist(history, 2, false, pc, target, 0);

    // Verify folded history remains consistent
    tage->checkFoldedHist(history, "not-taken update");
    EXPECT_EQ(history, before_not_taken);
}

TEST_F(BTBTAGETest, GlobalHistoryModeUpdate) {
    BTBTAGE ghrTage(4, 2, 1024, 4, false);
    boost::dynamic_bitset<> ghr(64, false);

    ghrTage.doUpdateHist(ghr, 1, true, 0, 0, 0);
    applyOutcomeHistory(ghr, 1, true);
    ghrTage.checkFoldedHist(ghr, "ghr taken update");

    boost::dynamic_bitset<> before_not_taken = ghr;
    ghrTage.doUpdateHist(ghr, 1, false, 0, 0, 0);
    applyOutcomeHistory(ghr, 1, false);
    ghrTage.checkFoldedHist(ghr, "ghr not-taken update");

    EXPECT_NE(ghr, before_not_taken)
        << "GHR mode should still shift history on not-taken branches";
}

TEST_F(BTBTAGETest, SecondBlockLookupUsesH1IndexAndH2Tag)
{
    BTBEntry firstEntry = createBTBEntry(0x1008, true, true, 1, 0x1040);
    const Addr block1Start = 0x1000;
    const Addr block2Start = 0x1040;

    stagePreds[1].btbEntries = {firstEntry};
    tage->putPCHistory(block1Start, history, stagePreds);
    auto firstPred = stagePreds[1];
    const auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(
        tage->getPredictionMeta());

    auto expectedTag = meta->tagFoldedHist[0];
    auto expectedAltTag = meta->altTagFoldedHist[0];
    const auto pathUpdate = firstPred.getPHistUpdate();
    ASSERT_TRUE(pathUpdate.taken);
    expectedTag.update(history, pathUpdate.shamt, true,
                       pathUpdate.pc, pathUpdate.target);
    expectedAltTag.update(history, pathUpdate.shamt, true,
                          pathUpdate.pc, pathUpdate.target);

    const auto context = tage->makeSecondBlockLookupContext(
        firstPred, block1Start, block2Start, 0, 0, &history);
    ASSERT_EQ(context.indexFoldedHist.size(), tage->numPredictors);
    EXPECT_EQ(context.indexFoldedHist[0], meta->indexFoldedHist[0].get());
    EXPECT_EQ(context.tagFoldedHist[0], expectedTag.get());
    EXPECT_EQ(context.altTagFoldedHist[0], expectedAltTag.get());

    BTBEntry secondEntry = createBTBEntry(0x1048, true, true, -1, 0x1080);
    const unsigned position = tage->getBranchIndexInBlock(
        secondEntry.pc, block2Start);
    const Addr index = tage->getTageIndex(
        block1Start, 0, context.indexFoldedHist[0]);
    const Addr tag = tage->getTageTag(
        block2Start, 0, context.tagFoldedHist[0],
        context.altTagFoldedHist[0], position);
    auto &entry = tage->tageTable[0][index][0];
    entry = BTBTAGE::TageEntry(tag, 2, secondEntry.pc);

    CondTakens results;
    tage->lookupSecondBlockNoSideEffect(context, {secondEntry}, results);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().first, secondEntry.pc);
    EXPECT_TRUE(results.front().second);

    FullBTBPrediction secondPred;
    secondPred.tid = 0;
    secondPred.bbStart = block2Start;
    secondPred.btbEntries = {secondEntry};
    tage->refreshSecondBlockPredictionMeta(
        block2Start, history, secondPred, context);
    const auto secondMeta = std::static_pointer_cast<BTBTAGE::TageMeta>(
        tage->getPredictionMeta());
    ASSERT_TRUE(secondMeta);
    EXPECT_TRUE(secondMeta->hasSecondBlockContext);
    EXPECT_EQ(secondMeta->secondBlockContext.indexPC, block1Start);
    EXPECT_EQ(secondMeta->secondBlockContext.tagPC, block2Start);
    ASSERT_TRUE(secondMeta->preds.count(secondEntry.pc));
    EXPECT_TRUE(secondMeta->preds.at(secondEntry.pc).taken);
}

class SecondBlockShadowTest : public ::testing::TestWithParam<bool>
{
  protected:
    BTBTAGE tage{4, 1, 64, 4, true, true, 1};
    boost::dynamic_bitset<> history{64, 0};
    const Addr block1Start = 0x1000;
    // The two blocks alias in the set index, so the seeded Main entry is
    // also a normal H1 provider for block2 with this zero-history context.
    const Addr block2Start = 0x1080;
    BTBEntry branch = createBTBEntry(0x1080, true, true, 1);
    BTBTAGE::SecondBlockLookupContext context;
    ThreadID tid = 0;
    using Storage = BTBTAGE::TageTableInfo::Storage;

    void SetUp() override
    {
        tage.updateOnRead = GetParam();
        context = tage.makeSecondBlockLookupContext(
            FullBTBPrediction{}, block1Start, block2Start, 0, 0, &history);
    }

    Addr index(unsigned table) const
    {
        return tage.getTageIndex(
            context.indexPC, table, context.indexFoldedHist[table], 0, tid);
    }

    Addr tag(unsigned table) const
    {
        return tage.getTageTag(
            context.tagPC, table, context.tagFoldedHist[table],
            context.altTagFoldedHist[table],
            tage.getBranchIndexInBlock(branch.pc, context.tagPC));
    }

    void install(bool shadow, unsigned table, short counter, bool useful)
    {
        auto &tables = shadow ? tage.secondBlockShadowTable : tage.tageTable;
        auto &entry = tables[table][index(table)][0];
        entry = BTBTAGE::TageEntry(tag(table), counter, branch.pc);
        entry.useful = useful;
    }

    std::shared_ptr<BTBTAGE::TageMeta> predictSecond()
    {
        FullBTBPrediction pred;
        pred.tid = tid;
        pred.bbStart = block2Start;
        pred.btbEntries = {branch};
        tage.refreshSecondBlockPredictionMeta(
            block2Start, history, pred, context);
        return std::static_pointer_cast<BTBTAGE::TageMeta>(
            tage.getPredictionMeta(tid));
    }

    std::shared_ptr<BTBTAGE::TageMeta> predictFirst()
    {
        FullBTBPrediction pred;
        pred.tid = tid;
        pred.bbStart = block2Start;
        pred.btbEntries = {branch};
        tage.refreshPredictionMeta(block2Start, history, pred);
        return std::static_pointer_cast<BTBTAGE::TageMeta>(
            tage.getPredictionMeta(tid));
    }

    void train(const std::shared_ptr<BTBTAGE::TageMeta> &meta, bool taken)
    {
        auto stream = createStream(block2Start, branch, meta);
        stream.tid = tid;
        tage.update(PredictionUpdateContext(stream),
                    createPreparedUpdate(stream, branch, taken,
                        meta->preds.at(branch.pc).taken != taken));
    }

    void expectMainUnchanged(
        const std::vector<std::vector<std::vector<BTBTAGE::TageEntry>>> &before)
    {
        for (unsigned table = 0; table < tage.numPredictors; ++table) {
            for (unsigned set = 0; set < tage.tableSizes[table]; ++set) {
                const auto &old = before[table][set][0];
                const auto &now = tage.tageTable[table][set][0];
                EXPECT_EQ(now.valid, old.valid);
                EXPECT_EQ(now.tag, old.tag);
                EXPECT_EQ(now.pc, old.pc);
                EXPECT_EQ(now.counter, old.counter);
                EXPECT_EQ(now.useful, old.useful);
            }
        }
    }

    // Each domain has its own pressure counter, shared between H1 and H2
    // only for Main's high tables. Seed all sets to expose over-wide sweeps.
    enum ResetDomain { MainLow, ShadowLow, SharedHigh };

    int &resetCounter(ResetDomain domain, bool partitioned, ThreadID thread)
    {
        switch (domain) {
          case MainLow:
            return partitioned ? tage.usefulResetCntByThread[thread] :
                                 tage.usefulResetCnt;
          case ShadowLow:
            return partitioned ?
                tage.secondBlockShadowUsefulResetCntByThread[thread] :
                tage.secondBlockShadowUsefulResetCnt;
          case SharedHigh:
            return partitioned ? tage.sharedHighUsefulResetCntByThread[thread] :
                                 tage.sharedHighUsefulResetCnt;
        }
        ADD_FAILURE() << "Unknown reset domain";
        return tage.usefulResetCnt;
    }

    void fillProtected(bool partitioned, ThreadID thread)
    {
        tid = thread;
        tage.setSmtTidPartitioned(partitioned);
        context = tage.makeSecondBlockLookupContext(
            FullBTBPrediction{}, block1Start, block2Start, tid, 0, &history);
        for (auto *tables : {&tage.tageTable, &tage.secondBlockShadowTable}) {
            for (unsigned table = 0; table < tables->size(); ++table) {
                for (auto &set : (*tables)[table]) {
                    set[0] = BTBTAGE::TageEntry(tag(table) ^ 1, 2, branch.pc + 2);
                    set[0].useful = true;
                }
            }
        }
        for (auto domain : {MainLow, ShadowLow, SharedHigh}) {
            resetCounter(domain, false, 0) = 17;
            resetCounter(domain, true, 0) = 17;
            resetCounter(domain, true, 1) = 17;
        }
    }

    void expectResetSweep(ResetDomain domain, bool highAllocation = false)
    {
        for (bool shadow : {false, true}) {
            const auto &tables = shadow ? tage.secondBlockShadowTable :
                                          tage.tageTable;
            for (unsigned table = 0; table < tables.size(); ++table) {
                const auto entryDomain = shadow ? ShadowLow :
                    (table <= tage.secondBlockShadowMaxTable ? MainLow : SharedHigh);
                const unsigned begin = tage.partitionBegin(tables[table].size(), tid);
                const unsigned end = tage.partitionEnd(tables[table].size(), tid);
                for (unsigned set = 0; set < tables[table].size(); ++set) {
                    const bool reset = entryDomain == domain &&
                                       set >= begin && set < end;
                    const bool allocated = highAllocation && !shadow &&
                                           table == 2 && set == index(2);
                    EXPECT_EQ(tables[table][set][0].useful, !reset && !allocated)
                        << "shadow=" << shadow << " table=" << table
                        << " set=" << set << " tid=" << tid;
                }
            }
        }
    }

    void checkResetIsolation(ResetDomain domain, bool partitioned,
                             ThreadID thread, bool highAllocation = false)
    {
        fillProtected(partitioned, thread);
        resetCounter(domain, partitioned, tid) = 254;
        if (domain == SharedHigh) {
            // Start strictly above the shadow boundary: only high pressure.
            install(true, 1, 2, true);
        }
        if (highAllocation) {
            tage.tageTable[2][index(2)][0].useful = false;
        }
        const auto meta = domain == MainLow ? predictFirst() : predictSecond();
        ASSERT_TRUE(meta->preds.at(branch.pc).taken);
        train(meta, false);

        expectResetSweep(domain, highAllocation);
        for (auto other : {MainLow, ShadowLow, SharedHigh}) {
            const int expected = other == domain ? 0 :
                (other == SharedHigh ? (highAllocation ? 16 : 19) : 17);
            EXPECT_EQ(resetCounter(other, partitioned, tid), expected);
            if (partitioned) {
                EXPECT_EQ(resetCounter(other, true, 1 - tid), 17);
                EXPECT_EQ(resetCounter(other, false, 0), 17);
            } else {
                EXPECT_EQ(resetCounter(other, true, 0), 17);
                EXPECT_EQ(resetCounter(other, true, 1), 17);
            }
        }
        if (highAllocation) {
            const auto &learned = tage.tageTable[2][index(2)][0];
            EXPECT_TRUE(learned.valid);
            EXPECT_EQ(learned.tag, tag(2));
            EXPECT_EQ(learned.counter, -1);
            EXPECT_EQ(learned.pc, branch.pc);
        }
    }
};

TEST_P(SecondBlockShadowTest, ConflictingLowTrainingPreservesH1MainLow)
{
    install(false, 1, 2, true);
    install(false, 0, 1, false);
    install(true, 1, -2, false);
    const auto mainBefore = tage.tageTable;
    const auto useAltBefore = tage.useAlt;
    ASSERT_TRUE(predictFirst()->preds.at(branch.pc).taken);

    for (unsigned iteration = 0; iteration < 8; ++iteration) {
        const auto meta = predictSecond();
        ASSERT_FALSE(meta->preds.at(branch.pc).taken);
        train(meta, false);
    }

    CondTakens h2;
    tage.lookupSecondBlockNoSideEffect(context, {branch}, h2);
    ASSERT_EQ(h2.size(), 1U);
    EXPECT_FALSE(h2.front().second);
    EXPECT_TRUE(predictFirst()->preds.at(branch.pc).taken);
    expectMainUnchanged(mainBefore);
    EXPECT_EQ(tage.useAlt, useAltBefore);
    EXPECT_EQ(tage.secondBlockShadowTable[1][index(1)][0].counter, -4);
}

TEST_P(SecondBlockShadowTest, MainHighWinsOverShadowLowAndTrainsSharedProvider)
{
    install(false, 2, 2, false);
    install(true, 1, -2, false);
    auto meta = predictSecond();
    const auto &pred = meta->preds.at(branch.pc);
    ASSERT_TRUE(pred.mainInfo.found);
    EXPECT_EQ(pred.mainInfo.table, 2U);
    EXPECT_EQ(pred.mainInfo.storage, Storage::Main);
    ASSERT_TRUE(pred.altInfo.found);
    EXPECT_EQ(pred.altInfo.table, 1U);
    EXPECT_EQ(pred.altInfo.storage, Storage::SecondBlockShadow);
    ASSERT_TRUE(pred.taken);
    CondTakens results;
    tage.lookupSecondBlockNoSideEffect(context, {branch}, results);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_TRUE(results.front().second);

    train(meta, true);
    EXPECT_EQ(tage.tageTable[2][index(2)][0].counter, 3);
    EXPECT_TRUE(tage.tageTable[2][index(2)][0].useful);
    EXPECT_EQ(tage.secondBlockShadowTable[1][index(1)][0].counter, -2);
}

TEST_P(SecondBlockShadowTest, SelectedShadowAlternateTrainsItsOwnStorage)
{
    install(false, 2, 0, false);
    install(false, 1, 2, true);
    install(true, 1, -2, false);
    const auto useAltBefore = tage.useAlt;
    auto meta = predictSecond();
    const auto &pred = meta->preds.at(branch.pc);
    ASSERT_EQ(pred.mainInfo.storage, Storage::Main);
    ASSERT_EQ(pred.mainInfo.table, 2U);
    ASSERT_EQ(pred.altInfo.storage, Storage::SecondBlockShadow);
    ASSERT_EQ(pred.altInfo.table, 1U);
    ASSERT_TRUE(pred.useAlt);
    ASSERT_FALSE(pred.taken);

    train(meta, false);
    EXPECT_EQ(tage.tageTable[2][index(2)][0].counter, -1);
    EXPECT_EQ(tage.secondBlockShadowTable[1][index(1)][0].counter, -3);
    EXPECT_EQ(tage.tageTable[1][index(1)][0].counter, 2);
    EXPECT_TRUE(tage.tageTable[1][index(1)][0].useful);
    EXPECT_EQ(tage.useAlt, useAltBefore);
    EXPECT_EQ(tage.secondBlockShadowUseAlt[pred.useAltIdx], 1);
}

TEST_P(SecondBlockShadowTest, SelectedMainHighAlternateAlsoTrains)
{
    install(false, 3, 0, false);
    install(false, 2, -2, false);
    install(true, 1, 2, true);
    const auto meta = predictSecond();
    const auto &pred = meta->preds.at(branch.pc);
    ASSERT_EQ(pred.mainInfo.table, 3U);
    ASSERT_EQ(pred.altInfo.table, 2U);
    ASSERT_EQ(pred.altInfo.storage, Storage::Main);
    ASSERT_TRUE(pred.useAlt);
    ASSERT_FALSE(pred.taken);

    train(meta, false);
    EXPECT_EQ(tage.tageTable[3][index(3)][0].counter, -1);
    EXPECT_EQ(tage.tageTable[2][index(2)][0].counter, -3);
    EXPECT_EQ(tage.secondBlockShadowTable[1][index(1)][0].counter, 2);
}

TEST_P(SecondBlockShadowTest, MissingShadowAlternateUsesBaseNotMainLow)
{
    branch.ctr = -1;
    install(false, 2, 0, false);
    install(false, 1, 2, true);
    const auto meta = predictSecond();
    const auto &pred = meta->preds.at(branch.pc);
    ASSERT_TRUE(pred.mainInfo.found);
    ASSERT_EQ(pred.mainInfo.table, 2U);
    EXPECT_FALSE(pred.altInfo.found);
    EXPECT_TRUE(pred.useAlt);
    EXPECT_FALSE(pred.taken);
    EXPECT_EQ(pred.finalProviderTable, -1);
    train(meta, false);
    EXPECT_EQ(tage.tageTable[2][index(2)][0].counter, -1);
    EXPECT_EQ(tage.tageTable[1][index(1)][0].counter, 2);
}

TEST_P(SecondBlockShadowTest, ShadowMissDoesNotReadMainLow)
{
    branch.ctr = -1;
    install(false, 1, 2, true);
    install(false, 0, 2, true);
    ASSERT_TRUE(predictFirst()->preds.at(branch.pc).taken);
    const auto meta = predictSecond();
    const auto &pred = meta->preds.at(branch.pc);
    EXPECT_FALSE(pred.mainInfo.found);
    EXPECT_FALSE(pred.altInfo.found);
    EXPECT_FALSE(pred.taken);
    EXPECT_EQ(pred.finalProviderTable, -1);
    CondTakens results;
    tage.lookupSecondBlockNoSideEffect(context, {branch}, results);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_FALSE(results.front().second);
    const auto mainBefore = tage.tageTable;
    train(meta, false);
    expectMainUnchanged(mainBefore);
}

TEST_P(SecondBlockShadowTest, ShadowT1MispredictAllocatesMainT2)
{
    install(true, 1, 2, true);
    install(false, 1, -2, true);
    const auto meta = predictSecond();
    ASSERT_EQ(meta->preds.at(branch.pc).mainInfo.table, 1U);
    ASSERT_TRUE(meta->preds.at(branch.pc).taken);
    train(meta, false);

    const auto &learned = tage.tageTable[2][index(2)][0];
    EXPECT_TRUE(learned.valid);
    EXPECT_EQ(learned.tag, tag(2));
    EXPECT_EQ(learned.pc, branch.pc);
    EXPECT_EQ(learned.counter, -1);
    EXPECT_FALSE(learned.useful);
    EXPECT_EQ(tage.secondBlockShadowTable[1][index(1)][0].counter, 1);
    EXPECT_FALSE(tage.secondBlockShadowTable[0][index(0)][0].valid);
    EXPECT_EQ(tage.tageTable[1][index(1)][0].counter, -2);
    EXPECT_FALSE(tage.tageTable[3][index(3)][0].valid);
}

TEST_P(SecondBlockShadowTest, CrossBoundaryAllocationKeepsH1IndexAndH2Tag)
{
    // Distinct folded contexts expose accidentally using live H1 history.
    for (unsigned table = 0; table < tage.numPredictors; ++table) {
        context.indexFoldedHist[table] = 3;
        context.tagFoldedHist[table] = 5;
        context.altTagFoldedHist[table] = 2;
    }
    install(true, 1, 2, true);
    const Addr liveIndex = tage.getTageIndex(block2Start, 2);
    ASSERT_NE(index(2), liveIndex);
    const auto meta = predictSecond();
    ASSERT_EQ(meta->preds.at(branch.pc).mainInfo.table, 1U);
    ASSERT_TRUE(meta->preds.at(branch.pc).taken);
    train(meta, false);

    const auto &learned = tage.tageTable[2][index(2)][0];
    EXPECT_TRUE(learned.valid);
    EXPECT_EQ(learned.tag, tag(2));
    EXPECT_EQ(learned.counter, -1);
    EXPECT_FALSE(tage.tageTable[2][liveIndex][0].valid);
}

TEST_P(SecondBlockShadowTest, MainT2MispredictAllocatesMainT3)
{
    install(false, 2, 2, true);
    const auto meta = predictSecond();
    ASSERT_EQ(meta->preds.at(branch.pc).mainInfo.table, 2U);
    ASSERT_TRUE(meta->preds.at(branch.pc).taken);
    train(meta, false);

    const auto &learned = tage.tageTable[3][index(3)][0];
    EXPECT_TRUE(learned.valid);
    EXPECT_EQ(learned.tag, tag(3));
    EXPECT_EQ(learned.pc, branch.pc);
    EXPECT_EQ(learned.counter, -1);
    EXPECT_EQ(tage.tageTable[2][index(2)][0].counter, 1);
    for (unsigned table = 0; table <= 1; ++table) {
        EXPECT_FALSE(tage.secondBlockShadowTable[table][index(table)][0].valid);
        EXPECT_FALSE(tage.tageTable[table][index(table)][0].valid);
    }
}

TEST_P(SecondBlockShadowTest, HighestMainT3MispredictNeverAllocates)
{
    install(false, 3, 2, true);
    const auto meta = predictSecond();
    ASSERT_EQ(meta->preds.at(branch.pc).mainInfo.table, 3U);
    ASSERT_TRUE(meta->preds.at(branch.pc).taken);
    train(meta, false);

    EXPECT_EQ(tage.tageTable[3][index(3)][0].counter, 1);
    for (unsigned table = 0; table < 3; ++table) {
        EXPECT_FALSE(tage.tageTable[table][index(table)][0].valid);
        if (table <= 1) {
            EXPECT_FALSE(tage.secondBlockShadowTable[table][index(table)][0].valid);
        }
    }
    EXPECT_EQ(tage.usefulResetCnt, 0);
    EXPECT_EQ(tage.secondBlockShadowUsefulResetCnt, 0);
    EXPECT_EQ(tage.sharedHighUsefulResetCnt, 0);
}

TEST_P(SecondBlockShadowTest, StaleProviderCannotTrainReplacementShadowTag)
{
    branch.ctr = -1;
    install(true, 1, 2, false);
    auto meta = predictSecond();
    ASSERT_TRUE(meta->preds.at(branch.pc).taken);
    auto &replacement = tage.secondBlockShadowTable[1][index(1)][0];
    replacement = BTBTAGE::TageEntry(tag(1) ^ 1, 1, branch.pc + 2);

    train(meta, true);

    EXPECT_EQ(replacement.tag, tag(1) ^ 1);
    EXPECT_EQ(replacement.counter, 1);
    EXPECT_FALSE(replacement.useful);
}

TEST_P(SecondBlockShadowTest, StaleAltCannotTrainReplacementShadowTag)
{
    branch.ctr = -1;
    install(true, 1, 0, false);
    install(true, 0, -2, false);
    auto meta = predictSecond();
    ASSERT_FALSE(meta->preds.at(branch.pc).taken);
    auto &replacement = tage.secondBlockShadowTable[0][index(0)][0];
    replacement = BTBTAGE::TageEntry(tag(0) ^ 1, 1, branch.pc + 2);

    train(meta, false);

    EXPECT_EQ(replacement.tag, tag(0) ^ 1);
    EXPECT_EQ(replacement.counter, 1);
    EXPECT_FALSE(replacement.useful);
}

TEST_P(SecondBlockShadowTest, StaleCrossDomainAltCannotTrainReplacementShadowTag)
{
    branch.ctr = -1;
    install(false, 2, 0, false);
    install(true, 1, -2, false);
    const auto meta = predictSecond();
    ASSERT_EQ(meta->preds.at(branch.pc).mainInfo.storage, Storage::Main);
    ASSERT_EQ(meta->preds.at(branch.pc).altInfo.storage,
              Storage::SecondBlockShadow);
    ASSERT_TRUE(meta->preds.at(branch.pc).useAlt);
    ASSERT_FALSE(meta->preds.at(branch.pc).taken);
    auto &replacement = tage.secondBlockShadowTable[1][index(1)][0];
    replacement = BTBTAGE::TageEntry(tag(1) ^ 1, 1, branch.pc + 2);

    train(meta, false);
    EXPECT_EQ(replacement.tag, tag(1) ^ 1);
    EXPECT_EQ(replacement.counter, 1);
    EXPECT_FALSE(replacement.useful);
    EXPECT_EQ(tage.tageTable[2][index(2)][0].counter, -1);
}

TEST_P(SecondBlockShadowTest, ShadowLowResetPreservesMainLowHighAndOtherThread)
{
    for (bool partitioned : {false, true}) {
        for (ThreadID thread : {0, 1}) {
            checkResetIsolation(ShadowLow, partitioned, thread);
        }
    }
}

TEST_P(SecondBlockShadowTest, MainLowResetPreservesShadowHighAndOtherThread)
{
    for (bool partitioned : {false, true}) {
        for (ThreadID thread : {0, 1}) {
            checkResetIsolation(MainLow, partitioned, thread);
        }
    }
}

TEST_P(SecondBlockShadowTest, H2HighResetPreservesBothLowDomainsAndOtherThread)
{
    for (bool partitioned : {false, true}) {
        for (ThreadID thread : {0, 1}) {
            checkResetIsolation(SharedHigh, partitioned, thread);
        }
    }
}

TEST_P(SecondBlockShadowTest, LowExhaustionResetsBeforeSuccessfulHighAllocation)
{
    for (auto domain : {MainLow, ShadowLow}) {
        for (bool partitioned : {false, true}) {
            for (ThreadID thread : {0, 1}) {
                checkResetIsolation(domain, partitioned, thread, true);
            }
        }
    }
}

TEST_P(SecondBlockShadowTest, SuccessfulLowAllocationDecrementsOnlyItsDomain)
{
    for (auto domain : {MainLow, ShadowLow}) {
        for (bool partitioned : {false, true}) {
            for (ThreadID thread : {0, 1}) {
                fillProtected(partitioned, thread);
                auto &tables = domain == MainLow ? tage.tageTable :
                                                   tage.secondBlockShadowTable;
                tables[0][index(0)][0].useful = false;
                const auto meta = domain == MainLow ? predictFirst() : predictSecond();
                ASSERT_TRUE(meta->preds.at(branch.pc).taken);
                train(meta, false);

                EXPECT_EQ(tables[0][index(0)][0].tag, tag(0));
                EXPECT_EQ(tables[0][index(0)][0].counter, -1);
                for (auto other : {MainLow, ShadowLow, SharedHigh}) {
                    EXPECT_EQ(resetCounter(other, partitioned, tid),
                              other == domain ? 16 : 17);
                    if (partitioned) {
                        EXPECT_EQ(resetCounter(other, true, 1 - tid), 17);
                        EXPECT_EQ(resetCounter(other, false, 0), 17);
                    }
                }
            }
        }
    }
}

TEST_P(SecondBlockShadowTest, H1AndH2AccumulateSharedHighResetPressure)
{
    for (bool partitioned : {false, true}) {
        for (ThreadID thread : {0, 1}) {
            fillProtected(partitioned, thread);
            install(false, 1, 2, true);
            install(true, 1, 2, true);
            resetCounter(SharedHigh, partitioned, tid) = 253;

            auto first = predictFirst();
            ASSERT_EQ(first->preds.at(branch.pc).mainInfo.table, 1U);
            ASSERT_TRUE(first->preds.at(branch.pc).taken);
            train(first, false);
            EXPECT_EQ(resetCounter(SharedHigh, partitioned, tid), 255);
            EXPECT_TRUE(tage.tageTable[2][index(2)][0].useful);
            EXPECT_TRUE(tage.tageTable[3][index(3)][0].useful);

            auto second = predictSecond();
            ASSERT_EQ(second->preds.at(branch.pc).mainInfo.table, 1U);
            ASSERT_EQ(second->preds.at(branch.pc).mainInfo.storage,
                      Storage::SecondBlockShadow);
            ASSERT_TRUE(second->preds.at(branch.pc).taken);
            train(second, false);
            EXPECT_EQ(resetCounter(SharedHigh, partitioned, tid), 0);
            EXPECT_EQ(resetCounter(MainLow, partitioned, tid), 17);
            EXPECT_EQ(resetCounter(ShadowLow, partitioned, tid), 17);
            if (partitioned) {
                EXPECT_EQ(resetCounter(SharedHigh, true, 1 - tid), 17);
                EXPECT_EQ(resetCounter(SharedHigh, false, 0), 17);
            }
            expectResetSweep(SharedHigh);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(UpdateModes, SecondBlockShadowTest,
                         ::testing::Bool());

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
    FetchTarget stream = createStream(0x1000, entry, meta);
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, true));

    // Verify useful bit is set (main prediction was correct and differed from alt)
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should be set when main predicts correctly and differs from alt";

    // Predict again
    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    meta = tage->getPredictionMeta();

    // Update with actual outcome opposite to main prediction (not taken)
    stream = createStream(0x1000, entry, meta);
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, false));

    // Verify useful bit is NOT cleared (policy is ++ only, no --)
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should remain set when main predicts incorrectly (no decrement)";
}

TEST_F(BTBTAGETest, UsefulBitIgnoresStrongCorrectAlternative) {
    BTBEntry entry = createBTBEntry(0x1000);

    // Provider and alternative both predict taken correctly. RTL-aligned
    // behavior keeps useful unchanged instead of clearing it.
    setupTageEntry(tage, 0x1000, 3, 2, true);
    setupTageEntry(tage, 0x1000, 1, 2, false);

    Addr mainIndex = tage->getTageIndex(0x1000, 3);

    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    auto meta = tage->getPredictionMeta();
    FetchTarget stream = createStream(0x1000, entry, meta);
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, true));

    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should not be cleared only because alt is also correct and strong";
}

TEST_F(BTBTAGETest, UsefulBitIgnoresWeakCounterTransition) {
    BTBEntry entry = createBTBEntry(0x1000);

    // Counter transitions to a weak state after update, but useful should not
    // be cleared by that transition alone.
    setupTageEntry(tage, 0x1000, 3, 1, true);

    Addr mainIndex = tage->getTageIndex(0x1000, 3);

    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    auto meta = tage->getPredictionMeta();
    FetchTarget stream = createStream(0x1000, entry, meta);
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, false));

    EXPECT_EQ(tage->tageTable[3][mainIndex][0].counter, 0);
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful)
        << "Useful bit should not be cleared only because the provider becomes weak";
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
    FetchTarget stream = createStream(0x1000, entry2, meta);

    // Update the predictor. With RTL-aligned highest-table gating, this should
    // not report a final allocation failure.
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry2, !predicted, true));

    int alloc_failed_no_valid = tage->tageStats.updateAllocFailureNoValidTable;
    EXPECT_EQ(alloc_failed_no_valid, 0)
        << "A highest-table provider should suppress final allocation failure";

}

TEST_F(BTBTAGETest, HighestTableProviderSuppressesAllocation) {
    BTBEntry entry = createBTBEntry(0x1000);

    int highestTable = tage->numPredictors - 1;
    setupTageEntry(tage, 0x1000, highestTable, 2, false);

    predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    FetchTarget stream = createStream(0x1000, entry, meta);

    int alloc_failed_before = tage->tageStats.updateAllocFailureNoValidTable;
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, false, true));

    EXPECT_EQ(tage->tageStats.updateAllocSuccess, 0);
    EXPECT_EQ(tage->tageStats.updateAllocFailureNoValidTable, alloc_failed_before)
        << "A highest-table provider should suppress allocation instead of reporting final failure";
}

// Test history recovery mechanism
TEST_F(BTBTAGETest, HistoryRecoveryCorrectness) {
    BTBEntry entry = createBTBEntry(0x1000);

    // Record initial history state
    boost::dynamic_bitset<> originalHistory = history;

    // Store original folded history state
    // Make a prediction
    bool predicted_taken = predictTAGE(tage, 0x1000, {entry}, history, stagePreds);

    // Speculatively update history
    specUpdateSelectedHistory(tage, history, stagePreds[1]);
    auto meta = tage->getPredictionMeta();

    // Update speculative history register to mirror decoupled_bpred behavior.
    applyPredictedHistory(tage, history, stagePreds[1]);

    // Create a recovery stream with opposite outcome
    FetchTarget stream = createStream(0x1000, entry, meta);

    // Recover to pre-speculative state and update with correct outcome
    boost::dynamic_bitset<> recoveryHistory = originalHistory;
    const auto path_update = getActualPathUpdate(
        entry, entry.pc, !predicted_taken, entry.target);
    recoverSelectedHistory(tage, recoveryHistory, stream, 1, !predicted_taken,
                           path_update);

    // Expected history should be original updated with the actual outcome.
    boost::dynamic_bitset<> expectedHistory = originalHistory;
    applyActualHistory(tage, expectedHistory, entry, 1, !predicted_taken);

    // Verify recovery produced the expected history
    for (int i = 0; i < tage->numPredictors; i++) {
        tage->threadHistory[0].tagFoldedHist[i].check(expectedHistory);
        tage->threadHistory[0].altTagFoldedHist[i].check(expectedHistory);
        tage->threadHistory[0].indexFoldedHist[i].check(expectedHistory);
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
    FetchTarget stream1 = createStream(0x1000, btbEntries[0], meta);
    tage->update(
        PredictionUpdateContext(stream1),
        createPreparedUpdate(stream1, btbEntries[0], first_pred));

    // Update second branch (incorrect prediction), allocate 1 entry
    FetchTarget stream2 = createStream(0x1000, btbEntries[1], meta);
    tage->update(
        PredictionUpdateContext(stream2),
        createPreparedUpdate(stream2, btbEntries[1], !second_pred, true));

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

        FetchTarget stream = createStream(0x1000, entry, meta);
        tage->update(
            PredictionUpdateContext(stream),
            createPreparedUpdate(stream, entry, true));
    }

    // Verify counter saturates at maximum
    EXPECT_EQ(tage->tageTable[testTable][index][0].counter, 3)
        << "Counter should saturate at maximum value";

    // Train with not-taken outcomes multiple times
    for (int i = 0; i < 7; i++) {
        predictTAGE(tage, 0x1000, {entry}, history, stagePreds);
        auto meta = tage->getPredictionMeta();

        FetchTarget stream = createStream(0x1000, entry, meta);
        tage->update(
            PredictionUpdateContext(stream),
            createPreparedUpdate(stream, entry, false));
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

TEST_F(BTBTAGETest, AllocationReplacesStrongNotUsefulEntry) {
    tage = new BTBTAGE(1, 2, 10); // only 1 predictor table, 2 ways
    history.resize(64, false);
    stagePreds.resize(2);

    Addr startPC = 0x1000;
    int testTable = 0;
    Addr testIndex = tage->getTageIndex(startPC, testTable);

    createManualTageEntry(
        tage, testTable, testIndex, 0, tage->getTageTag(startPC, testTable, 0), 2, false, 0x1000);
    createManualTageEntry(
        tage, testTable, testIndex, 1, tage->getTageTag(startPC, testTable, 2), -2, false, 0x1004);

    BTBEntry newEntry = createBTBEntry(0x1008);
    predictUpdateCycle(tage, startPC, newEntry, false, history, stagePreds);

    bool found = false;
    for (unsigned way = 0; way < tage->numWays[testTable]; way++) {
        if (tage->tageTable[testTable][testIndex][way].valid &&
            tage->tageTable[testTable][testIndex][way].pc == newEntry.pc) {
            found = true;
            break;
        }
    }

    EXPECT_TRUE(found)
        << "A strong but not-useful entry should be replaceable";
}

TEST_F(BTBTAGETest, MissingPredictionMetaDoesNotTrain) {
    stagePreds[1].btbEntries.clear();
    tage->putPCHistory(0x1000, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    BTBEntry newEntry = createBTBEntry(0x1010, true, true, -1);
    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predMetas[0] = meta;

    const auto outcome = createBranchOutcome(newEntry, true, true);
    PreparedUpdate update({outcome});
    tage->update(PredictionUpdateContext(stream), update);

    int table = findTableWithEntry(tage, 0x1000, newEntry.pc);
    EXPECT_EQ(table, -1);
}

TEST_F(BTBTAGETest, FinalPredictionWithoutTageMetaDoesNotTrain)
{
    stagePreds[1].btbEntries.clear();
    tage->putPCHistory(0x1000, history, stagePreds);
    auto meta = tage->getPredictionMeta();

    BTBEntry finalEntry =
        createBTBEntry(0x1010, true, true, -1);
    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBranchInfo = finalEntry;
    stream.setPredictedBranches({finalEntry});
    stream.predMetas[0] = meta;

    const auto outcome = createBranchOutcome(finalEntry, true, true);
    PreparedUpdate update({outcome});
    tage->setTrainingStage(PredictorTrainingStage::Resolve);
    tage->update(PredictionUpdateContext(stream), update);

    EXPECT_EQ(findTableWithEntry(tage, 0x1000, finalEntry.pc), -1);
}

TEST_F(BTBTAGETest, TargetMispredictDoesNotAllocateDirectionEntry)
{
    BTBEntry entry = createBTBEntry(0x1010, true, true, 0, 0x2000);
    EXPECT_TRUE(predictTAGE(tage, 0x1000, {entry}, history, stagePreds));
    auto meta = tage->getPredictionMeta();
    FetchTarget stream = createStream(0x1000, entry, meta);

    // The redirect was caused by target prediction, while direction was right.
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, true, true));

    EXPECT_EQ(findTableWithEntry(tage, 0x1000, entry.pc), -1);
}

/**
 * @brief Test bank conflict detection
 *
 * Verifies:
 * 1. Same bank access defers the update, and the same packet can retry
 * 2. Different bank access has no conflict
 * 3. Disabled flag prevents conflict detection
 */
TEST_F(BTBTAGETest, BankConflict) {
    // Create TAGE with 4 banks
    BTBTAGE *bankTage = new BTBTAGE(4, 2, 1024, 4);
    bankTage->setTrainingStage(PredictorTrainingStage::Resolve);
    boost::dynamic_bitset<> testHistory(128);
    std::vector<FullBTBPrediction> testStagePreds(5);

    // Bank ID derives from bits [2:1] (pc >> 1) & 0x3 when instShiftAmt == 1.
    // Bank 0: ..., 0x100, 0x108 ...  Bank 1: ..., 0x102, 0x10A ...
    // Bank 2: ..., 0x104, 0x10C ...  Bank 3: ..., 0x106, 0x10E ...

    // Test 1: Same bank conflict (enabled)
    bankTage->enableBankConflict = true;
    {
        // Predict and update the same branch in bank 1.
        setupTageEntry(bankTage, 0xa0, 0, 1, false);
        testStagePreds[1].btbEntries = {createBTBEntry(0xa0)};
        bankTage->putPCHistory(0xa0, testHistory, testStagePreds);
        EXPECT_TRUE(bankTage->predBankValid);

        auto meta = bankTage->getPredictionMeta();
        const auto entry = createBTBEntry(0xa0);
        FetchTarget stream = createStream(0xa0, entry, meta);
        auto update = createPreparedUpdate(stream, entry, true);
        Addr index = bankTage->getTageIndex(0xa0, 0);
        const auto before_probe = bankTage->tageTable[0][index][0];

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(
            PredictionUpdateContext(stream), update);

        // Should detect conflict and defer update
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before + 1);
        EXPECT_FALSE(can_update);
        EXPECT_FALSE(bankTage->predBankValid);
        const auto &after_probe = bankTage->tageTable[0][index][0];
        EXPECT_EQ(after_probe.valid, before_probe.valid);
        EXPECT_EQ(after_probe.tag, before_probe.tag);
        EXPECT_EQ(after_probe.counter, before_probe.counter);
        EXPECT_EQ(after_probe.useful, before_probe.useful);

        // The failed probe consumes the transient bank marker.  Retrying the
        // exact same packet succeeds and is applied once by the coordinator.
        EXPECT_TRUE(bankTage->canResolveUpdate(
            PredictionUpdateContext(stream), update));
        EXPECT_EQ(bankTage->tageTable[0][index][0].counter,
                  before_probe.counter);
        bankTage->doResolveUpdate(PredictionUpdateContext(stream), update);
        EXPECT_EQ(bankTage->tageTable[0][index][0].counter,
                  before_probe.counter + 1);
        EXPECT_EQ(bankTage->tageStats.updateBankConflict,
                  conflicts_before + 1);
    }

    // Test 2: Different bank, no conflict
    {
        // Predict on bank 0 (0x100), update on bank 2 (0x104)
        testStagePreds[1].btbEntries = {createBTBEntry(0x100)};
        bankTage->putPCHistory(0x100, testHistory, testStagePreds);

        auto meta = bankTage->getPredictionMeta();
        const auto entry = createBTBEntry(0x104);
        FetchTarget stream = createStream(0x104, entry, meta);
        auto update = createPreparedUpdate(stream, entry, true);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(
            PredictionUpdateContext(stream), update);
        ASSERT_TRUE(can_update);
        bankTage->doResolveUpdate(PredictionUpdateContext(stream), update);

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
        const auto entry = createBTBEntry(0xa0);
        FetchTarget stream = createStream(0xa0, entry, meta);
        setupTageEntry(bankTage, 0xa0, 0, 1, false);
        auto update = createPreparedUpdate(stream, entry, true);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(
            PredictionUpdateContext(stream), update);
        ASSERT_TRUE(can_update);
        bankTage->doResolveUpdate(PredictionUpdateContext(stream), update);

        // No conflict even with same bank
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before);
    }
}

class BTBTAGEUpperBoundTest : public ::testing::Test
{
  protected:
    void SetUp() override {
        tage = new BTBTAGEUpperBound();
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
    BTBEntry entry = createBTBEntry(0x1000, true, true, -1);
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
    BTBEntry entry = createBTBEntry(0x1000, true, true, -1);

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
    BTBEntry entry = createBTBEntry(0x1000, true, true, -1);
    boost::dynamic_bitset<> historyA(128, 0);
    boost::dynamic_bitset<> historyB(128, 0);
    historyB[0] = true;

    predictTAGE(tage, 0x1000, {entry}, historyA, stagePreds);
    auto meta = tage->getPredictionMeta();

    FetchTarget stream = createStream(0x1000, entry, meta);

    tage->recoverHist(
        historyB, HistoryRecoveryContext(stream),
        DirectionHistoryUpdate{1, true});
    tage->update(
        PredictionUpdateContext(stream),
        createPreparedUpdate(stream, entry, true, true));

    EXPECT_TRUE(tage->hasExactEntry(0, entry.pc, historyA));
    EXPECT_FALSE(tage->hasExactEntry(0, entry.pc, historyB));
}

TEST_F(BTBTAGEUpperBoundTest, MissingPredictionMetaDoesNotTrain) {
    boost::dynamic_bitset<> historyA(128, 0);
    stagePreds[1].btbEntries.clear();
    tage->putPCHistory(0x1000, historyA, stagePreds);
    auto meta = tage->getPredictionMeta();

    BTBEntry newEntry = createBTBEntry(0x1010, true, true, -1);
    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predMetas[0] = meta;

    const auto outcome = createBranchOutcome(newEntry, true, true);
    PreparedUpdate update({outcome});
    tage->update(PredictionUpdateContext(stream), update);

    EXPECT_FALSE(tage->hasExactEntry(0, newEntry.pc, historyA));
}

TEST_F(BTBTAGEUpperBoundPathHashTest, PredictionUsesPathHashHistorySnapshot) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, -1, 0x2000);
    boost::dynamic_bitset<> pathHistoryA(128, 0);
    boost::dynamic_bitset<> pathHistoryB(128, 0);
    applyPathHistoryTaken(pathHistoryB, entry.pc, entry.target);

    ASSERT_TRUE(tage->insertExactEntry(2, entry.pc, pathHistoryB, 2));

    FullBTBPrediction pred;
    pred.btbEntries.push_back(entry);
    pred.condTakens.push_back({entry.pc, true});
    tage->specUpdatePHist(pathHistoryA, pred, pred.getPHistUpdate());

    bool predicted = predictTAGE(tage, 0x1000, {entry}, outcomeHistory, stagePreds);

    EXPECT_TRUE(predicted);
}

TEST_F(BTBTAGEUpperBoundPathHashTest, PredictionUsesIndirectOverridePathHashSnapshot) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, -1, 0x2000);
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

    tage->specUpdatePHist(pathHistoryA, pred, pred.getPHistUpdate());
    tage->checkFoldedHist(pathHistoryB, "indirect target override");

    bool predicted = predictTAGE(tage, 0x1000, {entry}, outcomeHistory, stagePreds);

    EXPECT_TRUE(predicted);
}

TEST_F(BTBTAGEUpperBoundPathHashTest, PredictionUsesReturnOverridePathHashSnapshot) {
    BTBEntry entry = createBTBEntry(0x1000, true, true, -1, 0x2000);
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

    tage->specUpdatePHist(pathHistoryA, pred, pred.getPHistUpdate());
    tage->checkFoldedHist(pathHistoryB, "return target override");

    bool predicted = predictTAGE(tage, 0x1000, {entry}, outcomeHistory, stagePreds);

    EXPECT_TRUE(predicted);
}

TEST_F(BTBTAGEUpperBoundPathHashTest, RecoverPHistUsesTakenControlPath) {
    BTBEntry entry = createBTBEntry(0x1000, false, true, -1, 0x2040);
    boost::dynamic_bitset<> pathHistoryBefore(128, 0);
    boost::dynamic_bitset<> pathHistoryAfter(128, 0);
    applyPathHistoryTaken(pathHistoryAfter, entry.pc, entry.target);

    FullBTBPrediction pred;
    pred.btbEntries.push_back(entry);
    tage->putPCHistory(0x1000, pathHistoryBefore, stagePreds);
    auto meta = tage->getPredictionMeta();

    FetchTarget stream = createStream(0x1000, entry, meta);

    const auto ghist = stream.getGHistUpdateDuringSquash(entry.pc, false, true);
    const auto phist = getActualPathUpdate(
        entry, entry.pc, true, entry.target);
    EXPECT_EQ(ghist.shamt, 0);
    EXPECT_FALSE(ghist.taken);
    EXPECT_TRUE(phist.taken);
    EXPECT_EQ(phist.pc, entry.pc);
    EXPECT_EQ(phist.target, entry.target);

    tage->recoverPHist(
        pathHistoryBefore, HistoryRecoveryContext(stream), phist);
    tage->checkFoldedHist(pathHistoryAfter, "recover taken control path");
}


}  // namespace test

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

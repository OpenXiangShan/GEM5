#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

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

static BTBEntry
createBTBEntry(Addr pc, bool isCond = true, bool valid = true,
               bool alwaysTaken = false, int ctr = 0, Addr target = 0)
{
    BTBEntry entry;
    entry.pc = pc;
    entry.target = target ? target : (pc + 4);
    entry.isCond = isCond;
    entry.valid = valid;
    entry.alwaysTaken = alwaysTaken;
    entry.ctr = ctr;
    return entry;
}

static FetchTarget
createStream(Addr startPC,
             const std::vector<BTBEntry> &predEntries,
             const BTBEntry *actual_taken_entry,
             std::shared_ptr<void> meta)
{
    FetchTarget stream;
    stream.startPC = startPC;
    stream.predBTBEntries = predEntries;
    stream.updateBTBEntries = predEntries;
    stream.resolved = true;
    stream.updateIsOldEntry = true;
    stream.predMetas[0] = meta;

    if (actual_taken_entry) {
        stream.exeBranchInfo = *actual_taken_entry;
        stream.exeTaken = true;
        stream.squashType = SquashType::SQUASH_CTRL;
        stream.squashPC = actual_taken_entry->pc;
    } else {
        stream.exeTaken = false;
        stream.exeBranchInfo = BranchInfo();
        stream.squashType = SquashType::SQUASH_NONE;
        stream.squashPC = 0;
    }
    return stream;
}

static void
applyPathHistoryTaken(boost::dynamic_bitset<> &history, Addr pc, Addr target,
                      int shamt = 2)
{
    history <<= shamt;
    uint64_t hash = pathHash(pc, target);
    for (std::size_t i = 0; i < pathHashLength && i < history.size(); ++i) {
        bool bit = history[i];
        history[i] = (hash & 1) ^ bit;
        hash >>= 1;
    }
}

static Addr
predictExitPC(BTBTAGE *tage, Addr startPC,
              const std::vector<BTBEntry> &entries,
              const boost::dynamic_bitset<> &history,
              std::vector<FullBTBPrediction> &stagePreds)
{
    stagePreds[1].btbEntries = entries;
    tage->putPCHistory(startPC, history, stagePreds);

    Addr pred_pc = 0;
    for (auto &e : entries) {
        if (!(e.valid && e.isCond)) {
            continue;
        }
        Addr branch_pc = e.pc;
        auto it = CondTakens_find(stagePreds[1].condTakens, branch_pc);
        if (it != stagePreds[1].condTakens.end() && it->second) {
            pred_pc = e.pc;
            break;
        }
    }
    return pred_pc;
}

static void
setupTageEntry(BTBTAGE *tage, Addr startPC, int table_idx,
               uint8_t conf, uint8_t exit0, uint8_t exit1 = 0, uint8_t sel = 0,
               bool useful = false, int way = 0)
{
    Addr index = tage->getTageIndex(startPC, table_idx);
    Addr tag = tage->getTageTag(startPC, table_idx);
    auto &entry = tage->tageTable[table_idx][index][way];
    entry.valid = true;
    entry.tag = tag;
    entry.conf = conf;
    entry.useful = useful;
    entry.exitSlotEnc0 = exit0;
    entry.exitSlotEnc1 = exit1;
    entry.selCtr = sel;
}

static int
findTableWithEntryWithMeta(BTBTAGE *tage, Addr startPC,
                           const std::shared_ptr<BTBTAGE::TageMeta> &meta)
{
    for (int t = 0; t < (int)tage->numPredictors; ++t) {
        Addr index = tage->getTageIndex(startPC, t, meta->indexFoldedHist[t].get());
        Addr tag = tage->getTageTag(startPC, t,
                                    meta->tagFoldedHist[t].get(),
                                    meta->altTagFoldedHist[t].get());
        for (int way = 0; way < (int)tage->numWays; ++way) {
            auto &entry = tage->tageTable[t][index][way];
            if (entry.valid && entry.tag == tag) {
                return t;
            }
        }
    }
    return -1;
}

static std::shared_ptr<BTBTAGE::TageMeta>
predictUpdateCycleBlock(BTBTAGE *tage, Addr startPC,
                        const std::vector<BTBEntry> &entries,
                        const BTBEntry *actual_taken_entry,
                        boost::dynamic_bitset<> &history,
                        std::vector<FullBTBPrediction> &stagePreds)
{
    stagePreds[1].btbEntries = entries;
    tage->putPCHistory(startPC, history, stagePreds);
    tage->specUpdateHist(history, stagePreds[1]);

    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());

    // Mirror pHistShiftIn behavior to keep history consistent in the test.
    auto [pred_pc, pred_target, pred_taken] = stagePreds[1].getPHistInfo();
    Addr phr_pc = pred_taken ? pred_pc : startPC;
    Addr phr_target = pred_taken ? pred_target : (startPC + tage->blockSize);
    applyPathHistoryTaken(history, phr_pc, phr_target);

    FetchTarget stream = createStream(startPC, entries, actual_taken_entry,
                                      std::static_pointer_cast<void>(meta));
    tage->update(stream);
    return meta;
}

class BTBTAGETest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tage = new BTBTAGE();
        std::memset(&tage->tageStats, 0, sizeof(BTBTAGE::TageStats));
        history.resize(64, false);
        stagePreds.resize(2);
    }

    BTBTAGE *tage;
    boost::dynamic_bitset<> history;
    std::vector<FullBTBPrediction> stagePreds;
};

TEST_F(BTBTAGETest, BasicPrediction)
{
    Addr startPC = 0x1000;
    BTBEntry b0 = createBTBEntry(0x1000, true, true, false, -1);
    BTBEntry b1 = createBTBEntry(0x1002, true, true, false, -1);
    std::vector<BTBEntry> entries = {b0, b1};

    setupTageEntry(tage, startPC, /*table*/ 3, /*conf*/ 2, /*exit0*/ 2);

    Addr pred_pc = predictExitPC(tage, startPC, entries, history, stagePreds);
    EXPECT_EQ(pred_pc, 0x1002);

    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    EXPECT_TRUE(meta->hasPred);
    EXPECT_EQ(meta->pred.predEnc, 2);
    EXPECT_EQ(meta->pred.predCondPC, 0x1002);
    EXPECT_EQ(meta->pred.source, BTBTAGE::PredSource::Provider);
}

TEST_F(BTBTAGETest, HistoryUpdate)
{
    Addr pc = 0x1000;
    Addr target = pc + 0x40;

    tage->doUpdateHist(history, true, pc, target);
    applyPathHistoryTaken(history, pc, target);
    tage->checkFoldedHist(history, "taken update");

    tage->doUpdateHist(history, false, pc, target);
    applyPathHistoryTaken(history, pc, target);
    tage->checkFoldedHist(history, "not-taken update");
}

TEST_F(BTBTAGETest, MainAltPredictionBehavior)
{
    Addr startPC = 0x1000;
    // Make base prefer slot0.
    BTBEntry b0 = createBTBEntry(0x1000, true, true, false, /*ctr*/ 1);
    BTBEntry b1 = createBTBEntry(0x1002, true, true, false, /*ctr*/ -1);
    std::vector<BTBEntry> entries = {b0, b1};

    // Provider predicts slot1.
    setupTageEntry(tage, startPC, 3, /*conf*/ 2, /*exit0*/ 2);

    Addr pred_pc = predictExitPC(tage, startPC, entries, history, stagePreds);
    EXPECT_EQ(pred_pc, 0x1002);

    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    EXPECT_EQ(meta->pred.source, BTBTAGE::PredSource::Provider);
    EXPECT_FALSE(meta->pred.useAlt);

    // Make provider weak => default useAltOnNa is >= 0, so choose Base (conservative).
    setupTageEntry(tage, startPC, 3, /*conf*/ 0, /*exit0*/ 2);
    pred_pc = predictExitPC(tage, startPC, entries, history, stagePreds);
    EXPECT_EQ(pred_pc, 0x1000);
    meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    EXPECT_TRUE(meta->pred.useAlt);
    EXPECT_EQ(meta->pred.source, BTBTAGE::PredSource::Base);

    // Disable useAltOnNa => weak provider should be used.
    Addr uidx = tage->getUseAltIdx(startPC);
    tage->useAlt[uidx] = -1;
    pred_pc = predictExitPC(tage, startPC, entries, history, stagePreds);
    EXPECT_EQ(pred_pc, 0x1002);
    meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    EXPECT_EQ(meta->pred.source, BTBTAGE::PredSource::Provider);
}

TEST_F(BTBTAGETest, UsefulBitMechanism)
{
    Addr startPC = 0x1000;
    // Base prefers slot0, but actual is slot1.
    BTBEntry b0 = createBTBEntry(0x1000, true, true, false, /*ctr*/ 1);
    BTBEntry b1 = createBTBEntry(0x1002, true, true, false, /*ctr*/ -1);
    std::vector<BTBEntry> entries = {b0, b1};

    setupTageEntry(tage, startPC, 3, /*conf*/ 2, /*exit0*/ 2, /*exit1*/ 0, /*sel*/ 0, /*useful*/ false);

    Addr mainIndex = tage->getTageIndex(startPC, 3);
    EXPECT_FALSE(tage->tageTable[3][mainIndex][0].useful);

    predictUpdateCycleBlock(tage, startPC, entries, &b1, history, stagePreds);
    EXPECT_TRUE(tage->tageTable[3][mainIndex][0].useful);
}

TEST_F(BTBTAGETest, EntryAllocationOnMissWhenBaseWrong)
{
    Addr startPC = 0x1000;
    // Base predicts slot0 taken, but actual is slot1 => miss/wrong should allocate.
    BTBEntry b0 = createBTBEntry(0x1000, true, true, false, /*ctr*/ 1);
    BTBEntry b1 = createBTBEntry(0x1002, true, true, false, /*ctr*/ -1);
    std::vector<BTBEntry> entries = {b0, b1};

    auto meta = predictUpdateCycleBlock(tage, startPC, entries, &b1, history, stagePreds);

    int table = findTableWithEntryWithMeta(tage, startPC, meta);
    EXPECT_GE(table, 0);
    EXPECT_EQ(tage->tageStats.updateAllocOnMiss, 1);
    EXPECT_EQ(tage->tageStats.updateAllocSuccess, 1);
}

TEST_F(BTBTAGETest, SelectorTrainingOnOtherCandidateHit)
{
    Addr startPC = 0x1000;
    BTBEntry b0 = createBTBEntry(0x1000, true, true, false, /*ctr*/ -1);
    BTBEntry b1 = createBTBEntry(0x1002, true, true, false, /*ctr*/ -1);
    std::vector<BTBEntry> entries = {b0, b1};

    Addr uidx = tage->getUseAltIdx(startPC);
    tage->useAlt[uidx] = -1;

    // Dual-candidate entry: enc0 predicts slot0, enc1 predicts slot1, selector initially picks enc0.
    setupTageEntry(tage, startPC, /*table*/ 3, /*conf*/ 0, /*exit0*/ 1, /*exit1*/ 2, /*sel*/ 0, /*useful*/ true);
    Addr mainIndex = tage->getTageIndex(startPC, 3);

    predictUpdateCycleBlock(tage, startPC, entries, &b1, history, stagePreds);

    // Should not rewrite payload; should only steer selector toward the correct candidate.
    EXPECT_EQ(tage->tageTable[3][mainIndex][0].exitSlotEnc0, 1);
    EXPECT_EQ(tage->tageTable[3][mainIndex][0].exitSlotEnc1, 2);
    EXPECT_EQ(tage->tageTable[3][mainIndex][0].selCtr, 1);
}

TEST_F(BTBTAGETest, PayloadMapFailFallbackToBase)
{
    Addr startPC = 0x1000;
    // Only two conds in this block => slot0(0x1000), slot1(0x1002).
    BTBEntry b0 = createBTBEntry(0x1000, true, true, false, /*ctr*/ -1);
    BTBEntry b1 = createBTBEntry(0x1002, true, true, false, /*ctr*/ 1);
    std::vector<BTBEntry> entries = {b0, b1};

    // Provider predicts slot2 (enc=3) which cannot map => should fallback to base (slot1).
    setupTageEntry(tage, startPC, /*table*/ 3, /*conf*/ 2, /*exit0*/ 3);

    Addr pred_pc = predictExitPC(tage, startPC, entries, history, stagePreds);
    EXPECT_EQ(pred_pc, 0x1002);

    auto meta = std::static_pointer_cast<BTBTAGE::TageMeta>(tage->getPredictionMeta());
    EXPECT_TRUE(meta->hasPred);
    EXPECT_EQ(meta->pred.source, BTBTAGE::PredSource::Base);
    EXPECT_EQ(meta->pred.baseEnc, 2);
    EXPECT_EQ(tage->tageStats.predPayloadMapFail, 1);
    EXPECT_EQ(tage->tageStats.predBaseFallback, 1);
}

TEST_F(BTBTAGETest, BankConflict)
{
    BTBTAGE *bankTage = new BTBTAGE(4, 2, 1024, 4);

    // Test 1: Same bank conflict (enabled)
    bankTage->enableBankConflict = true;
    {
        bankTage->lastPredBankId = bankTage->getBankId(0x20);
        bankTage->predBankValid = true;

        BTBEntry u = createBTBEntry(0xa0);
        FetchTarget stream = createStream(0xa0, {u}, &u, nullptr);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(stream);
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before + 1);
        EXPECT_FALSE(can_update);
        EXPECT_FALSE(bankTage->predBankValid);
    }

    // Test 2: Different bank, no conflict
    {
        bankTage->lastPredBankId = bankTage->getBankId(0x100);
        bankTage->predBankValid = true;

        BTBEntry u = createBTBEntry(0x104);
        FetchTarget stream = createStream(0x104, {u}, &u, nullptr);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(stream);
        EXPECT_TRUE(can_update);
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before);
        EXPECT_TRUE(bankTage->predBankValid);
    }

    // Test 3: Disabled flag prevents conflict
    bankTage->enableBankConflict = false;
    {
        bankTage->lastPredBankId = bankTage->getBankId(0x20);
        bankTage->predBankValid = true;

        BTBEntry u = createBTBEntry(0xa0);
        FetchTarget stream = createStream(0xa0, {u}, &u, nullptr);

        uint64_t conflicts_before = bankTage->tageStats.updateBankConflict;
        bool can_update = bankTage->canResolveUpdate(stream);
        EXPECT_TRUE(can_update);
        EXPECT_EQ(bankTage->tageStats.updateBankConflict, conflicts_before);
        EXPECT_TRUE(bankTage->predBankValid);
    }
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

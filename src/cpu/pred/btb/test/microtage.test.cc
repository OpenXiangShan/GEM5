#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/microtage.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

namespace
{

BTBEntry
createBTBEntry(Addr pc, int ctr = 0)
{
    BTBEntry entry;
    entry.pc = pc;
    entry.target = pc + 0x80;
    entry.isCond = true;
    entry.valid = true;
    entry.alwaysTaken = false;
    entry.ctr = ctr;
    return entry;
}

FetchTarget
createLegacyStream(Addr startPC, const BTBEntry &entry, bool taken,
                   const std::shared_ptr<void> &meta)
{
    FetchTarget stream;
    stream.startPC = startPC;
    stream.resolved = true;
    stream.exeTaken = taken;
    stream.exeBranchInfo = entry;
    stream.predBranchInfo = entry;
    stream.updateBTBEntries = {entry};
    stream.updateIsOldEntry = true;
    stream.predMetas[0] = meta;
    return stream;
}

ResolvedBranch
createResolvedBranch(const BTBEntry &entry, bool taken, bool mispredict)
{
    BranchInfo branch(entry);
    branch.resolved = true;
    branch.size = 4;
    return ResolvedBranch(branch, taken, mispredict, 0);
}

ResolvedTrainPacket
createResolvedTrainPacket(Addr startPC, const ResolvedBranch &resolved)
{
    ResolvedTrainPacket packet;
    packet.startPC = startPC;
    packet.realBranches.push_back(resolved);
    return packet;
}

ResolvedTrainPacket
createResolvedTrainPacket(Addr startPC,
                         std::vector<ResolvedBranch> resolvedBranches)
{
    ResolvedTrainPacket packet;
    packet.startPC = startPC;
    packet.realBranches = std::move(resolvedBranches);
    return packet;
}

FetchTarget
createResolvedTrainTarget(Addr startPC, const std::shared_ptr<void> &meta)
{
    FetchTarget target;
    target.startPC = startPC;
    target.predMetas[0] = meta;
    return target;
}

void
applyPathHistoryTaken(boost::dynamic_bitset<> &history, Addr pc, Addr target)
{
    history <<= 2;
    uint64_t hash = pathHash(pc, target);
    for (std::size_t i = 0; i < pathHashLength && i < history.size(); ++i) {
        history[i] = history[i] ^ (hash & 1);
        hash >>= 1;
    }
}

void
advanceActualHistory(MicroTAGE &tage, boost::dynamic_bitset<> &history,
                     const BTBEntry &entry, bool taken)
{
    tage.doUpdateHist(history, taken, entry.pc, entry.target);
    if (taken) {
        applyPathHistoryTaken(history, entry.pc, entry.target);
    }
}

bool
predictTaken(MicroTAGE &tage, Addr startPC, const BTBEntry &entry,
             boost::dynamic_bitset<> &history,
             std::vector<FullBTBPrediction> &stagePreds)
{
    stagePreds[1].btbEntries = {entry};
    stagePreds[1].condTakens.clear();
    tage.putPCHistory(startPC, history, stagePreds);
    Addr branchPC = entry.pc;
    auto it = CondTakens_find(stagePreds[1].condTakens, branchPC);
    EXPECT_NE(it, stagePreds[1].condTakens.end());
    return it != stagePreds[1].condTakens.end() && it->second;
}

MicroTAGE::TagePrediction
probePrediction(MicroTAGE &tage, Addr startPC, const BTBEntry &entry,
                boost::dynamic_bitset<> &history,
                std::vector<FullBTBPrediction> &stagePreds)
{
    stagePreds[1].btbEntries = {entry};
    stagePreds[1].condTakens.clear();
    tage.putPCHistory(startPC, history, stagePreds);
    auto meta = std::static_pointer_cast<MicroTAGE::TageMeta>(
        tage.getPredictionMeta());
    auto it = meta->preds.find(entry.pc);
    EXPECT_NE(it, meta->preds.end());
    return it != meta->preds.end() ? it->second : MicroTAGE::TagePrediction();
}

void
legacyTrain(MicroTAGE &tage, Addr startPC, const BTBEntry &entry, bool taken,
            boost::dynamic_bitset<> &history,
            std::vector<FullBTBPrediction> &stagePreds)
{
    bool predicted_taken = predictTaken(tage, startPC, entry, history, stagePreds);
    auto meta = tage.getPredictionMeta();
    FetchTarget stream = createLegacyStream(startPC, entry, taken, meta);
    if (predicted_taken != taken) {
        stream.squashType = SquashType::SQUASH_CTRL;
        stream.squashPC = entry.pc;
    }
    tage.update(stream);
    advanceActualHistory(tage, history, entry, taken);
}

void
resolveTrain(MicroTAGE &tage, Addr startPC, const BTBEntry &entry, bool taken,
             boost::dynamic_bitset<> &history,
             std::vector<FullBTBPrediction> &stagePreds)
{
    bool predicted_taken = predictTaken(tage, startPC, entry, history, stagePreds);
    auto meta = tage.getPredictionMeta();
    auto packet = createResolvedTrainPacket(
        startPC, createResolvedBranch(entry, taken, predicted_taken != taken));
    auto target = createResolvedTrainTarget(startPC, meta);
    ASSERT_TRUE(tage.canResolveTrain(packet, target));
    tage.resolveTrain(packet, target);
    advanceActualHistory(tage, history, entry, taken);
}

size_t
countValidEntries(const MicroTAGE &tage)
{
    size_t count = 0;
    for (const auto &table : tage.tageTable) {
        for (const auto &set : table) {
            for (const auto &way : set) {
                count += way.valid ? 1 : 0;
            }
        }
    }
    return count;
}

size_t
countEntriesForPc(const MicroTAGE &tage, Addr pc)
{
    size_t count = 0;
    for (const auto &table : tage.tageTable) {
        for (const auto &set : table) {
            for (const auto &way : set) {
                count += way.valid && way.pc == pc;
            }
        }
    }
    return count;
}

void
zeroStats(MicroTAGE &tage)
{
    std::memset(&tage.tageStats, 0, sizeof(MicroTAGE::TageStats));
}

} // namespace

TEST(MicroTAGEResolveTrainTest, MatchesLegacyTrainingOnRepeatedConditionalPattern)
{
    MicroTAGE legacy;
    MicroTAGE resolved;
    zeroStats(legacy);
    zeroStats(resolved);
    Addr startPC = 0x1000;
    Addr branchPC = startPC + 0x10;
    BTBEntry entry = createBTBEntry(branchPC, 0);
    std::vector<bool> pattern = {true, true, false, true, false, false};

    boost::dynamic_bitset<> legacyHistory(256);
    boost::dynamic_bitset<> resolvedHistory(256);
    std::vector<FullBTBPrediction> legacyStagePreds(2);
    std::vector<FullBTBPrediction> resolvedStagePreds(2);

    for (int iter = 0; iter < 12; ++iter) {
        for (bool taken : pattern) {
            legacyTrain(legacy, startPC, entry, taken, legacyHistory,
                        legacyStagePreds);
            resolveTrain(resolved, startPC, entry, taken, resolvedHistory,
                         resolvedStagePreds);
        }
    }

    size_t legacyOccupancy = countValidEntries(legacy);
    size_t resolvedOccupancy = countValidEntries(resolved);
    EXPECT_GT(legacyOccupancy, 0U)
        << "legacy training should allocate at least one MicroTAGE entry";
    EXPECT_EQ(legacyOccupancy, resolvedOccupancy);

    auto legacyProbe = probePrediction(legacy, startPC, entry, legacyHistory,
                                       legacyStagePreds);
    auto resolvedProbe = probePrediction(resolved, startPC, entry, resolvedHistory,
                                         resolvedStagePreds);
    EXPECT_EQ(legacyProbe.mainprovided, resolvedProbe.mainprovided);
    EXPECT_EQ(legacyProbe.mainInfo.found, resolvedProbe.mainInfo.found);
    EXPECT_EQ(legacyProbe.mainInfo.table, resolvedProbe.mainInfo.table);
    EXPECT_EQ(legacyProbe.taken, resolvedProbe.taken);
}

TEST(MicroTAGEResolveTrainTest,
     IgnoresUnpredictedConditionalBranchesOutsideLegacyTrainingSet)
{
    MicroTAGE legacy;
    MicroTAGE resolved;
    zeroStats(legacy);
    zeroStats(resolved);

    const Addr startPC = 0x2000;
    const BTBEntry predicted = createBTBEntry(startPC + 0x4, -1);
    const BTBEntry missing = createBTBEntry(startPC + 0x10, -1);

    boost::dynamic_bitset<> legacyHistory(256);
    boost::dynamic_bitset<> resolvedHistory(256);
    std::vector<FullBTBPrediction> legacyStagePreds(2);
    std::vector<FullBTBPrediction> resolvedStagePreds(2);

    legacyStagePreds[1].btbEntries = {predicted};
    legacy.putPCHistory(startPC, legacyHistory, legacyStagePreds);
    auto legacyMeta = legacy.getPredictionMeta();

    FetchTarget legacyStream = createLegacyStream(startPC, predicted, false,
                                                  legacyMeta);
    legacy.update(legacyStream);

    resolvedStagePreds[1].btbEntries = {predicted};
    resolved.putPCHistory(startPC, resolvedHistory, resolvedStagePreds);
    auto resolvedMeta = resolved.getPredictionMeta();

    auto packet = createResolvedTrainPacket(
        startPC,
        {createResolvedBranch(predicted, false, false),
         createResolvedBranch(missing, true, true)});
    auto target = createResolvedTrainTarget(startPC, resolvedMeta);
    ASSERT_TRUE(resolved.canResolveTrain(packet, target));
    resolved.resolveTrain(packet, target);

    EXPECT_EQ(countEntriesForPc(legacy, missing.pc), 0U)
        << "legacy update should not synthesize a MicroTAGE entry for an "
           "unpredicted branch in this scenario";
    EXPECT_EQ(countEntriesForPc(resolved, missing.pc),
              countEntriesForPc(legacy, missing.pc));
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

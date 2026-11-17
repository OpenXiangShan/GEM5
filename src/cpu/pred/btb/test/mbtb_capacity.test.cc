#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "cpu/pred/btb/mbtb.hh"

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

class MBTBCapacityTest : public ::testing::TestWithParam<unsigned>
{
  protected:
    static constexpr unsigned Entries = 8192;
    static constexpr unsigned Ways = 4;
    static constexpr Addr BlockSize = 32;

    void
    train(MBTB &btb, Addr pc, Addr target, ThreadID tid = 0)
    {
        std::vector<FullBTBPrediction> predictions(4);
        for (auto &pred : predictions) {
            pred.tid = tid;
            pred.asidHash = GetParam();
        }
        btb.putPCHistory(pc, boost::dynamic_bitset<>(8, 0), predictions);

        FetchTarget stream;
        stream.tid = tid;
        stream.asidHash = GetParam();
        stream.startPC = pc;
        stream.predMetas[0] = btb.getPredictionMeta(tid);
        stream.setPredictedBranches(predictions[btb.getDelay()].btbEntries);

        BranchOutcome outcome;
        outcome.tid = tid;
        outcome.branch.pc = pc;
        outcome.branch.target = target;
        outcome.taken = true;
        outcome.branch.isCond = true;
        outcome.branch.size = 4;
        btb.update(PredictionUpdateContext(stream),
                   PreparedUpdate(std::vector<BranchOutcome>{outcome}));
    }

    void
    expectResident(MBTB &btb, Addr pc, Addr target, ThreadID tid = 0)
    {
        const auto entries = btb.getPredictedEntriesNoSideEffect(
            pc, tid, GetParam());
        const auto it = std::find_if(entries.begin(), entries.end(),
            [pc](const BranchPrediction &entry) {
                return entry.branch.pc == pc;
            });
        ASSERT_NE(it, entries.end()) << "pc=" << pc << " tid=" << tid;
        EXPECT_EQ(it->branch.target, target);

        std::vector<FullBTBPrediction> predictions(4);
        for (auto &pred : predictions) {
            pred.tid = tid;
            pred.asidHash = GetParam();
        }
        btb.putPCHistory(pc, boost::dynamic_bitset<>(8, 0), predictions);
        const auto &predicted = predictions[btb.getDelay()].btbEntries;
        const auto pred = std::find_if(predicted.begin(), predicted.end(),
            [pc](const BranchPrediction &entry) {
                return entry.branch.pc == pc;
            });
        ASSERT_NE(pred, predicted.end()) << "pc=" << pc << " tid=" << tid;
        EXPECT_EQ(pred->branch.target, target);
    }

    void
    checkFullCapacity(bool partitioned)
    {
        // Disable the VC so it cannot hide premature main-array evictions.
        MBTB btb(Entries, 16, Ways, 1, 0);
        btb.setSmtTidPartitioned(partitioned);
        const unsigned threads = partitioned ? 2 : 1;
        const unsigned blocks = Entries / Ways / threads;

        // Each pass has one branch per block. Successive passes use a new
        // tag at every set, filling all ways with distinct aligned blocks.
        for (ThreadID tid = 0; tid < threads; ++tid) {
            for (unsigned way = 0; way < Ways; ++way) {
                for (unsigned block = 0; block < blocks; ++block) {
                    const Addr pc = 0x100000 +
                        (way * blocks + block) * BlockSize + 4;
                    train(btb, pc, pc + 0x400000 + tid * 0x100000, tid);
                }
            }
        }
        ASSERT_EQ(btb.btbStats.updateReplaceValidOne, 0);
        EXPECT_EQ(btb.btbStats.updateInVC, 0);

        // Verify after both threads have trained, including their same-PC
        // entries, through both normal and side-effect-free prediction.
        for (ThreadID tid = 0; tid < threads; ++tid) {
            for (unsigned way = 0; way < Ways; ++way) {
                for (unsigned block = 0; block < blocks; ++block) {
                    const Addr pc = 0x100000 +
                        (way * blocks + block) * BlockSize + 4;
                    ASSERT_NO_FATAL_FAILURE(expectResident(
                        btb, pc, pc + 0x400000 + tid * 0x100000, tid));
                }
            }
        }
    }
};

TEST_P(MBTBCapacityTest, AllConfiguredEntriesRemainResident)
{
    checkFullCapacity(false);
}

TEST_P(MBTBCapacityTest, TidPartitionsUseTheirFullCapacity)
{
    checkFullCapacity(true);
}

TEST_P(MBTBCapacityTest, FormerConflictStrideUsesIndependentSets)
{
    MBTB btb(Entries, 16, Ways, 1, 0);
    for (unsigned bank = 0; bank < 2; ++bank) {
        // The old index wraps after 32 KiB, though each SRAM has room for
        // another independent set before the correct 64 KiB wraparound.
        for (unsigned i = 0; i < Ways + 1; ++i) {
            const Addr pc = 0x100004 + bank * BlockSize + i * 0x8000;
            train(btb, pc, pc + 0x400000);
        }
    }
    ASSERT_EQ(btb.btbStats.updateReplaceValidOne, 0);
    for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned i = 0; i < Ways + 1; ++i) {
            const Addr pc = 0x100004 + bank * BlockSize + i * 0x8000;
            ASSERT_NO_FATAL_FAILURE(expectResident(btb, pc, pc + 0x400000));
        }
    }
}

TEST_P(MBTBCapacityTest, TagsStartAboveTheSramLocalIndex)
{
    for (bool partitioned : {false, true}) {
        MBTB btb(Entries, 16, Ways, 1, 0);
        btb.setSmtTidPartitioned(partitioned);
        const Addr indexBit = partitioned ? 0x4000 : 0x8000;
        const Addr tagBit = indexBit * 2;
        const unsigned threads = partitioned ? 2 : 1;
        for (ThreadID tid = 0; tid < threads; ++tid) {
            const std::vector<Addr> pcs = {
                0x100004, 0x100024, 0x100004 + indexBit, 0x100004 + tagBit
            };
            std::vector<Addr> tags;
            for (Addr pc : pcs) {
                train(btb, pc, pc + 0x400000, tid);
                std::vector<FullBTBPrediction> predictions(4);
                for (auto &pred : predictions) {
                    pred.tid = tid;
                    pred.asidHash = GetParam();
                }
                btb.putPCHistory(pc, boost::dynamic_bitset<>(8, 0),
                                 predictions);
                const auto &entries = btb.predictionStorageEntries(tid);
                ASSERT_FALSE(entries.empty());
                ASSERT_EQ(entries.front().branch.pc, pc);
                tags.push_back(entries.front().tag);
            }
            // Neither the SRAM bit nor the highest local-index bit belongs
            // to the tag. The next address bit must distinguish tags.
            EXPECT_EQ(tags[0], tags[1]);
            EXPECT_EQ(tags[0], tags[2]);
            EXPECT_EQ(tags[0] ^ tags[3], 1);
        }
    }
}

TEST_P(MBTBCapacityTest, FourWayConflictsStillEvict)
{
    MBTB btb(Entries, 16, Ways, 1, 0);
    for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned i = 0; i < Ways; ++i) {
            const Addr pc = 0x100004 + bank * BlockSize + i * 0x10000;
            train(btb, pc, pc + 0x400000);
        }
        for (unsigned i = 0; i < Ways; ++i) {
            const Addr pc = 0x100004 + bank * BlockSize + i * 0x10000;
            ASSERT_NO_FATAL_FAILURE(expectResident(btb, pc, pc + 0x400000));
        }

        const Addr extra = 0x100004 + bank * BlockSize + Ways * 0x10000;
        train(btb, extra, extra + 0x400000);
        ASSERT_NO_FATAL_FAILURE(expectResident(btb, extra, extra + 0x400000));
        EXPECT_EQ(btb.btbStats.updateReplaceValidOne, bank + 1);

        unsigned resident = 0;
        for (unsigned i = 0; i < Ways + 1; ++i) {
            const Addr pc = 0x100004 + bank * BlockSize + i * 0x10000;
            const auto entries = btb.getPredictedEntriesNoSideEffect(
                pc, 0, GetParam());
            resident += std::count_if(entries.begin(), entries.end(),
                [pc](const BranchPrediction &entry) {
                    return entry.branch.pc == pc;
                });
        }
        EXPECT_EQ(resident, Ways);
    }
}

INSTANTIATE_TEST_SUITE_P(AllAsidHashes, MBTBCapacityTest,
                        ::testing::Range(0u, 16u));

} // anonymous namespace
} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

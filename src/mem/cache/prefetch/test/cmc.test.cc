#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "base/types.hh"
#include "common/events.hh"
#include "common/mock_types.hh"
#include "mem/cache/prefetch/test/cmc.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{

// Helper to create a PrefetchInfo object with a PC
PrefetchInfo
createPrefetchInfo(Addr addr, Addr pc, bool is_miss)
{
    PrefetchInfo pfi;
    pfi.setAddr(addr);
    pfi.setCacheMiss(is_miss);
    pfi.setSecure(false);
    pfi.setWrite(false);
    pfi.setPC(pc);
    return pfi;
}

// Test fixture for CMC tests
class CMCTest : public ::testing::Test
{
  protected:
    CMCPrefetcher *cmc;
    CMCPrefetcherParams params;
    std::vector<AddrPriority> prefetch_candidates;

    void
    SetUp() override
    {
        params.name = "CMCTest";
        params.block_size = 64;
        params.nr_entry = 16;
        // other params are default
        cmc = new CMCPrefetcher(params);
    }

    void
    TearDown() override
    {
        delete cmc;
    }

  public:
    // Helper function to issue a demand miss to the CMC
    void
    issueDmdMissToCmc(Addr addr, Addr pc)
    {
        PrefetchInfo pfi = createPrefetchInfo(addr, pc, true /* is_miss */);
        prefetch_candidates.clear();
        cmc->doPrefetch(pfi, prefetch_candidates, false, PrefetchSourceType::CMC, false);
    }
};

// Test case to verify that the CMC prefetcher can be initialized
TEST_F(CMCTest, Initialization)
{
    SUCCEED();
}

// Test case for the common pattern:
// 1. A PC triggers a miss, initiating a recording session.
// 2. 16 subsequent misses are recorded.
// 3. A second miss from the same PC triggers 16 prefetches.
TEST_F(CMCTest, LearnAndPrefetch)
{
    const Addr trigger_pc = 0x1000;
    const Addr trigger_addr = 0x10000;
    const unsigned num_entries_to_record = params.nr_entry;

    // Phase 1: Recording
    // Issue the first miss to start recording
    issueDmdMissToCmc(trigger_addr, trigger_pc);
    ASSERT_TRUE(prefetch_candidates.empty())
        << "Should not prefetch on the first trigger miss.";

    // Issue `num_entries_to_record + 1` more misses to be recorded
    // Actually, each trigger will record `num_entries_to_record + 1` misses
    std::vector<Addr> recorded_addrs;
    for (unsigned i = 0; i <= num_entries_to_record; ++i) {
        Addr record_addr = 0x20000 + i * params.block_size;
        recorded_addrs.push_back(record_addr);
        issueDmdMissToCmc(record_addr, trigger_pc + 4 * (i + 1));
        ASSERT_TRUE(prefetch_candidates.empty()) << "Should not prefetch during recording phase.";
    }

    // Phase 2: Prefetching
    // Issue a second miss with the same trigger PC and address
    issueDmdMissToCmc(trigger_addr, trigger_pc);

    // Check if `num_entries_to_record + 1` prefetches were generated
    ASSERT_EQ(prefetch_candidates.size(), num_entries_to_record + 1)
        << "Should generate " << num_entries_to_record + 1 << " prefetch candidates.";

    // Verify the addresses of the generated prefetches
    for (unsigned i = 0; i <= num_entries_to_record; ++i) {
        bool found = false;
        for (const auto& p_addr : prefetch_candidates) {
            if (p_addr.getAddr() == recorded_addrs[i]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Prefetch for address " << std::hex
                           << recorded_addrs[i] << " not found.";
    }
}

// Test case to check hash collision avoidance.
// Training with (PC, Addr_A) should not cause prefetches for (PC, Addr_B).
TEST_F(CMCTest, HashCollisionAvoidance)
{
    const Addr trigger_pc = 0x1000;
    const Addr trigger_addr_A = 0x10000;
    const Addr trigger_addr_B = 0x50000; // Different address, same PC
    const unsigned num_entries_to_record = params.nr_entry;

    // Phase 1: Train with (PC, Addr_A)
    issueDmdMissToCmc(trigger_addr_A, trigger_pc); // Start recording
    for (unsigned i = 0; i <= num_entries_to_record; ++i) {
        Addr record_addr = 0x20000 + i * params.block_size;
        issueDmdMissToCmc(record_addr, trigger_pc + 4 * (i + 1));
    }

    // Phase 2: Trigger with (PC, Addr_B)
    // This should not find a match for Addr_A, so no prefetches should be issued.
    // Instead, it should start a new recording session.
    issueDmdMissToCmc(trigger_addr_B, trigger_pc);

    ASSERT_TRUE(prefetch_candidates.empty())
        << "Should not prefetch when the trigger address does not match.";
}

// Test case to check the effect of a real hash collision.
// If (PC_1, Addr_1) and (PC_2, Addr_2) have the same hash, training on the
// first pair should incorrectly cause prefetching for the second pair.
TEST_F(CMCTest, HashCollisionAliasing)
{
    // These PC/address pairs are chosen so their hashes collide.
    // hash = (addr >> log2(block_size)) ^ pc
    // block_size = 64, so log2(block_size) = 6
    // hash_A = (0x20000 >> 6) ^ 0x1000 = 0x800 ^ 0x1000 = 0x1800
    // hash_B = (0xA0000 >> 6) ^ 0x3000 = 0x2800 ^ 0x3000 = 0x1800
    const Addr trigger_pc_A = 0x1000;
    const Addr trigger_addr_A = 0x20000;
    const Addr trigger_pc_B = 0x3000;
    const Addr trigger_addr_B = 0xA0000;
    const unsigned num_entries_to_record = params.nr_entry;

    // Phase 1: Train with (PC_A, Addr_A)
    issueDmdMissToCmc(trigger_addr_A, trigger_pc_A); // Start recording

    std::vector<Addr> recorded_addrs;
    for (unsigned i = 0; i <= num_entries_to_record; ++i) {
        Addr record_addr = 0x30000 + i * params.block_size;
        recorded_addrs.push_back(record_addr);
        issueDmdMissToCmc(record_addr, trigger_pc_A + 4 * (i + 1));
    }

    // Phase 2: Trigger with (PC_B, Addr_B)
    // Due to the hash collision, this should incorrectly trigger prefetches
    issueDmdMissToCmc(trigger_addr_B, trigger_pc_B);

    // Check that prefetches were generated
    ASSERT_EQ(prefetch_candidates.size(), num_entries_to_record + 1)
        << "Should incorrectly generate prefetches due to hash collision.";

    // Verify the addresses match what was trained for pair A
    for (unsigned i = 0; i <= num_entries_to_record; ++i) {
        bool found = false;
        for (const auto& p_addr : prefetch_candidates) {
            if (p_addr.getAddr() == recorded_addrs[i]) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Incorrectly prefetched address for "
                           << std::hex << recorded_addrs[i] << " not found.";
    }
}


// Test case to ensure that a miss on a recorded (non-trigger) address
// does not generate prefetches.
TEST_F(CMCTest, NoPrefetchOnRecordedAddress)
{
    const Addr trigger_pc = 0x1000;
    const Addr trigger_addr = 0x10000;
    const unsigned num_entries_to_record = params.nr_entry;

    // Phase 1: Train the prefetcher
    issueDmdMissToCmc(trigger_addr, trigger_pc); // Start recording

    std::vector<Addr> recorded_addrs;
    for (unsigned i = 0; i <= num_entries_to_record; ++i) {
        Addr record_addr = 0x20000 + i * params.block_size;
        recorded_addrs.push_back(record_addr);
        issueDmdMissToCmc(record_addr, trigger_pc + 4 * (i + 1));
    }

    // Phase 2: Issue a miss on one of the recorded addresses
    // This should not trigger any prefetching.
    const Addr recorded_addr_miss = recorded_addrs[5];
    const Addr some_other_pc = 0x9999;
    issueDmdMissToCmc(recorded_addr_miss, some_other_pc);

    ASSERT_TRUE(prefetch_candidates.empty())
        << "Should not generate prefetches for a miss on a recorded address.";
}

} // namespace test
} // namespace prefetch
} // namespace gem5

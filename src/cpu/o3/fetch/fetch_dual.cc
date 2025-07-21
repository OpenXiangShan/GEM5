#include <algorithm>
#include <set>
#include <sstream>

#include "base/types.hh"
#include "cpu/o3/fetch/fetch.hh"
#include "debug/Fetch.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{
namespace o3
{

void
Fetch::sendNextCacheRequest(ThreadID tid, const PCStateBase &pc_state)
{
    fetchStats.twoFetchRequests++;
    if (!needNewFTQEntry(tid)) {
        fetchStats.twoFetchFailedDueToNoNewFTQEntry++;
        return;
    }

    // reset fetch2Coord
    fetch2Coord[tid].reset();

    // Check if we should use dual fetch mode
    if (shouldUseDualFetch(tid) && hasDualFTQEntries(tid)) {
        DPRINTF(Fetch, "[tid:%i] Using dual fetch mode - checking dual fetch conditions\n", tid);

        // Get dual FTQ start PCs from BTB predictor
        auto pcPair = getDualFTQPCs(tid);
        Addr ftq0_pc = pcPair.first;
        Addr ftq1_pc = pcPair.second;

        // Validate FTQ PCs
        if (ftq0_pc == 0 || ftq1_pc == 0) {
            fetchStats.twoFetchFailedDueToInvalidFTQPCs++;
            fallbackToSingleFetch(tid, pc_state, "Dual FTQ PCs invalid");
            return;
        }

        // Strict 2fetch address constraint: only use 2fetch when both FTQ start addresses are in the same 4K page
        Addr ftq0_page = ftq0_pc & ~0xFFF;  // 4K page containing FTQ0 start address
        Addr ftq1_page = ftq1_pc & ~0xFFF;  // 4K page containing FTQ1 start address

        // Only use 2fetch when both FTQ start addresses are in the same 4K page
        if (ftq0_page != ftq1_page) {
            fetchStats.twoFetchFailedDueToFetchRangesSpanMultiplePages++;
            fallbackToSingleFetch(tid, pc_state,
                csprintf("FTQ addresses in different pages: %#x vs %#x", ftq0_page, ftq1_page));
            return;
        }

        // Even if start addresses are in the same page, verify entire fetch ranges are within reasonable bounds
        Addr ftq0_end = ftq0_pc + fetchBufferSize;
        Addr ftq1_end = ftq1_pc + fetchBufferSize;
        Addr ftq0_end_page = (ftq0_end - 1) & ~0xFFF;  // Page containing FTQ0 end address
        Addr ftq1_end_page = (ftq1_end - 1) & ~0xFFF;  // Page containing FTQ1 end address

        // If fetch ranges span too many different pages, fallback to single fetch for consistency
        if (ftq0_end_page != ftq0_page || ftq1_end_page != ftq1_page ||
            ftq0_end_page != ftq1_end_page) {
            fetchStats.twoFetchFailedDueToFetchRangesSpanMultiplePages++;
            fallbackToSingleFetch(tid, pc_state, "Fetch ranges span multiple pages");
            return;
        }

        // === Bank conflict aware 2fetch implementation ===
        // Read FTQ information to calculate valid lengths
        auto ftq0_entry = getFTQEntry(tid, 0);      // FTQ 0
        auto ftq1_entry = getFTQEntry(tid, 1);      // FTQ 1

        // Check if both FTQ entries have taken branches
        if (!ftq0_entry.taken || !ftq1_entry.taken) {
            fetchStats.twoFetchFailedDueToFallThrough++;
            fallbackToSingleFetch(tid, pc_state,
                csprintf("FTQ without taken branch: ftq0=%d, ftq1=%d",
                        ftq0_entry.taken, ftq1_entry.taken));
            return;
        }

        // Calculate valid length (from startPC to takenPC + instruction size)
        unsigned ftq0_validLen = ftq0_entry.takenPC - ftq0_entry.startPC + 4;
        unsigned ftq1_validLen = ftq1_entry.takenPC - ftq1_entry.startPC + 4;

        // Check for bank conflicts using BankConflictCalculator
        if (BankConflictCalculator::hasBankConflict(ftq0_entry.startPC, ftq0_validLen,
                                                   ftq1_entry.startPC, ftq1_validLen)) {
            DPRINTF(Fetch, "[tid:%i] Bank conflict detected: ftq0[%#x:%d] vs ftq1[%#x:%d]\n",
                    tid, ftq0_entry.startPC, ftq0_validLen, ftq1_entry.startPC, ftq1_validLen);

            fetchStats.twoFetchFailedDueToBankConflict++;
            fallbackToSingleFetch(tid, pc_state,
                csprintf("Bank conflict: ftq0[%#x:%d] vs ftq1[%#x:%d]",
                        ftq0_entry.startPC, ftq0_validLen, ftq1_entry.startPC, ftq1_validLen));
            return;
        }

        fetchStats.twoFetchSuccess++;
        DPRINTF(Fetch, "[tid:%i] **2FETCH APPROVED** ftq0[%#x:%d bytes] ftq1[%#x:%d bytes]\n",
                tid, ftq0_entry.startPC, ftq0_validLen, ftq1_entry.startPC, ftq1_validLen);

        DPRINTF(Fetch, "[tid:%i] Issuing dual pipelined I-cache accesses: "
                    "FTQ0 PC %#x, FTQ1 PC %#x (original PC %s)\n",
                    tid, ftq0_pc, ftq1_pc, pc_state);

        // Send parallel cache requests for both FTQs (standard 66 bytes)
        bool success0 = fetchCacheLine(ftq0_pc, tid, pc_state.instAddr(), 0);
        bool success1 = fetchCacheLine(ftq1_pc, tid, pc_state.instAddr(), 1);

        if (!success0 && !success1) {
            DPRINTF(Fetch, "[tid:%i] Both dual cache requests failed\n", tid);
            fetchStats.twoFetchFailedDueToCacheBlocked++;
            return;
        }

        // === Set fetchBuffer valid bytes for bank conflict limitation ===
        if (success0) {
            fetchBuffer[tid][0].validBytes = ftq0_validLen;
            DPRINTF(Fetch, "[tid:%i][ftq:0] Set validBytes=%d (vs size=%d)\n",
                    tid, ftq0_validLen, fetchBufferSize);
        }
        if (success1) {
            fetchBuffer[tid][1].validBytes = ftq1_validLen;
            DPRINTF(Fetch, "[tid:%i][ftq:1] Set validBytes=%d (vs size=%d)\n",
                    tid, ftq1_validLen, fetchBufferSize);
        }

        if (!success0 && !success1) {
            DPRINTF(Fetch, "[tid:%i] Both dual cache requests failed\n", tid);
            return;
        }

        // Mark FTQ coordinator states based on success
        fetch2Coord[tid].ftqActive[0] = success0;
        fetch2Coord[tid].ftqActive[1] = success1;

        DPRINTF(Fetch, "[tid:%i] Dual fetch coordinator: FTQ0=%s, FTQ1=%s\n",
                tid, success0 ? "active" : "inactive", success1 ? "active" : "inactive");

    } else {
        fetchStats.twoFetchFailedDueToInvalidFTQPCs++;
        // Single FTQ mode - either 2fetch not enabled or conditions not met
        fallbackToSingleFetch(tid, pc_state, "Single fetch mode selected");
    }
}

bool
Fetch::shouldUseDualFetch(ThreadID tid)
{
    assert(tid < MaxThreads);

    // Check basic conditions for dual fetch
    return enable2Fetch &&                      // 2fetch enabled via parameter
           isDecoupledFrontend() &&              // Using decoupled frontend
           isBTBPred() &&                        // BTB predictor (only supported one)
           canFetchInstructions(tid) &&          // Thread can fetch
           !hasPendingCacheRequests(tid) &&      // No pending requests
           hasDualFTQEntries(tid) &&             // BTB has dual entries
           !fetch2Coord[tid].hasPendingFetch();  // No pending dual fetch
}

bool
Fetch::hasDualFTQEntries(ThreadID tid)
{
    assert(tid < MaxThreads);

    // Check if BTB predictor has dual FTQ entries available
    if (isBTBPred() && dbpbtb) {
        return dbpbtb->hasTwoFetchTargets();
    }

    // Other predictor types not supported yet
    return false;
}

std::pair<Addr, Addr>
Fetch::getDualFTQPCs(ThreadID tid)
{
    assert(tid < MaxThreads);

    // Get dual FTQ start PCs from BTB predictor
    if (isBTBPred() && dbpbtb) {
        return dbpbtb->getDualFTQPCs();
    }

    // Return invalid PCs for unsupported predictors
    return std::make_pair(0, 0);
}

branch_prediction::btb_pred::FtqEntry
Fetch::getFTQEntry(ThreadID tid, unsigned ftqIndex)
{
    assert(tid < MaxThreads);
    assert(ftqIndex < 2);

    // Get dual FTQ entry from BTB predictor
    if (isBTBPred() && dbpbtb) {
        return dbpbtb->getFTQEntry(ftqIndex);
    }

    // Return invalid entry for unsupported predictors
    return branch_prediction::btb_pred::FtqEntry();
}

void
Fetch::finishDualFTQTargets()
{
    // Mark dual FTQ targets as finished in BTB predictor
    if (isBTBPred() && dbpbtb) {
        dbpbtb->finishDualFetchTargets();
    }
}

void
Fetch::fallbackToSingleFetch(ThreadID tid, const PCStateBase &pc_state,
                             const std::string& reason)
{
    DPRINTF(Fetch, "[tid:%i] **2FETCH FALLBACK** %s, using single fetch mode\n",
            tid, reason);

    Addr ftq_start_pc = isDecoupledFrontend() ?
            getNextFTQStartPC(tid) : pc_state.instAddr();
    if (ftq_start_pc != 0) {
        DPRINTF(Fetch, "[tid:%i] Fallback: issuing single cache request, "
                    "starting at PC %#x\n", tid, ftq_start_pc);
        bool success = fetchCacheLine(ftq_start_pc, tid, pc_state.instAddr(), 0);

        // Set validBytes to full buffer size for single fetch mode
        if (success) {
            fetchBuffer[tid][0].validBytes = fetchBufferSize;
            DPRINTF(Fetch, "[tid:%i] Single fetch: set validBytes=%d for FTQ 0\n",
                    tid, fetchBufferSize);
        }
    }
}

//
// BankConflictCalculator Implementation
//

std::set<unsigned>
Fetch::BankConflictCalculator::getBankSet(Addr addr, unsigned len)
{
    std::set<unsigned> banks;

    // Iterate through all bytes in the address range
    for (Addr a = addr; a < addr + len; a++) {
        // Calculate bank index: (address % cache_line_size) / bank_size
        unsigned bank = (a % CACHE_LINE_SIZE) / BANK_SIZE;
        banks.insert(bank);
    }

    return banks;
}

bool
Fetch::BankConflictCalculator::canBeMergedInto64B(Addr addr1, unsigned len1,
                                                  Addr addr2, unsigned len2)
{
    // Calculate the total range that would cover both FTQ ranges
    Addr start = std::min(addr1, addr2);
    Addr end1 = addr1 + len1;
    Addr end2 = addr2 + len2;
    Addr end = std::max(end1, end2);

    // Check if total range can fit in 64B access
    return (end - start) <= CACHE_LINE_SIZE;
}

bool
Fetch::BankConflictCalculator::hasBankConflict(Addr addr1, unsigned len1,
                                               Addr addr2, unsigned len2)
{
    // Check if two FTQ ranges can be merged into a single 64B access
    if (canBeMergedInto64B(addr1, len1, addr2, len2)) {
        // Case 1: RTL can merge into single 64B access
        // Calculate the merged range and check bank usage directly
        Addr mergedStart = std::min(addr1, addr2);
        Addr mergedEnd = std::max(addr1 + len1, addr2 + len2);
        auto mergedBanks = getBankSet(mergedStart, mergedEnd - mergedStart);

        // Check if merged access exceeds bank limit (8 banks per cycle)
        return mergedBanks.size() > BANKS_PER_LINE;
    }

    // Case 2: Cannot merge - RTL will do two separate accesses
    // Check for bank index conflicts between the two accesses
    auto banks1 = getBankSet(addr1, len1);
    auto banks2 = getBankSet(addr2, len2);

    // Check if any bank indices conflict
    for (auto bank : banks1) {
        if (banks2.count(bank)) {
            return true;  // Bank conflict detected
        }
    }

    return false;  // No bank conflicts
}

std::string
Fetch::BankConflictCalculator::getBankSetString(Addr addr, unsigned len)
{
    auto banks = getBankSet(addr, len);
    std::stringstream ss;

    ss << "banks[";
    bool first = true;
    for (auto bank : banks) {
        if (!first) ss << ",";
        ss << bank;
        first = false;
    }
    ss << "]";

    return ss.str();
}


} // namespace o3
} // namespace gem5

#ifndef __CPU_PRED_BTB_HISTORY_MANAGER_HH__
#define __CPU_PRED_BTB_HISTORY_MANAGER_HH__

#include <list>

#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/folded_hist.hh"

#ifdef UNIT_TEST
#include "cpu/pred/btb/test/test_dprintf.hh"

#else
#include "debug/DecoupleBPVerbose.hh"

#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

/**
 * @class HistoryManager
 * @brief Manages branch prediction history including speculative updates and recovery
 *
 * This class tracks branch prediction history and handles:
 * - Speculative history updates for predicted branches
 * - History recovery on misprediction
 * - History commit when branches are resolved
 */
class HistoryManager
{
  public:
    /**
     * @struct HistoryEntry
     * @brief Represents a single branch history entry
     *
     * Contains information about a branch prediction including:
     * - Branch PC and shift amount for history updates
     * - Direction and type information (conditional, call, return)
     * - Return address for return instructions
     * - Stream ID for tracking speculative state
     */
    struct HistoryEntry
    {
        HistoryEntry(Addr _pc,
            const boost::dynamic_bitset<> &ghist_base,
            const boost::dynamic_bitset<> &phist_base,
            DirectionHistoryUpdate ghist_update,
            PathHistoryUpdate phist_update, bool _is_call,
            bool _is_return, Addr _retAddr, uint64_t stream_id)
            : pc(_pc),
              ghistBase(ghist_base),
              phistBase(phist_base),
              shamt(ghist_update.shamt),
              cond_taken(ghist_update.taken),
              phistUpdate(phist_update),
              is_call(_is_call),
              is_return(_is_return),
              retAddr(_retAddr),
              streamId(stream_id)
        {
        }
      Addr pc;           ///< Program counter of the branch
      boost::dynamic_bitset<> ghistBase; ///< GHR before this speculative update
      boost::dynamic_bitset<> phistBase; ///< PHR before this speculative update
      int shamt;         ///< Shift amount for direction history update
      bool cond_taken;   ///< Whether conditional branch was taken
      PathHistoryUpdate phistUpdate; ///< Path history update
      bool is_call;      ///< Whether branch is a call instruction
      bool is_return;    ///< Whether branch is a return instruction
      Addr retAddr;      ///< Return address (for call instructions)
      uint64_t streamId; ///< ID of the fetch stream containing this branch
    };

    /**
     * @brief Constructor
     * @param _maxShamt Maximum shift amount allowed for history updates
     */
    HistoryManager(unsigned _maxShamt) : maxShamt(_maxShamt) {}

  private:
    /**
     * List of speculative history entries, maintained in program order
     * Each entry represents a predicted branch that has not yet been committed
     */
    std::list<HistoryEntry> speculativeHists;

    /**
     * Ideal history length for branch prediction
     */
    unsigned IdealHistLen{246};

    /**
     * Maximum allowed shift amount for history updates
     * Used for sanity checking to prevent excessive history shifts
     */
    unsigned maxShamt;

  public:
    /**
     * @brief Adds a new speculative history entry
     *
     * Records branch prediction information for a new fetch stream.
     * This is called when a new branch prediction is made.
     *
     * @param addr Address of the branch
     * @param ghist_update Direction-history update
     * @param phist_update Path-history update
     * @param is_call Whether the predicted branch is a call
     * @param is_return Whether the predicted branch is a return
     * @param ret_addr Predicted return address for calls
     * @param stream_id ID of the fetch stream containing this branch
     */
    void addSpeculativeHist(const Addr addr,
                            const boost::dynamic_bitset<> &ghist_base,
                            const boost::dynamic_bitset<> &phist_base,
                            const DirectionHistoryUpdate &ghist_update,
                            const PathHistoryUpdate &phist_update,
                            bool is_call,
                            bool is_return,
                            Addr ret_addr,
                            const uint64_t stream_id)
    {
        // Add new entry to the end of speculative history list
        speculativeHists.emplace_back(addr, ghist_base, phist_base,
            ghist_update, phist_update,
            is_call, is_return, ret_addr, stream_id);

        // Debug print the newly added entry
        const auto &it = speculativeHists.back();
        printEntry("Add", it);
    }

    /**
     * @brief Commits history entries up to the specified stream ID
     *
     * Removes speculative history entries that are confirmed correct
     * after a stream has been executed and committed.
     *
     * @param stream_id Highest stream ID to commit
     */
    void commit(const uint64_t stream_id)
    {
        auto it = speculativeHists.begin();

        // Remove all entries with stream IDs less than or equal to the committed stream ID
        while (it != speculativeHists.end()) {
            if (it->streamId <= stream_id) {
                // Debug output before removing
                printEntry("Commit", *it);

                // Remove and advance iterator
                it = speculativeHists.erase(it);
            } else {
                // Keep entries with higher stream IDs
                ++it;
            }
        }
    }

    /**
     * @brief Gets the list of speculative history entries
     *
     * Used for debugging and history reconstruction.
     * Used in DecoupledBPUWithBTB::checkHistory to rebuild the "ideal" history
     *
     * @return Reference to the list of speculative history entries
     */
    const std::list<HistoryEntry> &getSpeculativeHist()
    {
        return speculativeHists;
    }

    /**
     * @brief Handles history recovery during squash
     *
     * Updates the history for the squashed stream with actual branch outcome
     * and removes all subsequent speculative history entries.
     *
     * @param stream_id ID of the stream being squashed
     * @param ghist_update Actual direction-history update
     * @param phist_update Path-history update
     * @param branch Actual resolved branch result, or nullptr for non-control
     *               squashes
     */
    void squash(const uint64_t stream_id,
                const DirectionHistoryUpdate &ghist_update,
                const PathHistoryUpdate &phist_update,
                const ResolvedBranch *branch)
    {
        // Debug dump before squash operations
        dump("before squash");

        auto it = speculativeHists.begin();

        while (it != speculativeHists.end()) {
            if (it->streamId == stream_id) {
                // Found the squashed stream - update with correct information
                it->cond_taken = ghist_update.taken;
                it->shamt = ghist_update.shamt;
                it->phistUpdate = phist_update;

                // Update branch type information
                it->is_call = branch && branch->isCall;
                it->is_return = branch && branch->isReturn;
                it->retAddr = branch ? branch->pc + branch->size : 0;

                // Move to next entry
                ++it;
            } else if (it->streamId > stream_id) {
                // Remove all speculative entries after the squashed stream
                // as they were based on incorrect prediction
                printEntry("Squash", *it);
                it = speculativeHists.erase(it);
            } else { // it->streamId < stream_id
                // Keep entries with lower stream IDs (already committed)
                DPRINTF(DecoupleBPVerbose,
                        "Skip stream %lu when squashing stream %lu\n",
                        it->streamId, stream_id);
                ++it;
            }
        }

        // Debug dump after squash operations
        dump("after squash");

        // Verify history integrity after squash
        checkSanity();
    }

    boost::dynamic_bitset<> replayGHist(unsigned history_bits) const
    {
        if (speculativeHists.empty()) {
            return boost::dynamic_bitset<>(history_bits, 0);
        }

        // The first surviving entry stores the GHR snapshot before its own
        // speculative update. Replaying all recorded direction updates from
        // that base should reproduce DecoupledBPUWithBTB's current GHR after
        // commit/squash bookkeeping.
        boost::dynamic_bitset<> ideal_hist = speculativeHists.front().ghistBase;
        ideal_hist.resize(history_bits);
        for (const auto &entry : speculativeHists) {
            if (entry.shamt <= 0) {
                continue;
            }
            ideal_hist <<= entry.shamt;
            ideal_hist[0] = entry.cond_taken;
        }
        return ideal_hist;
    }

    boost::dynamic_bitset<> replayPHist(unsigned history_bits) const
    {
        if (speculativeHists.empty()) {
            return boost::dynamic_bitset<>(history_bits, 0);
        }

        // PHR uses the same ledger invariant as GHR, but replays the recorded
        // PathHistoryUpdate instead of re-deriving path-update semantics here.
        // This keeps HistoryManager a consistency checker for raw PHR state,
        // while FetchTarget tests cover whether each descriptor is correct.
        boost::dynamic_bitset<> ideal_hist = speculativeHists.front().phistBase;
        ideal_hist.resize(history_bits);
        for (const auto &entry : speculativeHists) {
            const auto &update = entry.phistUpdate;
            if (update.shamt <= 0 || !update.taken) {
                continue;
            }
            ideal_hist <<= update.shamt;
            uint64_t hash = pathHash(update.pc, update.target);
            for (std::size_t i = 0;
                 i < pathHashLength && i < ideal_hist.size(); ++i) {
                ideal_hist[i] = (hash & 1) ^ ideal_hist[i];
                hash >>= 1;
            }
        }
        return ideal_hist;
    }

    bool checkGHist(const boost::dynamic_bitset<> &history,
                    unsigned history_bits) const
    {
        if (speculativeHists.empty()) {
            return true;
        }
        return compareHistory(replayGHist(history_bits), history, history_bits);
    }

    bool checkPHist(const boost::dynamic_bitset<> &history,
                    unsigned history_bits) const
    {
        if (speculativeHists.empty()) {
            return true;
        }
        return compareHistory(replayPHist(history_bits), history, history_bits);
    }

    /**
     * @brief Validates history integrity
     *
     * Performs sanity checks on the history list to ensure
     * history shift amounts are within limits.
     */
    void checkSanity()
    {
        // Skip check if fewer than 2 entries
        if (speculativeHists.size() < 2) {
            return;
        }

        // Iterate through history entries checking shift amounts
        auto last = speculativeHists.begin();
        auto cur = speculativeHists.begin();
        cur++;

        while (cur != speculativeHists.end()) {
            // Check if shift amount exceeds maximum allowed
            if (cur->shamt > maxShamt) {
                dump("before warn");
                warn("entry shifted more than %d bits\n", maxShamt);
            }

            // Move to next entry
            last = cur;
            cur++;
        }
    }

    /**
     * @brief Dumps the history list for debugging
     *
     * @param when Context string indicating when the dump is occurring
     */
    void dump(const char* when)
    {
        DPRINTF(DecoupleBPVerbose, "Dump ideal history %s:\n", when);

        // Print each entry in the history list
        for (const auto& entry : speculativeHists) {
            printEntry("", entry);
        }
    }

    /**
     * @brief Prints a single history entry for debugging
     *
     * @param when Context string indicating when the print is occurring
     * @param entry The history entry to print
     */
    void printEntry(const char* when, const HistoryEntry& entry)
    {
        DPRINTF(DecoupleBPVerbose,
                "%s stream: %lu, pc %#lx, shamt %ld, cond_taken %d, "
                "phist_shamt %d, phist_taken %d, phist_pc %#lx, "
                "phist_target %#lx, is_call %d, is_ret %d, retAddr %#lx\n",
                when, entry.streamId, entry.pc, entry.shamt, entry.cond_taken,
                entry.phistUpdate.shamt,
                entry.phistUpdate.taken,
                entry.phistUpdate.pc,
                entry.phistUpdate.target,
                entry.is_call, entry.is_return, entry.retAddr);
    }

  private:
    static bool compareHistory(const boost::dynamic_bitset<> &ideal,
                               const boost::dynamic_bitset<> &actual,
                               unsigned history_bits)
    {
        boost::dynamic_bitset<> sized_ideal(ideal);
        boost::dynamic_bitset<> sized_actual(actual);
        sized_ideal.resize(history_bits);
        sized_actual.resize(history_bits);
        return sized_ideal == sized_actual;
    }
};

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_HISTORY_MANAGER_HH__

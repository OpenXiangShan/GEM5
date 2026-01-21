#include "cpu/pred/btb/fetch_target_queue.hh"

#include "base/trace.hh"
#ifdef UNIT_TEST
  #include "cpu/pred/btb/test/test_dprintf.hh"
#else
  #include "debug/DecoupleBP.hh"
  #include "debug/DecoupleBPProbe.hh"
#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

/**
 * @brief Constructor for the Fetch Target Queue
 *
 * Initializes the FTQ with a specified maximum size and sets up the
 * initial state for fetch target enqueuing and demand tracking.
 *
 * @param size Maximum number of entries the queue can hold
 */
FetchTargetQueue::FetchTargetQueue(unsigned size) :
 ftqSize(size)
{
    fetchTargetEnqState.pc = 0x80000000;  // Initialize PC to default boot address
    fetchDemandTargetId = 0;              // Start with target ID 0
    supplyFetchTargetState.valid = false; // No valid supply state initially
}

/**
 * @brief Clear all entries and reset queue state after a pipeline flush
 *
 * This method is called after a branch misprediction or other event that
 * requires the pipeline to be flushed. It resets the state of the FTQ to
 * start fetching from a new PC and stream.
 *
 * @param new_enq_target_id New target ID to begin enqueueing at
 * @param new_enq_stream_id New stream ID to associate with new entries
 * @param new_enq_pc New PC to begin fetching from
 */
void
FetchTargetQueue::squash(FetchTargetId new_enq_target_id,
                         FetchStreamId new_enq_stream_id, Addr new_enq_pc)
{
    ftq.clear();  // Remove all entries from the queue
    // Because we squash the whole ftq, head and tail should be the same
    auto new_fetch_demand_target_id = new_enq_target_id;

    // Update enqueue state
    fetchTargetEnqState.nextEnqTargetId = new_enq_target_id;
    fetchTargetEnqState.streamId = new_enq_stream_id;
    fetchTargetEnqState.pc = new_enq_pc;

    // Reset supply state
    supplyFetchTargetState.valid = false;
    supplyFetchTargetState.entry = nullptr;
    fetchDemandTargetId = new_fetch_demand_target_id;

    DPRINTF(DecoupleBP,
            "FTQ demand stream ID update to %lu, ftqEnqPC update to "
            "%#lx, fetch demand target Id updated to %lu\n",
            new_enq_stream_id, new_enq_pc, fetchDemandTargetId);
}

/**
 * @brief Check if a fetch target is available for the current demand
 *
 * This method checks if there is a valid fetch target entry that
 * matches the current demand target ID.
 *
 * @return true if a matching target is available, false otherwise
 */
bool
FetchTargetQueue::fetchTargetAvailable() const
{
    return supplyFetchTargetState.valid &&
           supplyFetchTargetState.targetId == fetchDemandTargetId;
}

/**
 * @brief Get the currently available fetch target
 *
 * @return Reference to the current fetch target entry
 * @pre fetchTargetAvailable() must be true
 */
FtqEntry&
FetchTargetQueue::getTarget()
{
    assert(fetchTargetAvailable());
    return *supplyFetchTargetState.entry;
}

/**
 * @brief Mark the current fetch target as finished and advance to the next
 *
 * This method is called when the fetch unit has consumed the current
 * fetch target. It removes the entry from the queue and advances the
 * demand target ID.
 */
void
FetchTargetQueue::finishCurrentFetchTarget()
{
    ++fetchDemandTargetId;  // Move to next target
    ftq.erase(supplyFetchTargetState.targetId);  // Remove current target from queue
    supplyFetchTargetState.valid = false;  // Invalidate supply state
    supplyFetchTargetState.entry = nullptr;

    DPRINTF(DecoupleBP,
            "Finish current fetch target: %lu, inc demand to %lu\n",
            supplyFetchTargetState.targetId, fetchDemandTargetId);
}

/**
 * @brief Try to supply fetch with a target matching the demand PC
 *
 * This method attempts to find a fetch target that matches the current
 * demand target ID. If found, it updates the supply state to provide
 * this target to the fetch unit.
 *
 * @param fetch_demand_pc The PC that fetch is requesting
 * @param in_loop Output parameter set to true if the target is in a loop
 * @return true if a target was successfully located and supplied
 */
bool
FetchTargetQueue::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop)
{
    // If we don't already have a valid supply state matching the demand
    if (!supplyFetchTargetState.valid ||
        supplyFetchTargetState.targetId != fetchDemandTargetId) {
        // Try to find the target in the queue
        auto it = ftq.find(fetchDemandTargetId);
        if (it != ftq.end()) {
            // Special case: fetch PC is already past the end of this target
            assert("fetch_demand_pc < it->second.endPC" && fetch_demand_pc < it->second.endPC);

            // Update supply state with found target
            DPRINTF(DecoupleBP,
                    "Found ftq entry with id %lu, writing to "
                    "fetchReadFtqEntryBuffer\n",
                    fetchDemandTargetId);
            supplyFetchTargetState.valid = true;
            supplyFetchTargetState.targetId = fetchDemandTargetId;
            supplyFetchTargetState.entry = &(it->second);
            return true;
        } else {
            // Target not found in queue
            DPRINTF(DecoupleBP, "Target id %lu not found\n",
                    fetchDemandTargetId);
            if (!ftq.empty()) {
                // Sanity check: demand ID should not be less than smallest entry
                --it;
                DPRINTF(DecoupleBP, "Last entry of target queue: %lu\n",
                        it->first);
                if (it->first > fetchDemandTargetId) {
                    dump("targets in buffer goes beyond demand\n");
                }
                assert(it->first < fetchDemandTargetId);
            }
            in_loop = false;
            return false;
        }
    }

    // Already have valid supply state
    DPRINTF(DecoupleBP,
            "FTQ supplying, valid: %u, supply id: %lu, demand id: %lu\n",
            supplyFetchTargetState.valid, supplyFetchTargetState.targetId,
            fetchDemandTargetId);
    return true;
}

/**
 * @brief Get the iterator for the currently demanded target
 *
 * @return Pair containing a boolean (true if found) and the iterator
 */
std::pair<bool, FetchTargetQueue::FTQIt>
FetchTargetQueue::getDemandTargetIt()
{
    FTQIt it = ftq.find(fetchDemandTargetId);
    return std::make_pair(it != ftq.end(), it);
}

/**
 * @brief Add a new entry to the queue
 *
 * This method adds a new fetch target entry to the queue and
 * advances the next enqueue target ID.
 *
 * @param entry The fetch target entry to add
 */
void
FetchTargetQueue::enqueue(FtqEntry entry)
{
    DPRINTF(DecoupleBP, "Enqueueing target %lu with pc %#lx and stream %lu\n",
            fetchTargetEnqState.nextEnqTargetId, entry.startPC, entry.fsqID);
    ftq[fetchTargetEnqState.nextEnqTargetId] = entry;
    ++fetchTargetEnqState.nextEnqTargetId;
}

/**
 * @brief Print debug information about the queue
 *
 * Dumps the contents of the queue for debugging purposes.
 *
 * @param when String describing when the dump was triggered
 */
void
FetchTargetQueue::dump(const char* when)
{
    DPRINTF(DecoupleBPProbe, "%s, dump FTQ\n", when);
    for (auto it = ftq.begin(); it != ftq.end(); ++it) {
        DPRINTFR(DecoupleBPProbe, "FTQ entry: %lu, start pc: %#lx, end pc: %#lx, stream ID: %lu\n",
                 it->first, it->second.startPC, it->second.endPC, it->second.fsqID);
    }
}

/**
 * @brief Check if the supply fetch target state is valid
 *
 * @return true if there is a valid target in the supply state
 */
bool
FetchTargetQueue::validSupplyFetchTargetState() const
{
    return supplyFetchTargetState.valid;
}

/**
 * @brief Reset the program counter for the enqueue state
 *
 * This method is used when changing the PC but not doing a full squash.
 *
 * @param new_pc New program counter value
 */
void
FetchTargetQueue::resetPC(Addr new_pc)
{
    supplyFetchTargetState.valid = false;
    fetchTargetEnqState.pc = new_pc;
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

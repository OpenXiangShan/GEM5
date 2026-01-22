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
    fetchTargetEnqState.nextEnqTargetId = 0;
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

    // Update enqueue state
    fetchTargetEnqState.nextEnqTargetId = new_enq_target_id;
    fetchTargetEnqState.streamId = new_enq_stream_id;
    fetchTargetEnqState.pc = new_enq_pc;

    DPRINTF(DecoupleBP,
            "FTQ demand stream ID update to %lu, ftqEnqPC update to "
            "%#lx, next enq target Id updated to %lu\n",
            new_enq_stream_id, new_enq_pc, fetchTargetEnqState.nextEnqTargetId);
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
    return !ftq.empty();
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
    return ftq.front().second;
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
    assert(!ftq.empty());
    auto finished_id = ftq.front().first;
    ftq.pop_front();

    DPRINTF(DecoupleBP,
            "Finish current fetch target: %lu\n",
            finished_id);
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
    ftq.emplace_back(fetchTargetEnqState.nextEnqTargetId, entry);
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
    for (const auto &[id, e] : ftq) {
        DPRINTFR(DecoupleBPProbe,
                 "FTQ entry: %lu, start pc: %#lx, end pc: %#lx, stream ID: %lu\n",
                 id, e.startPC, e.endPC, e.fsqID);
    }
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
    fetchTargetEnqState.pc = new_pc;
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

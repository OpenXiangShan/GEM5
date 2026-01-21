#ifndef __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__
#define __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__

#include <deque>
#include <utility>

#include "cpu/pred/btb/stream_struct.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

/**
 * @brief State for enqueueing fetch targets
 *
 * This structure maintains state information needed when adding
 * new entries to the Fetch Target Queue (FTQ).
 */
struct FetchTargetEnqState
{
    Addr pc;                    // Current program counter for enqueue operations
    FetchStreamId streamId;     // Current fetch stream ID, associated with the fsq entry
    FetchTargetId nextEnqTargetId; // Next target ID to be assigned/ enqueue id
    FetchTargetEnqState() : pc(0), streamId(1), nextEnqTargetId(0) {}
};

/**
 * @brief Fetch Target Queue
 *
 * The FTQ holds fetch targets which represent decoded instruction streams.
 * It serves as an interface between branch prediction (which produces targets)
 * and the fetch unit (which consumes them). The FTQ helps implement decoupled
 * branch prediction in a processor pipeline.
 *
 * Key functions:
 * 1. Enqueues new fetch targets from a fetch stream buffer
 * 2. Supplies the fetch unit with fetch targets
 * 3. Manages redirections after branch mispredictions
 */
class FetchTargetQueue
{
    // todo: move fetch target buffer here
    // 1. enqueue from fetch stream buffer
    // 2. supply fetch with fetch target head
    // 3. redirect fetch target head after squash
    using FTQEntry = std::pair<FetchTargetId, FtqEntry>;
    using FTQ = std::deque<FTQEntry>; // FIFO: (ftqId, entry)

    // use fetchTargetEnqState.nextEnqTargetId to enqueue new entries
    FTQ ftq;              // The queue storage structure
    unsigned ftqSize;     // Maximum number of entries in the queue

    // State for enqueueing new fetch targets
    FetchTargetEnqState fetchTargetEnqState;

    std::string _name;

  public:
    /**
     * @brief Construct a new Fetch Target Queue
     *
     * @param size Maximum size of the queue
     */
    FetchTargetQueue(unsigned size);

    /**
     * @brief Squash all entries in the queue and reset state
     *
     * Called after a branch misprediction or other pipeline flush event
     *
     * @param new_enq_target_id New target ID to start enqueueing from
     * @param new_enq_stream_id New stream ID to associate with new entries
     * @param new_enq_pc New PC to start enqueueing from
     */
    void squash(FetchTargetId new_enq_target_id,
                FetchStreamId new_enq_stream_id, Addr new_enq_pc);

    /**
     * @brief Check if a fetch target is available for the current demand
     *
     * @return true if a target is available for the current fetchDemandTargetId
     */
    bool fetchTargetAvailable() const;

    /**
     * @brief Get the currently available fetch target
     *
     * @return Reference to the current fetch target entry
     */
    FtqEntry &getTarget();

    /**
     * @brief Get the enqueue state
     *
     * @return Reference to the enqueue state
     */
    FetchTargetEnqState &getEnqState() { return fetchTargetEnqState; }

    /**
     * @brief Get the ID of the target currently being supplied
     *
     * @return The target ID that is currently being supplied
     */
    FetchTargetId getSupplyingTargetId()
    {
        if (!ftq.empty())
            return ftq.front().first;
        return fetchTargetEnqState.nextEnqTargetId;
    }

    /**
     * @brief Get the stream ID of the target currently being supplied
     *
     * @return The stream ID of the target currently being supplied
     */
    FetchStreamId getSupplyingStreamId()
    {
        if (!ftq.empty())
            return ftq.front().second.fsqID;
        return fetchTargetEnqState.streamId;
    }

    /**
     * @brief Mark the current fetch target as finished
     *
     * This advances to the next fetch target and removes the current one
     */
    void finishCurrentFetchTarget();


    bool empty() const { return ftq.empty(); }

    unsigned size() const { return ftq.size(); }

    bool full() const { return ftq.size() >= ftqSize; }

    /**
     * @brief Add a new entry to the queue
     *
     * @param entry The entry to add
     */
    void enqueue(FtqEntry entry);

    /**
     * @brief Print debug information about the queue
     *
     * @param when String describing when the dump was triggered
     */
    void dump(const char *when);

    const std::string &name() const { return _name; }

    void setName(const std::string &parent) { _name = parent + ".ftq"; }

    /**
     * @brief Get the last entry inserted into the queue
     *
     * @return Reference to the most recently inserted entry
     */
    FtqEntry &getLastInsertedEntry() { return ftq.back().second; }

    void resetPC(Addr new_pc);
};

}
}
}

#endif  // __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__

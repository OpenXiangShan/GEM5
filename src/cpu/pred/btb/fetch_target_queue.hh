#ifndef __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__
#define __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__

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
 * @brief State for reading fetch targets
 *
 * This structure maintains state information when reading entries
 * from the Fetch Target Queue (FTQ).
 */
struct FetchTargetReadState
{
    bool valid;          // Whether this state contains valid data
    FetchTargetId targetId; // ID of the target being read, if valid, equals to fetchDemandTargetId
    FtqEntry *entry;     // Pointer to the entry being read
    FetchTargetReadState() : valid(false), targetId(0), entry(nullptr) {}
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
    using FTQ = std::map<FetchTargetId, FtqEntry>; // inorder map: id -> entry
    using FTQIt = FTQ::iterator;

    // use fetchTargetEnqState.nextEnqTargetId to enqueue new entries
    // use fetchDemandTargetId = supplyFetchTargetState.targetId to supply
    FTQ ftq;              // The queue storage structure
    unsigned ftqSize;     // Maximum number of entries in the queue
    FetchTargetId ftqId{0};  // Queue ID pointer for internal tracking

    // State for supplying fetch targets to the fetch unit
    FetchTargetReadState supplyFetchTargetState;

    // The target ID that fetch is currently demanding
    FetchTargetId fetchDemandTargetId{0};

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
        if (supplyFetchTargetState.valid) {
            return supplyFetchTargetState.targetId;
        } else {
            return fetchDemandTargetId;
        }
    }

    /**
     * @brief Get the stream ID of the target currently being supplied
     *
     * @return The stream ID of the target currently being supplied
     */
    FetchStreamId getSupplyingStreamId()
    {
        if (supplyFetchTargetState.valid) {
            return supplyFetchTargetState.entry->fsqID;
        } else if (!ftq.empty()) {
            return ftq.begin()->second.fsqID;
        } else {
            return fetchTargetEnqState.streamId;
        }
    }

    /**
     * @brief Mark the current fetch target as finished
     *
     * This advances to the next fetch target and removes the current one
     */
    void finishCurrentFetchTarget();

    // NEW: 2Fetch support methods
    /**
     * @brief Check if there is a next available FTQ entry
     *
     * @return true if next FTQ entry is available
     */
    bool hasNext() const;

    /**
     * @brief Peek at the next FTQ entry without consuming it
     *
     * @return Reference to the next FTQ entry
     */
    const FtqEntry& peekNext() const;

    /**
     * @brief Advance to the next FTQ entry without dequeuing current one
     *
     * Used for 2fetch when we want to process the next entry
     * while keeping the current one active
     */
    void advance();

    /**
     * @brief Try to supply fetch with a target matching the demand PC
     *
     * @param fetch_demand_pc The PC that fetch is demanding
     * @param in_loop Output parameter indicating if we're in a loop
     * @return true if a target was found and supplied
     */
    bool trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop);


    bool empty() const { return ftq.empty(); }

    unsigned size() const { return ftq.size(); }

    bool full() const { return ftq.size() >= ftqSize; }

    /**
     * @brief Get the iterator for the currently demanded target
     *
     * @return Pair containing a boolean (true if found) and the iterator
     */
    std::pair<bool, FTQIt> getDemandTargetIt();

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
     * @brief Check if the supply fetch target state is valid
     *
     * @return true if the state is valid
     */
    bool validSupplyFetchTargetState() const;

    /**
     * @brief Get the last entry inserted into the queue
     *
     * @return Reference to the most recently inserted entry
     */
    FtqEntry &getLastInsertedEntry() { return ftq.rbegin()->second; }

    void resetPC(Addr new_pc);
};

}
}
}

#endif  // __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__

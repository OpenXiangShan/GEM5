#include "cpu/pred/ftb/fetch_target_queue.hh"

#include "base/trace.hh"
#include "debug/DecoupleBP.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

FetchTargetQueue::FetchTargetQueue(unsigned size) :
 ftqSize(size)
{
    fetchTargetEnqState.pc = 0x80000000;
    fetchDemandTargetId = 0;
    supplyFetchTargetState.valid = false;
    currentLoopIter = 0;
}

void
FetchTargetQueue::squash(FetchTargetId new_enq_target_id,
                         FetchStreamId new_enq_stream_id, Addr new_enq_pc)
{
    ftq.clear();
    // Because we squash the whole ftq, head and tail should be the same
    auto new_fetch_demand_target_id = new_enq_target_id;

    fetchTargetEnqState.nextEnqTargetId = new_enq_target_id;
    fetchTargetEnqState.streamId = new_enq_stream_id;
    fetchTargetEnqState.pc = new_enq_pc;

    supplyFetchTargetState.valid = false;
    supplyFetchTargetState.entry = nullptr;
    fetchDemandTargetId = new_fetch_demand_target_id;
    currentLoopIter = 0;
    DPRINTF(DecoupleBP,
            "FTQ demand stream ID update to %lu, ftqEnqPC update to "
            "%#lx, fetch demand target Id updated to %lu\n",
            new_enq_stream_id, new_enq_pc, fetchDemandTargetId);
}

bool
FetchTargetQueue::fetchTargetAvailable() const
{
    return supplyFetchTargetState.valid &&
           supplyFetchTargetState.targetId == fetchDemandTargetId;
}

bool
FetchTargetQueue::fetchTargetAvailable(int nextN) const
{
    return ftq.find(fetchDemandTargetId + nextN) != ftq.end();
}

FtqEntry&
FetchTargetQueue::getTarget()
{
    assert(fetchTargetAvailable());
    return *supplyFetchTargetState.entry;
}

FtqEntry&
FetchTargetQueue::getTarget(int nextN)
{
    assert(fetchTargetAvailable(nextN));
    return ftq[fetchDemandTargetId + nextN];
}


void
FetchTargetQueue::finishCurrentFetchTarget()
{

    ++fetchDemandTargetId;
    ftq.erase(supplyFetchTargetState.targetId);
    supplyFetchTargetState.valid = false;
    supplyFetchTargetState.entry = nullptr;
    currentLoopIter = 0;
    DPRINTF(DecoupleBP,
            "Finish current fetch target: %lu, inc demand to %lu\n",
            supplyFetchTargetState.targetId, fetchDemandTargetId);
}

void
FetchTargetQueue::finishCurrentFetchTarget(int N)
{

    fetchDemandTargetId += N;
    ftq.erase(supplyFetchTargetState.targetId);
    supplyFetchTargetState.valid = false;
    supplyFetchTargetState.entry = nullptr;
    currentLoopIter = 0;
    DPRINTF(DecoupleBP,
            "Finish current fetch target: %lu, inc demand to %lu\n",
            supplyFetchTargetState.targetId, fetchDemandTargetId);
}

bool
FetchTargetQueue::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop)
{
    if (!supplyFetchTargetState.valid ||
        supplyFetchTargetState.targetId != fetchDemandTargetId) {
        auto it = ftq.find(fetchDemandTargetId);
        if (it != ftq.end()) {
            if (M5_UNLIKELY(fetch_demand_pc >= it->second.endPC)) {
                // This is a special case where the fetch demand pc is
                // already past the end of the ftq entry.
                // In this case, we should just finish the current ftq
                // entry and supply the fetch with the next ftq entry.
                DPRINTF(DecoupleBP,
                        "Skip ftq entry %lu: [%#lx, %#lx),", it->first,
                        it->second.startPC, it->second.endPC);

                ++fetchDemandTargetId;
                it = ftq.erase(it);
                if (it == ftq.end()) {
                    in_loop = false;
                    return false;
                }
                DPRINTFR(DecoupleBP,
                        " use %lu: [%#lx, %#lx) instead. because demand pc "
                        "past the first entry.\n",
                        it->first, it->second.startPC, it->second.endPC);
            }
            DPRINTF(DecoupleBP,
                    "Found ftq entry with id %lu, writing to "
                    "fetchReadFtqEntryBuffer\n",
                    fetchDemandTargetId);
            supplyFetchTargetState.valid = true;
            supplyFetchTargetState.targetId = fetchDemandTargetId;
            supplyFetchTargetState.entry = &(it->second);
            in_loop = it->second.inLoop;
            return true;
        } else {
            DPRINTF(DecoupleBP, "Target id %lu not found\n",
                    fetchDemandTargetId);
            if (!ftq.empty()) {
                // sanity check
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
    DPRINTF(DecoupleBP,
            "FTQ supplying, valid: %u, supply id: %u, demand id: %u\n",
            supplyFetchTargetState.valid, supplyFetchTargetState.targetId,
            fetchDemandTargetId);
    in_loop = supplyFetchTargetState.entry->inLoop;
    return true;
}


std::pair<bool, FetchTargetQueue::FTQIt>
FetchTargetQueue::getDemandTargetIt()
{
    FTQIt it = ftq.find(fetchDemandTargetId);
    return std::make_pair(it != ftq.end(), it);
}

void
FetchTargetQueue::enqueue(FtqEntry entry)
{
    DPRINTF(DecoupleBP, "Enqueueing target %lu with pc %#x and stream %lu\n",
            fetchTargetEnqState.nextEnqTargetId, entry.startPC, entry.fsqID);
    ftq[fetchTargetEnqState.nextEnqTargetId] = entry;
    ++fetchTargetEnqState.nextEnqTargetId;
}

void
FetchTargetQueue::dump(const char* when)
{
    DPRINTF(DecoupleBP, "%s, dump FTQ\n", when);
    for (auto it = ftq.begin(); it != ftq.end(); ++it) {
        DPRINTFR(DecoupleBP, "FTQ entry: %lu, start pc: %#x, end pc: %#lx, stream ID: %lu\n",
                 it->first, it->second.startPC, it->second.endPC, it->second.fsqID);
    }
}

bool
FetchTargetQueue::validSupplyFetchTargetState() const
{
    return supplyFetchTargetState.valid;
}

void
FetchTargetQueue::resetPC(Addr new_pc)
{
    supplyFetchTargetState.valid = false;
    fetchTargetEnqState.pc = new_pc;
}

}  // namespace stream_pred

}  // namespace branch_prediction

}  // namespace gem5
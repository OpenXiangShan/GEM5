#include "cpu/o3/mls_virtual_queue.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/IEW.hh"

namespace gem5
{

namespace o3
{

MlsVirtualQueue::MlsVirtualQueue(unsigned num_threads, unsigned capacity)
    : queueCapacity(capacity),
      queues(num_threads),
      entries(num_threads, std::vector<Entry>(capacity)),
      nextSlots(num_threads, 0)
{
}

bool
MlsVirtualQueue::canAllocate(ThreadID tid, unsigned count) const
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);
    return queues[tid].size() + count <= queueCapacity;
}

MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryBySlot(ThreadID tid, unsigned slot)
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

const MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryBySlot(ThreadID tid, unsigned slot) const
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    const auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryByInst(const DynInstPtr &inst)
{
    if (!inst || inst->threadNumber >= queues.size()) {
        return nullptr;
    }

    if (inst->hasMatrixMlsqSlot()) {
        auto *entry =
            findEntryBySlot(inst->threadNumber, inst->getMatrixMlsqSlot());
        if (entry && entry->robSeqNum == inst->seqNum) {
            return entry;
        }
    }

    return nullptr;
}

const MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryByInst(const DynInstPtr &inst) const
{
    if (!inst || inst->threadNumber >= queues.size()) {
        return nullptr;
    }

    if (inst->hasMatrixMlsqSlot()) {
        auto *entry =
            findEntryBySlot(inst->threadNumber, inst->getMatrixMlsqSlot());
        if (entry && entry->robSeqNum == inst->seqNum) {
            return entry;
        }
    }

    return nullptr;
}

bool
MlsVirtualQueue::hasEntry(const DynInstPtr &inst) const
{
    return findEntryByInst(inst) != nullptr;
}

bool
MlsVirtualQueue::allocate(const DynInstPtr &inst)
{
    panic_if(!inst, "Attempted to allocate null matrix mem instruction");
    const ThreadID tid = inst->threadNumber;
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);

    if (hasEntry(inst) || !canAllocate(tid)) {
        return false;
    }

    const unsigned slot = nextSlots[tid];
    auto &entry = entries[tid][slot];
    panic_if(entry.allocated,
             "MLSQ slot still allocated [tid:%i] slot=%u [sn:%llu]",
             tid, slot, inst->seqNum);

    entry = {};
    entry.robSeqNum = inst->seqNum;
    entry.tid = tid;
    entry.slot = slot;
    entry.allocated = true;
    queues[tid].push_back(slot);
    nextSlots[tid] = (slot + 1) % queueCapacity;
    inst->setMatrixMlsqSlot(slot);

    DPRINTF(IEW,
            "MlsVirtualQueue alloc [tid:%i] [sn:%llu] slot=%u robOrder=%llu "
            "size=%u free=%u.\n",
            tid, inst->seqNum, entry.slot, entry.robSeqNum,
            static_cast<unsigned>(queues[tid].size()), freeEntries(tid));

    return true;
}

bool
MlsVirtualQueue::markFinished(const DynInstPtr &inst)
{
    auto *entry = findEntryByInst(inst);
    if (!entry) {
        return false;
    }

    entry->finished = true;
    DPRINTF(IEW,
            "MlsVirtualQueue finish [tid:%i] [sn:%llu] slot=%u robOrder=%llu.\n",
            entry->tid, inst->seqNum, entry->slot, entry->robSeqNum);
    return true;
}

unsigned
MlsVirtualQueue::retireCommitted(ThreadID tid, InstSeqNum committed_seq)
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);

    auto &queue = queues[tid];
    unsigned retired = 0;
    while (!queue.empty()) {
        auto &head = entries[tid][queue.front()];
        if (!head.finished || head.robSeqNum > committed_seq) {
            break;
        }

        DPRINTF(IEW,
                "MlsVirtualQueue free [tid:%i] [sn:%llu] slot=%u "
                "robOrder=%llu committed=%llu.\n",
                tid, head.robSeqNum, head.slot, head.robSeqNum, committed_seq);
        head = {};
        queue.pop_front();
        retired++;
    }

    return retired;
}

unsigned
MlsVirtualQueue::squash(ThreadID tid, InstSeqNum squash_seq)
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);

    auto &queue = queues[tid];
    unsigned canceled = 0;
    while (!queue.empty()) {
        auto &tail = entries[tid][queue.back()];
        if (tail.robSeqNum <= squash_seq) {
            break;
        }

        DPRINTF(IEW,
                "MlsVirtualQueue cancel [tid:%i] [sn:%llu] slot=%u "
                "robOrder=%llu squash=%llu.\n",
                tid, tail.robSeqNum, tail.slot, tail.robSeqNum, squash_seq);
        tail = {};
        queue.pop_back();
        canceled++;
    }

    if (canceled != 0) {
        nextSlots[tid] =
            (nextSlots[tid] + queueCapacity - (canceled % queueCapacity)) %
            queueCapacity;
    }

    return canceled;
}

unsigned
MlsVirtualQueue::freeEntries(ThreadID tid) const
{
    return queueCapacity - size(tid);
}

unsigned
MlsVirtualQueue::size(ThreadID tid) const
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);
    return queues[tid].size();
}

} // namespace o3
} // namespace gem5

#include "cpu/o3/mls_replay_queue.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/IEW.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace o3
{

MlsReplayQueue::MlsReplayQueue(unsigned num_threads, unsigned capacity,
                               Tick replay_select_latency)
    : queueCapacity(capacity),
      replaySelectLatency(replay_select_latency),
      entries(num_threads, std::vector<Entry>(capacity))
{
}

MlsReplayQueue::Entry *
MlsReplayQueue::findEntryBySlot(ThreadID tid, unsigned slot)
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

const MlsReplayQueue::Entry *
MlsReplayQueue::findEntryBySlot(ThreadID tid, unsigned slot) const
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    const auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

MlsReplayQueue::Entry *
MlsReplayQueue::findEntryByInst(const DynInstPtr &inst)
{
    if (!inst || inst->threadNumber >= entries.size() ||
        !inst->hasMatrixMlsReplaySlot()) {
        return nullptr;
    }

    auto *entry =
        findEntryBySlot(inst->threadNumber, inst->getMatrixMlsReplaySlot());
    if (entry && entry->robSeqNum == inst->seqNum) {
        return entry;
    }

    return nullptr;
}

const MlsReplayQueue::Entry *
MlsReplayQueue::findEntryByInst(const DynInstPtr &inst) const
{
    if (!inst || inst->threadNumber >= entries.size() ||
        !inst->hasMatrixMlsReplaySlot()) {
        return nullptr;
    }

    auto *entry =
        findEntryBySlot(inst->threadNumber, inst->getMatrixMlsReplaySlot());
    if (entry && entry->robSeqNum == inst->seqNum) {
        return entry;
    }

    return nullptr;
}

bool
MlsReplayQueue::hasEntry(const DynInstPtr &inst) const
{
    return findEntryByInst(inst) != nullptr;
}

const MlsReplayQueue::ReplayState *
MlsReplayQueue::getState(const DynInstPtr &inst) const
{
    const auto *entry = findEntryByInst(inst);
    return entry ? &entry->state : nullptr;
}

std::optional<unsigned>
MlsReplayQueue::allocateSlot(ThreadID tid)
{
    for (unsigned slot = 0; slot < queueCapacity; ++slot) {
        if (!entries[tid][slot].allocated) {
            return slot;
        }
    }
    return std::nullopt;
}

bool
MlsReplayQueue::allocateOrUpdate(
    const DynInstPtr &inst, const ReplayState &state, bool ready)
{
    panic_if(!inst, "Attempted to allocate null matrix replay entry");
    const ThreadID tid = inst->threadNumber;
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    if (auto *entry = findEntryByInst(inst)) {
        entry->scheduled = false;
        entry->ready = ready;
        entry->availableTick = ready ? curTick() + replaySelectLatency : 0;
        entry->state = state;
        DPRINTF(IEW,
                "MlsReplayQueue retry-arm [tid:%i] [sn:%llu] slot=%u "
                "robOrder=%llu ready=%d vaddr=%#llx.\n",
                tid, inst->seqNum, entry->slot, entry->robSeqNum,
                ready, state.vaddr);
        return true;
    }

    auto slot = allocateSlot(tid);
    if (!slot) {
        return false;
    }

    auto &entry = entries[tid][*slot];
    entry = {};
    entry.allocated = true;
    entry.scheduled = false;
    entry.ready = ready;
    entry.availableTick = ready ? curTick() + replaySelectLatency : 0;
    entry.robSeqNum = inst->seqNum;
    entry.tid = tid;
    entry.slot = *slot;
    entry.inst = inst;
    entry.state = state;
    inst->setMatrixMlsReplaySlot(*slot);

    DPRINTF(IEW,
            "MlsReplayQueue alloc [tid:%i] [sn:%llu] slot=%u robOrder=%llu "
            "ready=%d vaddr=%#llx stride=%#llx tile0=%#llx tile1=%#llx.\n",
            tid, inst->seqNum, entry.slot, entry.robSeqNum,
            ready, state.vaddr, state.stride, state.tile0, state.tile1);
    return true;
}

void
MlsReplayQueue::refreshReady(
    ThreadID tid, const std::function<bool(const ReplayState &)> &ready_fn)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.scheduled) {
            continue;
        }

        const bool was_ready = entry.ready;
        entry.ready = ready_fn(entry.state);
        if (entry.ready && !was_ready) {
            entry.availableTick = curTick() + replaySelectLatency;
        }
    }
}

bool
MlsReplayQueue::scheduleNext(ThreadID tid, DynInstPtr &inst_out)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    Entry *selected = nullptr;
    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.scheduled || !entry.ready ||
            entry.availableTick > curTick()) {
            continue;
        }
        if (!selected || entry.robSeqNum < selected->robSeqNum) {
            selected = &entry;
        }
    }

    if (!selected) {
        return false;
    }

    selected->scheduled = true;
    inst_out = selected->inst;

    DPRINTF(IEW,
            "MlsReplayQueue schedule [tid:%i] [sn:%llu] slot=%u robOrder=%llu.\n",
            tid, selected->robSeqNum, selected->slot, selected->robSeqNum);
    return true;
}

void
MlsReplayQueue::freeEntry(Entry &entry)
{
    if (entry.inst) {
        entry.inst->clearMatrixMlsReplaySlot();
    }

    DPRINTF(IEW,
            "MlsReplayQueue free [tid:%i] [sn:%llu] slot=%u robOrder=%llu.\n",
            entry.tid, entry.robSeqNum, entry.slot, entry.robSeqNum);
    entry = {};
}

bool
MlsReplayQueue::completeRetry(const DynInstPtr &inst)
{
    auto *entry = findEntryByInst(inst);
    if (!entry) {
        return false;
    }

    freeEntry(*entry);
    return true;
}

unsigned
MlsReplayQueue::squash(ThreadID tid, InstSeqNum squash_seq)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    unsigned canceled = 0;
    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.robSeqNum <= squash_seq) {
            continue;
        }

        DPRINTF(IEW,
                "MlsReplayQueue cancel [tid:%i] [sn:%llu] slot=%u robOrder=%llu squash=%llu.\n",
                tid, entry.robSeqNum, entry.slot, entry.robSeqNum, squash_seq);
        freeEntry(entry);
        canceled++;
    }
    return canceled;
}

unsigned
MlsReplayQueue::size(ThreadID tid) const
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);
    unsigned total = 0;
    for (const auto &entry : entries[tid]) {
        total += entry.allocated ? 1 : 0;
    }
    return total;
}

unsigned
MlsReplayQueue::freeEntries(ThreadID tid) const
{
    return queueCapacity - size(tid);
}

} // namespace o3
} // namespace gem5

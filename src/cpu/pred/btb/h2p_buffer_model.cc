#include "cpu/pred/btb/h2p_buffer_model.hh"

#include <algorithm>
#include <cassert>

namespace gem5::branch_prediction::btb_pred
{

H2PBufferModel::H2PBufferModel(unsigned capacity)
    : bufferCapacity(capacity)
{
    assert(capacity > 0);
}

H2PBufferModel::AddResult
H2PBufferModel::add(ThreadID tid, FetchTargetId ftqId, Addr pc)
{
    const Key key{tid, ftqId, pc};
    if (entries.find(key) != entries.end())
        return {};

    const bool admitted = admittedEntries < bufferCapacity;
    entries.emplace(key, Entry{admitted});
    if (admitted) {
        ++admittedEntries;
        peakOccupancy = std::max(peakOccupancy, admittedEntries);
    }

    return {true, admitted, !admitted};
}

H2PBufferModel::ResolveResult
H2PBufferModel::resolve(const BranchOutcome &branch)
{
    const Key key{branch.tid, branch.ftqId, branch.pc};
    auto it = entries.find(key);
    if (it == entries.end()) {
        const auto resolved = resolvedEntries.find(key);
        if (resolved != resolvedEntries.end())
            return {true, resolved->second.admitted};
        return {};
    }

    const bool admitted = it->second.admitted;
    if (admitted)
        --admittedEntries;
    resolvedEntries.emplace(key, it->second);
    entries.erase(it);
    return {true, admitted};
}

H2PBufferModel::ResolveResult
H2PBufferModel::commit(const BranchOutcome &branch)
{
    const Key key{branch.tid, branch.ftqId, branch.pc};
    auto resolved = resolvedEntries.find(key);
    if (resolved != resolvedEntries.end()) {
        const bool admitted = resolved->second.admitted;
        resolvedEntries.erase(resolved);
        return {true, admitted};
    }

    // Fall back to an unresolved entry if its resolve update was unavailable.
    auto it = entries.find(key);
    if (it == entries.end())
        return {};

    const bool admitted = it->second.admitted;
    if (admitted)
        --admittedEntries;
    entries.erase(it);
    return {true, admitted};
}

unsigned
H2PBufferModel::squashAfter(FetchTargetId targetId, ThreadID tid)
{
    unsigned removed = 0;
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->first.tid == tid && it->first.ftqId > targetId) {
            if (it->second.admitted) {
                --admittedEntries;
                ++removed;
            }
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = resolvedEntries.begin();
         it != resolvedEntries.end();) {
        if (it->first.tid == tid && it->first.ftqId > targetId) {
            it = resolvedEntries.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

unsigned
H2PBufferModel::squashTargetExcept(FetchTargetId targetId, ThreadID tid,
                                    Addr keepPc)
{
    unsigned removed = 0;
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->first.tid == tid && it->first.ftqId == targetId &&
            it->first.pc != keepPc) {
            if (it->second.admitted) {
                --admittedEntries;
                ++removed;
            }
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = resolvedEntries.begin();
         it != resolvedEntries.end();) {
        if (it->first.tid == tid && it->first.ftqId == targetId &&
            it->first.pc != keepPc) {
            it = resolvedEntries.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

unsigned
H2PBufferModel::clear(ThreadID tid)
{
    unsigned removed = 0;
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->first.tid == tid) {
            if (it->second.admitted) {
                --admittedEntries;
                ++removed;
            }
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = resolvedEntries.begin();
         it != resolvedEntries.end();) {
        if (it->first.tid == tid) {
            it = resolvedEntries.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

} // namespace gem5::branch_prediction::btb_pred

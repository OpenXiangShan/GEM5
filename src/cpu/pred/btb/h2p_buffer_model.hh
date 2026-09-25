#ifndef __CPU_PRED_BTB_H2P_BUFFER_MODEL_HH__
#define __CPU_PRED_BTB_H2P_BUFFER_MODEL_HH__

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "base/types.hh"
#include "cpu/pred/btb/branch_outcome.hh"

namespace gem5::branch_prediction::btb_pred
{

/**
 * Metadata-only model of APF alternate path buffer occupancy.
 *
 * It tracks admitted H2P branches until resolution. Resolved metadata remains
 * available until commit so final-path statistics can exclude branches later
 * removed by an older squash. The model is intentionally performance-neutral:
 * capacity rejection is recorded, but it does not stall the main fetch/decode
 * pipeline.
 */
class H2PBufferModel
{
  public:
    static constexpr unsigned UopsPerBuffer = 104;
    static constexpr unsigned BytesPerBuffer = 800;

    struct AddResult
    {
        bool added = false;
        bool admitted = false;
        bool rejectedFull = false;
    };

    struct ResolveResult
    {
        bool found = false;
        bool admitted = false;
    };

    explicit H2PBufferModel(unsigned capacity);

    AddResult add(ThreadID tid, FetchTargetId ftqId, Addr pc);
    ResolveResult resolve(const BranchOutcome &branch);
    ResolveResult commit(const BranchOutcome &branch);

    unsigned squashAfter(FetchTargetId targetId, ThreadID tid);
    unsigned squashTargetExcept(FetchTargetId targetId, ThreadID tid,
                                Addr keepPc);
    unsigned clear(ThreadID tid);

    unsigned capacity() const { return bufferCapacity; }
    unsigned occupancy() const { return admittedEntries; }
    unsigned maxOccupancy() const { return peakOccupancy; }

  private:
    struct Key
    {
        ThreadID tid = 0;
        FetchTargetId ftqId = 0;
        Addr pc = 0;

        bool operator==(const Key &other) const
        {
            return tid == other.tid && ftqId == other.ftqId && pc == other.pc;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key &key) const
        {
            size_t hash = std::hash<uint64_t>{}(key.ftqId);
            hash ^= std::hash<uint64_t>{}(key.pc) + 0x9e3779b9 +
                (hash << 6) + (hash >> 2);
            hash ^= std::hash<unsigned>{}(key.tid) + 0x9e3779b9 +
                (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct Entry
    {
        bool admitted = false;
    };

    const unsigned bufferCapacity;
    unsigned admittedEntries = 0;
    unsigned peakOccupancy = 0;
    std::unordered_map<Key, Entry, KeyHash> entries;
    std::unordered_map<Key, Entry, KeyHash> resolvedEntries;
};

} // namespace gem5::branch_prediction::btb_pred

#endif // __CPU_PRED_BTB_H2P_BUFFER_MODEL_HH__

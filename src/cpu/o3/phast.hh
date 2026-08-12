/*
 * Copyright (c) 2026
 * All rights reserved.
 */

#ifndef __CPU_O3_PHAST_HH__
#define __CPU_O3_PHAST_HH__

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

struct PHASTPredictionResult
{
    std::pair<std::ptrdiff_t, std::ptrdiff_t> storeQueueDistances{-1, -1};
    unsigned predBranchHistLength = 0;
    uint64_t predictorHash = 0;
};

class PHAST
{
  public:
    PHAST() = default;
    explicit PHAST(const BaseO3CPUParams &params) { init(params); }

    void init(const BaseO3CPUParams &params);
    void clear();

    PHASTPredictionResult checkInst(Addr load_pc, InstSeqNum load_seq_num,
                                    const BranchHistory &branch_history,
                                    bool is_load);

    void violation(Addr load_pc, InstSeqNum load_seq_num,
                   InstSeqNum store_seq_num, Addr store_pc,
                   std::ptrdiff_t store_queue_distance, bool predicted,
                   unsigned predicted_path_index, uint64_t predicted_hash,
                   const BranchHistory &branch_history);

    void commit(Addr load_pc, Addr load_addr, unsigned load_size,
                const std::pair<Addr, Addr> &store_addrs,
                const std::pair<unsigned, unsigned> &store_sizes,
                unsigned path_index, uint64_t predictor_hash);

    void squash(InstSeqNum, ThreadID) {}
    void issued(Addr, InstSeqNum, bool) {}
    void insertStore(Addr, InstSeqNum, ThreadID) {}
    void insertLoad(Addr, InstSeqNum) {}

    unsigned selectedTargetBits = 5;
    uint64_t selectedTargetMask = (1ULL << selectedTargetBits) - 1;

  private:
    class SimplBlockCache
    {
      private:
        struct Entry
        {
            uint64_t tag = 0;
            std::pair<std::ptrdiff_t, std::ptrdiff_t> distances{-1, -1};
            uint64_t lru = 0;
            uint32_t counter = 0;
            bool valid = false;
        };

        uint32_t setBits = 0;
        uint32_t tagBits = 0;
        uint32_t associativity = 0;
        uint64_t lruCounter = 0;
        uint32_t maxCounterValue = 0;
        uint32_t counterThreshold = 0;
        uint32_t counterIncrement = 0;
        uint32_t counterDecrement = 0;
        unsigned secondTargetMaxDistance = 0;
        std::vector<std::vector<Entry>> cache;

        uint64_t xorFold(uint64_t pc, uint64_t history, unsigned size) const;
        uint64_t getIndex(Addr pc, uint64_t history) const;
        uint64_t getTag(Addr pc, uint64_t history) const;
        Entry *findEntry(Addr pc, uint64_t history);
        const Entry *findEntry(Addr pc, uint64_t history) const;
        Entry *getLRUEntry(uint64_t set);
        void updateLRU(Entry *entry);

      public:
        int init(uint32_t set_bits, uint32_t _associativity, uint32_t tag_bits,
                 uint32_t max_counter_value, uint32_t counter_threshold,
                 uint32_t counter_increment,
                 uint32_t counter_decrement,
                 unsigned second_target_max_distance);
        std::pair<std::ptrdiff_t, std::ptrdiff_t> predict(Addr pc,
                                                          uint64_t history) const;
        void update(Addr pc, uint64_t history, std::ptrdiff_t distance);
        void updateCommit(Addr pc, uint64_t history, bool prediction_wrong);
        void clear();

        unsigned getSetBits() const { return setBits; }
        unsigned getTagBits() const { return tagBits; }
    };

    unsigned depCheckShift = 0;
    unsigned SQEntries = 0;
    std::vector<unsigned> historySizes;
    std::vector<SimplBlockCache> paths;

    uint64_t makePathHash(Addr load_pc, const BranchHistory &branch_history,
                          unsigned history_len) const;
    BranchHistory filteredHistory(InstSeqNum load_seq_num,
                                  const BranchHistory &branch_history) const;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_PHAST_HH__

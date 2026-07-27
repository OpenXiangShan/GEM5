#pragma once
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Cache.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace xsCHI
{
    class TxnIDManager
    {
    public:
        static constexpr uint32_t InvalidTxnId = static_cast<uint32_t>(-1);
        static constexpr Tick TxnHoldPanicCycles = 50000000;

        TxnIDManager(int max_outstanding)
            : max_outstanding(max_outstanding),
              next_id(0),
              next_untrack_id(max_outstanding)
        {
            panic_if(max_outstanding <= 0,
                     "TxnIDManager requires max_outstanding > 0, got %d",
                     max_outstanding);
            panic_if(max_outstanding >= max_ids,
                     "TxnIDManager requires max_outstanding < %d, got %d",
                     max_ids, max_outstanding);
            DPRINTF(Cache,"TxnManagerInit,size:%d\n",used_ids.size());
            used_ids.reset();
            untrack_used_ids.reset();
        }

        // 获取一个可用的TxnID，返回-1表示无可用ID
        int getID() {
            checkOldestTxnAge();
            if (used_ids.count() < max_outstanding){
                for (int i = 0; i < max_outstanding; ++i) {
                    int candidate = (next_id + i) % max_outstanding;
                    if (!used_ids.test(candidate)) {
                        used_ids.set(candidate);
                        next_id = (candidate + 1) % max_outstanding;
                        const Tick now = curTick();
                        allocTickById[candidate] = now;
                        allocOrder.emplace_back(candidate, now);
                        return candidate;
                    }
                }
            }

            return -1; // 没有可用ID
        }

        // 获取一个不受数量限制的TxnID，用来处理不需要跟踪的事务，例如L3的写回请求
        int getUntrackID() {
            checkOldestTxnAge();
            assert(max_outstanding < max_ids &&
                   "max_outstanding must be less than max_ids for untrack ID allocation");
            const int untrack_space = max_ids - max_outstanding;
            for (int i = 0; i < untrack_space; ++i) {
                int candidate = (next_untrack_id + i) % max_ids;
                if (candidate < max_outstanding) {
                    continue;
                }
                if (!untrack_used_ids.test(candidate)) {
                    untrack_used_ids.set(candidate);
                    next_untrack_id = (candidate + 1) % max_ids;
                    if (next_untrack_id < max_outstanding) {
                        next_untrack_id = max_outstanding; // Skip IDs reserved for tracked transactions
                    }
                    return candidate;
                }
            }
            return InvalidTxnId;

        }
        // 释放一个TxnID（收到所有响应或RetryAck后调用）
        void releaseID(int id) {
            checkOldestTxnAge();
            assert (id >= 0 && id < max_ids);
            if (id >= max_outstanding) {
                // 这是一个untrack ID
                assert(untrack_used_ids.test(id) && "Untrack ID not in use");
                untrack_used_ids.reset(id);
                return;
            }
            assert(used_ids.test(id) && "ID not in use");
            used_ids.reset(id);
            allocTickById.erase(id);
        }

        // 判断ID是否被占用
        bool isUsed(int id) {
            assert(id >= 0 && id < max_ids);
            if (id < max_outstanding){
                return used_ids.test(id);
            }else{
                return untrack_used_ids.test(id);
            }

        }

    private:
        void checkOldestTxnAge()
        {
            const Tick now = curTick();
            const Tick cpu_clock_period = getCpuClockPeriod();

            // Drop stale queue entries that were already released or reallocated.
            while (!allocOrder.empty()) {
                const int id = allocOrder.front().first;
                const Tick tick = allocOrder.front().second;
                auto it = allocTickById.find(id);
                if (it == allocTickById.end() || it->second != tick) {
                    allocOrder.pop_front();
                    continue;
                }

                const Tick age = (now - tick)/cpu_clock_period;
                panic_if(age > TxnHoldPanicCycles,
                         "TxnIDManager monitor: oldest txn id=%d held for %llu cycles (limit=%llu), now=%llu alloc=%llu",
                         id,
                         static_cast<unsigned long long>(age),
                         static_cast<unsigned long long>(TxnHoldPanicCycles),
                         static_cast<unsigned long long>(now/cpu_clock_period),
                         static_cast<unsigned long long>(tick/cpu_clock_period));
                break;
            }
        }

        static constexpr int max_ids = 4096; // 12-bit
        int max_outstanding = 1024;
        std::bitset<max_ids> used_ids;
        std::bitset<max_ids> untrack_used_ids;
        int next_id;
        int next_untrack_id;
        std::unordered_map<int, Tick> allocTickById;
        std::deque<std::pair<int, Tick>> allocOrder;
    };

}}

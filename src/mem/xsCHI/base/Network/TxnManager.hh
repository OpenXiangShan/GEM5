#pragma once
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "base/trace.hh"
#include "debug/Cache.hh"

namespace gem5
{

namespace xsCHI
{
    class TxnIDManager
    {
    public:
        TxnIDManager(int max_outstanding) : max_outstanding(max_outstanding), next_id(0) {
            DPRINTF(Cache,"TxnManagerInit,size:%d\n",used_ids.size());
            used_ids.reset();
        }

        // 获取一个可用的TxnID，返回-1表示无可用ID
        int getID() {
            if (used_ids.count() < max_outstanding){
               for (int i = 0; i < max_ids; ++i) {
                int candidate = (next_id + i) % max_ids;
                if (!used_ids.test(candidate)) {
                    used_ids.set(candidate);
                    next_id = (candidate + 1) % max_ids;
                    return candidate;
                }
                }
            }

            return -1; // 没有可用ID
        }

        // 释放一个TxnID（收到所有响应或RetryAck后调用）
        void releaseID(int id) {
            if (id >= 0 && id < max_ids)
                used_ids.reset(id);
        }

        // 判断ID是否被占用
        bool isUsed(int id) {
            if (id >= 0 && id < max_ids)
                return used_ids.test(id);
            return false;
        }

    private:
        static constexpr int max_ids = 4096; // 12-bit
        int max_outstanding = 1024;
        std::bitset<max_ids> used_ids;
        int next_id;
    };

}}

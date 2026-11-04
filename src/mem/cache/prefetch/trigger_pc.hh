// Copyright (c) 2026 Institute of Computing Technology, CAS
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __MEM_CACHE_PREFETCH_TRIGGER_PC_HH__
#define __MEM_CACHE_PREFETCH_TRIGGER_PC_HH__

#include "mem/request.hh"

namespace gem5
{
namespace prefetch
{

/** Snapshot the original instruction before the request PC can change. */
class TriggerPC
{
  private:
    Addr pc = 0;
    bool valid = false;

  public:
    explicit TriggerPC(const Request &request)
    {
        if (request.hasPrefetchTriggerPC()) {
            pc = request.getPrefetchTriggerPC();
            valid = true;
        } else if (request.hasPC()) {
            pc = request.getPC();
            valid = true;
        }
    }

    bool hasPC() const { return valid; }

    Addr getPC() const
    {
        assert(valid);
        return pc;
    }

    void tag(Request &request) const
    {
        if (valid)
            request.setPrefetchTriggerPC(pc);
    }
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_TRIGGER_PC_HH__

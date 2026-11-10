/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_HIERARCHY_POLICY_HH__
#define __MEM_CACHE_HIERARCHY_POLICY_HH__

#include "enums/CacheHierarchyPolicy.hh"
#include "mem/packet.hh"

namespace gem5
{
namespace cache_hierarchy
{

/** XiangShan single-core grants and victim routing, independent of replacement.
 *
 * Ordinary reads retain their request command. An explicit expectation prepares
 * the requesting MSHR before a lower cache may transfer deferred dirty ownership.
 * Real sharers, pending downgrades and ReadCleanReq retain the normal protocol.
 */
class Policy
{
  private:
    const enums::CacheHierarchyPolicy mode;

  public:
    explicit Policy(enums::CacheHierarchyPolicy policy) : mode(policy) {}

    bool xiangshanSingleCore() const
    {
        return mode == enums::xiangshan_single_core;
    }

    bool expectWritableRead(const PacketPtr pkt, bool read_only) const
    {
        return xiangshanSingleCore() && !read_only && pkt && pkt->req &&
            (pkt->cmd == MemCmd::ReadReq ||
             pkt->cmd == MemCmd::ReadSharedReq) &&
            !pkt->req->isUncacheable() && !pkt->req->isInstFetch() &&
            !pkt->req->isPrefetch();
    }

    bool allowDeferredWritable(const PacketPtr pkt) const
    {
        return xiangshanSingleCore() && pkt->writableFillExpected;
    }

    bool dropCleanL1Data(unsigned level) const
    {
        return xiangshanSingleCore() && level == 1;
    }

    bool captureBypassFill(unsigned level, const PacketPtr pkt) const
    {
        return xiangshanSingleCore() && level == 1 && pkt->bypassL2;
    }

    bool forwardBypassWriteback(unsigned level, const PacketPtr pkt) const
    {
        return xiangshanSingleCore() && level == 2 &&
            pkt->isWriteback() && pkt->bypassL2;
    }

    bool consumeBypassWriteback(unsigned level, const PacketPtr pkt) const
    {
        return xiangshanSingleCore() && level == 3 &&
            pkt->isWriteback() && pkt->bypassL2;
    }
};

} // namespace cache_hierarchy
} // namespace gem5

#endif // __MEM_CACHE_HIERARCHY_POLICY_HH__

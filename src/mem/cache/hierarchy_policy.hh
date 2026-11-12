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
 * the requesting MSHR before a lower cache may grant deferred writable access.
 * Resident L2 blocks retain dirty responsibility while L1 records its own
 * writes. Bypass fills may transfer dirty ownership to L1. Real sharers,
 * pending downgrades and ReadCleanReq retain the normal protocol.
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

    bool retainDirty(unsigned level) const
    {
        return xiangshanSingleCore() && level == 2;
    }

    bool retainDirtyOnGrant(unsigned level, const PacketPtr pkt,
                            bool bypass_fill = false) const
    {
        // Only actual bypass temporary fills opt out of L2 retention.
        // The grant path still checks whether writable can be passed.
        return retainDirty(level) && !bypass_fill && pkt && pkt->req &&
            pkt->fromCache() &&
            !pkt->req->isUncacheable() && !pkt->req->isInstFetch() &&
            !pkt->req->isPTWalk() && !pkt->isWrite() &&
            (pkt->isRead() || pkt->isUpgrade());
    }

    bool prepareWritableRead(unsigned level) const
    {
        // A read-only L1 transaction must not promise a dirty snoop response.
        // Actual writes still prepare their MSHR through needsWritable.
        return !xiangshanSingleCore() || level != 1;
    }

    bool dropCleanL1Data(unsigned level) const
    {
        return xiangshanSingleCore() && level == 1;
    }

    bool writebackBypassVictim(unsigned level, bool bypass_l2) const
    {
        return xiangshanSingleCore() && level == 1 && bypass_l2;
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

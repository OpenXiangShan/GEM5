/*
 * Copyright (c) 2023 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_RUBY_STRUCTURES_RUBY_PREFETCHER_WRAPPER_HH__
#define __MEM_RUBY_STRUCTURES_RUBY_PREFETCHER_WRAPPER_HH__

#include <unordered_map>

// #include "mem/cache/cache_probe_arg.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/ruby/slicc_interface/AbstractCacheEntry.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/slicc_interface/RubyRequest.hh"
#include "mem/ruby/slicc_interface/XsPFMetaData.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{
// Removed cache accessor
  using DataUpdate = BaseCache::DataUpdate;

class RubyPrefetcherProxy : public CacheAccessor, public Named
{
  public:

    RubyPrefetcherProxy(AbstractController* parent,
                        prefetch::Base* prefetcher,
                        MessageBuffer *pf_queue);

    /** Deschedled the ready prefetch event */
    void deschedulePrefetch();

    /** Notifies a completed prefetch request */
    void completePrefetch(Addr addr);

    /**
     * Notify PF probes hit/miss/fill
     */
    void notifyPfHit(const RequestPtr& req, bool is_read, XsPFMetaData& pfmeta,
                     const DataBlock& data_blk);

    void notifyPfMiss(const RequestPtr& req, bool is_read, XsPFMetaData& pfmeta,
                      const DataBlock& data_blk);

    void notifyPfFill(const RequestPtr& req, const DataBlock& data_blk,
                      bool from_pf);
    void notifyPfEvict(Addr blkAddr, bool hwPrefetched, XsPFMetaData& pfmeta,
                       RequestorID requestorID);

    void pfHitInCache(const XsPFMetaData& pfmeta);

    void notifyHitToDownStream(const RequestPtr& req);

    void offloadToDownStream();

    /** Registers probes. */
    void regProbePoints();

  private:

    /** Schedule the next ready prefetch */
    void scheduleNextPrefetch();

    /** Issue prefetch to the contoller prefetch queue */
    void issuePrefetch();

    /** Prefetcher from classic memory */
    prefetch::Base* prefetcher;

    /** Ruby cache controller */
    AbstractController* cacheCntrl;

    /** Prefetch queue to the cache controller */
    MessageBuffer* pfQueue;

    /** List of issued prefetch request packets */
    std::unordered_map<Addr, PacketPtr> issuedPfPkts;

    /** Prefetch event */
    EventFunctionWrapper pfEvent;

    /** To probe when a cache hit occurs */
    ProbePointArg<PacketPtr> *ppHit;

    /** To probe when a cache miss occurs */
    ProbePointArg<PacketPtr> *ppMiss;

    /** To probe when a cache fill occurs */
    ProbePointArg<PacketPtr> *ppFill;

    /**
     * To probe when the contents of a block are updated. Content updates
     * include data fills, overwrites, and invalidations, which means that
     * this probe partially overlaps with other probes.
     */



    ProbePointArg<DataUpdate> *ppDataUpdate;

  public:

    /** Accessor functions */

    bool inCache(Addr addr, bool is_secure) const override
    {
        return cacheCntrl->inCache(addr, is_secure);
    }

    virtual unsigned level() const override
    {
        return cacheCntrl->level();
    }

    bool hasBeenPrefetched(Addr addr, bool is_secure) const override
    {
        return cacheCntrl->hasBeenPrefetched(addr, is_secure);
    }

    bool hasBeenPrefetched(Addr addr, bool is_secure,
                            RequestorID requestor) const override
    {
        return cacheCntrl->hasBeenPrefetched(addr, is_secure, requestor);
    }

    bool hasBeenPrefetchedAndNotAccessed(Addr addr, bool is_secure) const override
    {
        return cacheCntrl->hasBeenPrefetchedAndNotAccessed(addr, is_secure);
    }

    Request::XsMetadata getHitBlkXsMetadata(PacketPtr pkt) override
    {
        return cacheCntrl->getHitBlkXsMetadata(pkt->getAddr(), pkt->isSecure());
    }

    bool inMissQueue(Addr addr, bool is_secure) const override
    {
        return cacheCntrl->inMissQueue(addr, is_secure);
    }

    bool coalesce() const override
    { return cacheCntrl->coalesce(); }

    const uint8_t* findBlock(Addr addr, bool is_secure) const override
    {
      return cacheCntrl->findBlock(addr, is_secure)->getDataBlk().getData(0, RubySystem::getBlockSizeBytes());
    }

};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_STRUCTURES_RUBY_PREFETCHER_WRAPPER_HH__

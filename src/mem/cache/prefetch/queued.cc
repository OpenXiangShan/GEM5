/*
 * Copyright (c) 2014-2015 ARM Limited
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

#include "mem/cache/prefetch/queued.hh"

#include <cassert>

#include "arch/generic/tlb.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "debug/HWPrefetchOther.hh"
#include "debug/HWPrefetchQueue.hh"
#include "mem/cache/base.hh"
#include "mem/request.hh"
#include "params/QueuedPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

void
Queued::DeferredPacket::createPkt(Addr paddr, unsigned blk_size, RequestorID requestor_id, bool tag_prefetch, Tick t,
                                  PrefetchSourceType pf_src, int prf_depth)
{
    // TODO: mark from BOP here

    /* Create a prefetch memory request */
    RequestPtr req;
    if (owner->useVirtualAddresses && pfInfo.hasPC()) {
        req = std::make_shared<Request>(pfInfo.getAddr(), blk_size, 0,
                                        requestor_id, pfInfo.getPC(), 0);
        req->setPaddr(paddr);
    } else {
        req = std::make_shared<Request>(paddr, blk_size, 0, requestor_id);
    }

    req->setFlags(Request::PREFETCH);
    req->setXsMetadata(Request::XsMetadata(pf_src, prf_depth));
    DPRINTFR(HWPrefetch, "Create prefetch request for paddr %lx from prefetcher %i\n", paddr, pf_src);

    if (pfInfo.isSecure()) {
        req->setFlags(Request::SECURE);
    }
    req->taskId(context_switch_task_id::Prefetcher);
    //TODO: Xiangshan Metadata insert?
    if (pkt != nullptr) {
        DPRINTFR(HWPrefetch, "Overwriting existing prefetch pkt when it is NOT null!\n");
    }
    pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
    if (tag_prefetch && pfInfo.hasPC()) {
        // Tag prefetch packet with  accessing pc
        pkt->req->setPC(pfInfo.getPC());
    }
    tick = t;
}

void
Queued::DeferredPacket::startTranslation(BaseTLB *tlb)
{
    assert(translationRequest != nullptr);
    if (!ongoingTranslation) {
        ongoingTranslation = true;
        // Prefetchers only operate in Timing mode
        tlb->translateTiming(translationRequest, tc, this, BaseMMU::Read);
    }
}

void
Queued::DeferredPacket::finish(const Fault &fault,
    const RequestPtr &req, ThreadContext *tc, BaseMMU::Mode mode)
{
    assert(ongoingTranslation);
    ongoingTranslation = false;
    bool failed = (fault != NoFault);
    owner->translationComplete(this, failed);
}

Queued::Queued(const QueuedPrefetcherParams &p)
    : Base(p), queueSize(p.queue_size),
      missingTranslationQueueSize(
        p.max_prefetch_requests_with_pending_translation),
      latency(p.latency), queueSquash(p.queue_squash),
      queueFilter(p.queue_filter), cacheSnoop(p.cache_snoop),
      tagPrefetch(p.tag_prefetch),
      throttleControlPct(p.throttle_control_percentage),
      tlbReqEvent(
          [this]{ processMissingTranslations(queueSize); },
          name()),
      statsQueued(this)
{
}

Queued::~Queued()
{
    // Delete the queued prefetch packets
    for (DeferredPacket &p : pfq) {
        delete p.pkt;
    }
}

void
Queued::printQueue(const std::list<DeferredPacket> &queue) const
{
    int pos = 0;
    std::string queue_name = "";
    if (&queue == &pfq) {
        queue_name = "PFQ";
    } else {
        assert(&queue == &pfqMissingTranslation);
        queue_name = "PFTransQ";
    }

    for (const_iterator it = queue.cbegin(); it != queue.cend();
                                                            it++, pos++) {
        Addr vaddr = it->pfInfo.getAddr();
        /* Set paddr to 0 if not yet translated */
        Addr paddr = it->pkt ? it->pkt->getAddr() : 0;
        DPRINTF(HWPrefetchQueue, "%s[%d]: Prefetch Req VA: %#x PA: %#x "
                "prio: %3d\n", queue_name, pos, vaddr, paddr, it->priority);
    }
}

size_t
Queued::getMaxPermittedPrefetches(size_t total) const
{
    /**
     * Throttle generated prefetches based in the accuracy of the prefetcher.
     * Accuracy is computed based in the ratio of useful prefetches with
     * respect to the number of issued prefetches.
     *
     * The throttleControlPct controls how many of the candidate addresses
     * generated by the prefetcher will be finally turned into prefetch
     * requests
     * - If set to 100, all candidates can be discarded (one request
     *   will always be allowed to be generated)
     * - Setting it to 0 will disable the throttle control, so requests are
     *   created for all candidates
     * - If set to 60, 40% of candidates will generate a request, and the
     *   remaining 60% will be generated depending on the current accuracy
     */

    size_t max_pfs = total;
    if (total > 0 && issuedPrefetches > 0) {
        size_t throttle_pfs = (total * throttleControlPct) / 100;
        size_t min_pfs = (total - throttle_pfs) == 0 ?
            1 : (total - throttle_pfs);
        max_pfs = min_pfs + (total - min_pfs) *
            usefulPrefetches / issuedPrefetches;
    }
    return max_pfs;
}

void
Queued::calculatePrefetch(const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addresses, bool late, PrefetchSourceType source, bool miss_repeat)
{
    this->calculatePrefetch(pfi, addresses);
}

void
Queued::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    Addr blk_addr = blockAddress(pfi.getAddr());
    bool is_secure = pfi.isSecure();

    bool late_in_mshr = pkt->missOnLatePf;  // hit in pf mshr

    bool late_in_pfq = false;  // hit in pf queue
    PrefetchSourceType late_pfq_src = PrefetchSourceType::PF_NONE;

    // Squash queued prefetches if demand miss to same line
    if (queueSquash) {
        auto itr = pfq.begin();
        while (itr != pfq.end()) {
            if (itr->pfInfo.getAddr() == blk_addr &&
                itr->pfInfo.isSecure() == is_secure) {
                DPRINTF(HWPrefetch, "Removing pf candidate addr: %#x "
                        "(cl: %#x), demand request going to the same addr\n",
                        itr->pfInfo.getAddr(),
                        blockAddress(itr->pfInfo.getAddr()));
                late_in_pfq = true;  // hit in pf queue
                late_pfq_src = itr->pfInfo.getXsMetadata().prefetchSource;
                delete itr->pkt;
                itr = pfq.erase(itr);
                statsQueued.pfRemovedDemand++;
            } else {
                ++itr;
            }
        }
    }

    PrefetchSourceType pf_source = PrefetchSourceType::PF_NONE;
    if (!pfi.isCacheMiss()) {
        pf_source = pfi.getXsMetadata().prefetchSource;
    } else if (late_in_mshr) {
        pf_source = pkt->getPFSource();
    } else if (late_in_pfq) {
        pf_source = late_pfq_src;
    }
    // Calculate prefetches given this access
    std::vector<AddrPriority> addresses;
    // if (!pkt->coalescingMSHR) {  // hit to Other cpu access
    calculatePrefetch(pfi, addresses, pfi.isCacheMiss() && (late_in_mshr || late_in_pfq), pf_source,
                      pkt->coalescingMSHR);
    // }

    // Get the maximu number of prefetches that we are allowed to generate
    size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

    // Queue up generated prefetches
    size_t num_pfs = 0;
    for (AddrPriority& addr_prio : addresses) {

        // Block align prefetch address
        addr_prio.addr = blockAddress(addr_prio.addr);

        if (!samePage(addr_prio.addr, pfi.getAddr())) {
            statsQueued.pfSpanPage += 1;

            if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
                statsQueued.pfUsefulSpanPage += 1;
            }
        }

        bool can_cross_page = (tlb != nullptr);
        if (can_cross_page || samePage(addr_prio.addr, pfi.getAddr())) {
            PrefetchInfo new_pfi(pfi, addr_prio.addr);
            new_pfi.setXsMetadata(Request::XsMetadata(addr_prio.pfSource,addr_prio.depth));
            statsQueued.pfIdentified++;
            DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, "
                    "inserting into prefetch queue.\n", new_pfi.getAddr());
            // Create and insert the request
            insert(pkt, new_pfi, addr_prio);
            num_pfs += 1;
            if (num_pfs == max_pfs) {
                break;
            }
        } else {
            DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
        }
    }
}

bool
Queued::hasPendingPacket()
{
    return !pfq.empty();
}

PacketPtr
Queued::getPacket()
{
    DPRINTF(HWPrefetch, "Requesting a prefetch to issue.\n");


    if (pfq.empty()) {
        DPRINTF(HWPrefetch, "No hardware prefetches available.\n");
        return nullptr;
    }

    PacketPtr pkt = pfq.front().pkt;
    if (pfq.front().pfahead) {
        prefetchStats.pfaheadProcess++;
    }
    pfq.pop_front();

    prefetchStats.pfIssued++;
    prefetchStats.pfIssued_srcs[pkt->req->getXsMetadata().prefetchSource]++;
    issuedPrefetches += 1;
    assert(pkt != nullptr);
    DPRINTF(HWPrefetch, "Generating prefetch for %#x.\n", pkt->getAddr());

    return pkt;
}

Queued::QueuedStats::QueuedStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(pfIdentified, statistics::units::Count::get(),
             "number of prefetch candidates identified"),
    ADD_STAT(pfBufferHit, statistics::units::Count::get(),
             "number of redundant prefetches already in prefetch queue"),
    ADD_STAT(pfInCache, statistics::units::Count::get(),
             "number of redundant prefetches already in cache/mshr dropped"),
    ADD_STAT(pfRemovedDemand, statistics::units::Count::get(),
             "number of prefetches dropped due to a demand for the same "
             "address"),
    ADD_STAT(pfRemovedFull, statistics::units::Count::get(),
             "number of prefetches dropped due to prefetch queue size"),
    ADD_STAT(pfSpanPage, statistics::units::Count::get(),
             "number of prefetches that crossed the page"),
    ADD_STAT(pfUsefulSpanPage, statistics::units::Count::get(),
             "number of prefetches that is useful and crossed the page")
{
}


void
Queued::processMissingTranslations(unsigned max)
{
    unsigned count = 0;
    iterator it = pfqMissingTranslation.begin();
    while (it != pfqMissingTranslation.end() && count < max) {
        DeferredPacket &dp = *it;
        // Increase the iterator first because dp.startTranslation can end up
        // calling finishTranslation, which will erase "it"
        it++;
        dp.startTranslation(tlb);
        count += 1;
    }
}

void
Queued::translationComplete(DeferredPacket *dp, bool failed)
{
    bool in_squash = false;
    auto it = pfqMissingTranslation.begin();
    while (it != pfqMissingTranslation.end()) {
        if (&(*it) == dp) {
            break;
        }
        it++;
    }
    // If the dp is not in pfqMissingTranslation,
    // we will find it in pfqSquashed
    if (it == pfqMissingTranslation.end()){
        in_squash = true;
        it = pfqSquashed.begin();
        while (it != pfqSquashed.end()) {
            if (&(*it) == dp) {
                break;
            }
            it++;
        }
        assert(it != pfqSquashed.end());
    }
    if (!in_squash){
        if (!failed) {
            DPRINTF(HWPrefetch, "%s Translation of vaddr %#x succeeded: "
                    "paddr %#x \n", tlb->name(),
                    it->translationRequest->getVaddr(),
                    it->translationRequest->getPaddr());
            Addr target_paddr = it->translationRequest->getPaddr();
            // check if this prefetch is already redundant
            if (cacheSnoop && (inCache(target_paddr, it->pfInfo.isSecure()) ||
                        inMissQueue(target_paddr, it->pfInfo.isSecure()))) {
                statsQueued.pfInCache++;
                DPRINTF(HWPrefetch, "Dropping redundant in "
                        "cache/MSHR prefetch addr:%#x\n", target_paddr);
            } else if (!system->isMemAddr(target_paddr)) {
                DPRINTF(HWPrefetch, "wrong paddr of prefetch:%#x\n", target_paddr);

            } else {
                Tick pf_time = curTick() + clockPeriod() * latency;
                it->createPkt(target_paddr, blkSize, requestorId, tagPrefetch,
                            pf_time, it->translationRequest->getPFSource(), it->translationRequest->getPFDepth());
                addToQueue(pfq, *it);
            }
        } else {
            DPRINTF(HWPrefetch, "%s Translation of vaddr %#x failed, dropping "
                    "prefetch request %#x \n", tlb->name(),
                    it->translationRequest->getVaddr());
        }
        pfqMissingTranslation.erase(it);
    } else {
        pfqSquashed.erase(it);
    }
}

bool
Queued::alreadyInQueue(std::list<DeferredPacket> &queue,
                                 const PrefetchInfo &pfi, int32_t priority)
{
    bool found = false;
    iterator it;
    for (it = queue.begin(); it != queue.end() && !found; it++) {
        found = it->pfInfo.sameAddr(pfi);
    }

    /* If the address is already in the queue, update priority and leave */
    if (it != queue.end()) {
        statsQueued.pfBufferHit++;
        if (it->priority < priority) {
            /* Update priority value and position in the queue */
            it->priority = priority;
            /* Because swap() will cause the translationComplete
             * run into wrong DeferredPacket, we use std::list::sort
             * to update this queue */
            queue.sort(std::greater<DeferredPacket>());
            DPRINTF(HWPrefetch, "Prefetch addr already in "
                "prefetch queue, priority updated\n");
        } else {
            DPRINTF(HWPrefetch, "Prefetch addr already in "
                "prefetch queue\n");
        }
    }
    return found;
}
bool

Queued::alreadyInQueue(std::list<DeferredPacket> &queue,
                                 Addr addr, bool isSecure, int32_t priority)
{
    bool found = false;
    iterator it;
    for (it = queue.begin(); it != queue.end() && !found; it++) {
        found = it->pfInfo.sameAddr(addr, isSecure);
    }

    /* If the address is already in the queue, update priority and leave */
    if (it != queue.end()) {
        statsQueued.pfBufferHit++;
        if (it->priority < priority) {
            /* Update priority value and position in the queue */
            it->priority = priority;
            /* Because swap() will cause the translationComplete
             * run into wrong DeferredPacket, we use std::list::sort
             * to update this queue */
            queue.sort(std::greater<DeferredPacket>());
            DPRINTF(HWPrefetch, "Prefetch addr already in "
                "prefetch queue, priority updated\n");
        } else {
            DPRINTF(HWPrefetch, "Prefetch addr already in "
                "prefetch queue\n");
        }
    }
    return found;
}



RequestPtr
Queued::createPrefetchRequest(Addr addr, PrefetchInfo const &pfi, PacketPtr pkt, PrefetchSourceType pf_src, int pf_depth)
{
    RequestPtr translation_req = std::make_shared<Request>(
            addr, blkSize, pkt->req->getFlags(), requestorId, pfi.getPC(),
            pkt->req->contextId());
    translation_req->setFlags(Request::PF_EXCLUSIVE);
    translation_req->setPFSource(pf_src);
    translation_req->setPFDepth(pf_depth);
    translation_req->setXsMetadata(Request::XsMetadata(pf_src, pf_depth));
    DPRINTF(HWPrefetch, "Create prefetch request for vaddr %lx from prefetcher %i\n", addr, pf_src);
    assert(translation_req->hasXsMetadata());
    return translation_req;
}

void
Queued::insert(const PacketPtr &pkt, PrefetchInfo &new_pfi, const AddrPriority &addr_prio)
{
    int32_t priority = addr_prio.priority;
    if (queueFilter) {
        if (alreadyInQueue(pfq, new_pfi, priority)) {
            return;
        }
        if (alreadyInQueue(pfqMissingTranslation, new_pfi, priority)) {
            return;
        }
    }

    /*
     * Physical address computation
     * if the prefetch is within the same page
     *   using VA: add the computed stride to the original PA
     *   using PA: no actions needed
     * if we are page crossing
     *   using VA: Create a translaion request and enqueue the corresponding
     *       deferred packet to the queue of pending translations
     *   using PA: use the provided VA to obtain the target VA, then attempt to
     *     translate the resulting address
     */

    Addr orig_addr = useVirtualAddresses ?
        pkt->req->getVaddr() : pkt->req->getPaddr();
    bool positive_stride = new_pfi.getAddr() >= orig_addr;
    Addr stride = positive_stride ?
        (new_pfi.getAddr() - orig_addr) : (orig_addr - new_pfi.getAddr());

    Addr target_paddr;
    bool has_target_pa = false;
    RequestPtr translation_req = nullptr;
    if (samePage(orig_addr, new_pfi.getAddr())) {
        if (useVirtualAddresses) {
            // if we trained with virtual addresses,
            // compute the target PA using the original PA and adding the
            // prefetch stride (difference between target VA and original VA)
            target_paddr = positive_stride ? (pkt->req->getPaddr() + stride) :
                (pkt->req->getPaddr() - stride);
        } else {
            target_paddr = new_pfi.getAddr();
        }
        has_target_pa = true;
    } else {
        // Page crossing reference

        // ContextID is needed for translation
        if (!pkt->req->hasContextId()) {
            return;
        }
        if (useVirtualAddresses) {
            has_target_pa = false;
            translation_req = createPrefetchRequest(new_pfi.getAddr(), new_pfi, pkt, addr_prio.pfSource, addr_prio.depth);
        } else if (pkt->req->hasVaddr()) {
            has_target_pa = false;
            // Compute the target VA using req->getVaddr + stride
            Addr target_vaddr = positive_stride ?
                (pkt->req->getVaddr() + stride) :
                (pkt->req->getVaddr() - stride);
            translation_req = createPrefetchRequest(target_vaddr, new_pfi, pkt, addr_prio.pfSource, addr_prio.depth);
        } else {
            // Using PA for training but the request does not have a VA,
            // unable to process this page crossing prefetch.
            return;
        }
    }
    if (has_target_pa && cacheSnoop &&
            (inCache(target_paddr, new_pfi.isSecure()) ||
            inMissQueue(target_paddr, new_pfi.isSecure()))) {
        statsQueued.pfInCache++;
        DPRINTF(HWPrefetch, "Dropping redundant in "
                "cache/MSHR prefetch addr:%#x\n", target_paddr);
        return;
    }
    if (has_target_pa && !system->isMemAddr(target_paddr)) {
        DPRINTF(HWPrefetch, "wrong paddr of prefetch:%#x\n", target_paddr);
        return;
    }

    /* Create the packet and find the spot to insert it */
    DeferredPacket dpp(this, new_pfi, 0, priority);
    dpp.pfahead = addr_prio.pfahead;
    dpp.pfahead_host = addr_prio.pfahead_host;
    if (dpp.pfahead) {
        DPRINTF(HWPrefetchOther, "Create one pfahead request\n");
    }
    if (has_target_pa) {
        Tick pf_time = curTick() + clockPeriod() * latency;
        dpp.createPkt(target_paddr, blkSize, requestorId, tagPrefetch,
                      pf_time, addr_prio.pfSource, addr_prio.depth);
        DPRINTF(HWPrefetch, "Prefetch queued. "
                "addr:%#x priority: %3d tick:%lld.\n",
                new_pfi.getAddr(), priority, pf_time);
        addToQueue(pfq, dpp);
    } else {
        // Add the translation request and try to resolve it later
        dpp.setTranslationRequest(translation_req);
        dpp.tc = system->threads[translation_req->contextId()];
        DPRINTF(HWPrefetch, "Prefetch queued with no translation. "
                "addr:%#x priority: %3d\n", new_pfi.getAddr(), priority);
        addToQueue(pfqMissingTranslation, dpp);
        if (!tlbReqEvent.scheduled()) {
            schedule(tlbReqEvent, nextCycle());
        }
    }
}

void
Queued::addToQueue(std::list<DeferredPacket> &queue,
                             DeferredPacket &dpp)
{
    /* Verify prefetch buffer space for request */
    unsigned queue_size;
    const char *queue_name;
    if (&queue == &pfq) {
        // if found the dpp is pfahead marked
        // send it to next level pfq
        if (hasHintDownStream() && dpp.pfahead && (dpp.pfahead_host > cache->level())) {
            hintDownStream->rxHint(&dpp);
            prefetchStats.pfaheadOffloaded++;
            DPRINTF(HWPrefetchOther,
                    "Prefetch ahead host: %d, will send to cache l%s\n",dpp.pfahead_host, cache->level() + 1);
            return;
        }
        if (dpp.pfahead) {
            // l1 can not process l3 pfahead request
            // but l3 can process l1 request
            // if (dpp.pfahead_host > cache->level()) {
            //     panic("Prefetch req from src %i heading to l%i, but l%i can not process it\n",
            //           dpp.pfInfo.getXsMetadata().prefetchSource, dpp.pfahead_host, cache->level());
            // }
        }
        queue_size = queueSize;
        queue_name = "PFQ";
    } else {
        assert(&queue == &pfqMissingTranslation);
        queue_size = missingTranslationQueueSize;
        queue_name = "PFTransQ";
    }
    if (queue.size() == queue_size) {
        statsQueued.pfRemovedFull++;
        /* Lowest priority packet */
        iterator it = queue.end();
        panic_if (it == queue.begin(),
            "Prefetch queue is both full and empty!");
        --it;
        /* Look for oldest in that level of priority */
        panic_if (it == queue.begin(),
            "Prefetch queue is full with 1 element!");
        iterator prev = it;
        bool cont = true;
        /* While not at the head of the queue */
        while (cont && prev != queue.begin()) {
            prev--;
            /* While at the same level of priority */
            cont = prev->priority == it->priority;
            if (cont)
                /* update pointer */
                it = prev;
        }
        DPRINTF(HWPrefetch, "%s full (sz=%lu), removing lowest priority oldest packet, addr: %#x\n", queue_name,
                queue.size(), it->pfInfo.getAddr());
        if (&queue == &pfq || !it->ongoingTranslation){
            delete it->pkt;
            queue.erase(it);
            DPRINTF(HWPrefetch, "Deleted pkt without translation\n");
        } else {
            /* If the packet's translation is on going,
             * we can't erase it here. Just put it into
             * the pfqSquashed list and wait for
             * translationComplete to erase it */
            assert(&queue == &pfqMissingTranslation);
            DeferredPacket * old_ptr = &(*it);
            pfqSquashed.splice(pfqSquashed.end(),queue,it);
            it = pfqSquashed.end();
            it--;
            assert(&(*it) == old_ptr);
            DPRINTF(HWPrefetch, "After moving pkt from transMissQueue to squashQueue, squashQueue sz=%lu\n",
                    pfqSquashed.size());
        }
    }

    if ((queue.size() == 0) || (dpp <= queue.back())) {
        queue.emplace_back(dpp);
        if (&queue == &pfq && dpp.pfahead) {
            DPRINTF(HWPrefetchOther, "insert one pfahead request host by self\n");
        }
    } else {
        iterator it = queue.end();
        do {
            --it;
        } while (it != queue.begin() && dpp > *it);
        /* If we reach the head, we have to see if the new element is new head
         * or not */
        if (it == queue.begin() && dpp <= *it)
            it++;
        queue.insert(it, dpp);
        if (&queue == &pfq && dpp.pfahead) {
            DPRINTF(HWPrefetchOther, "insert one pfahead request host by self\n");
        }
    }

    if (debug::HWPrefetchQueue)
        printQueue(queue);
}

void
Queued::offloadToDownStream()
{
    assert(hintDownStream);

    if (pfq.empty()) {
        DPRINTF(HWPrefetch, "No hardware prefetches available.\n");
        return;
    }

    unsigned offloaded = 0;
    auto dpp_it = pfq.begin();
    while (offloaded < offloadBandwidth && dpp_it != pfq.end()) {
        prefetchStats.pfOffloaded++;
        assert(dpp_it->pkt != nullptr);
        DPRINTF(HWPrefetch, "Offload prefetch for %#x.\n", dpp_it->pkt->getAddr());
        // down stream must copy it instead of store its pointer
        hintDownStream->rxHint(&(*dpp_it));
        dpp_it = pfq.erase(dpp_it);
    }
    DPRINTF(HWPrefetch, "Prefetch requests left in pfq: %lu, trans pfq: %lu\n", pfq.size(),
            pfqMissingTranslation.size());
}

} // namespace prefetch
} // namespace gem5

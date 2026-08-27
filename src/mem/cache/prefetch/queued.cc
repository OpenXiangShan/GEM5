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

#include <algorithm>
#include <cassert>
#include <linux/limits.h>
#include <string>
#include <climits>

#include "arch/generic/tlb.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cmc.hh"
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

namespace
{

std::array<int, NUM_PF_SOURCES>
buildPfControlSourceAdmitPct(const std::vector<int> &values)
{
    std::array<int, NUM_PF_SOURCES> table{};
    table.fill(-1);

    if (values.empty()) {
        return table;
    }

    panic_if(values.size() != NUM_PF_SOURCES &&
                 values.size() != NUM_PF_SOURCES - 1,
             "pf_control_source_admit_pcts must be empty or have %u "
             "entries (legacy form: %u), got %zu",
             unsigned(NUM_PF_SOURCES), unsigned(NUM_PF_SOURCES - 1),
             values.size());

    for (size_t idx = 0; idx < values.size(); ++idx) {
        const int pct = values[idx];
        panic_if(pct < -1 || pct > 100,
                 "pf_control_source_admit_pcts[%zu] must be -1 or "
                 "in [0, 100], got %d",
                 idx, pct);
        table[idx] = pct;
    }

    return table;
}

unsigned
sanitizePercentParam(unsigned pct)
{
    return std::min<unsigned>(pct, 100);
}

unsigned
quantizePercentAboveMin(unsigned pct, unsigned min_pct, unsigned quantum)
{
    if (pct <= min_pct) {
        return min_pct;
    }

    const unsigned rounded =
        ((pct + quantum / 2) / quantum) * quantum;
    return std::max(min_pct, sanitizePercentParam(rounded));
}

} // namespace

void
Queued::DeferredPacket::createPkt(Addr paddr, unsigned blk_size, RequestorID requestor_id, bool tag_prefetch, Tick t,
                                  PrefetchSourceType pf_src, int prf_depth)
{
    // TODO: mark from BOP here

    /* Create a prefetch memory request */
    RequestPtr req;
    if (owner->useVirtualAddresses && pfInfo.hasPC()) {
        if (pfInfo.hasContextId()) {
            req = std::make_shared<Request>(pfInfo.getAddr(), blk_size, 0,
                                            requestor_id, pfInfo.getPC(),
                                            pfInfo.contextId());
        } else {
            req = std::make_shared<Request>();
            req->setVirt(pfInfo.getAddr(), blk_size, 0, requestor_id,
                         pfInfo.getPC());
        }
        req->setPaddr(paddr);
    } else {
        req = std::make_shared<Request>(paddr, blk_size, 0, requestor_id);
        if (pfInfo.hasContextId()) {
            req->setContext(pfInfo.contextId());
        }
    }

    req->setFlags(Request::PREFETCH);
    const PrefetchSourceType safe_pf_src =
        owner->sanitizePfControlSourceType(pf_src);
    req->setXsMetadata(Request::XsMetadata(safe_pf_src, prf_depth));
    DPRINTFR(HWPrefetch, "Create prefetch request for paddr %lx from prefetcher %i\n", paddr, safe_pf_src);

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
        if (owner->functionalTLB) {
            tlb->translateFunctional(translationRequest, tc, this, BaseMMU::Read);
        } else {
            tlb->translateTiming(translationRequest, tc, this, BaseMMU::Read);
        }
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
      pfControl(p.pf_control),
      pfControlWindow(p.pf_control_window),
      pfControlDefaultAdmitPct(
          std::min<unsigned>(p.pf_control_admit_pct, 100)),
      pfControlSweep(p.pf_control_sweep),
      pfControlSourceAdmitPct(
          buildPfControlSourceAdmitPct(
              p.pf_control_source_admit_pcts)),
      pfControlSweepWindows(p.pf_control_sweep_windows),
      pfControlWarmupWindows(p.pf_control_warmup_windows),
      pfAdaptive(p.pf_adaptive),
      pfAdaptiveMinPct(sanitizePercentParam(p.pf_adaptive_min_pct)),
      pfAdaptivePctQuantum(std::max<unsigned>(1, p.pf_adaptive_pct_quantum)),
      pfAdaptiveGradientStep(p.pf_adaptive_gradient_step),
      pfAdaptivePfBadWeightNumerator(
          std::max<unsigned>(1, p.pf_adaptive_pfbad_weight_numer)),
      pfAdaptivePfBadWeightDenominator(
          std::max<unsigned>(1, p.pf_adaptive_pfbad_weight_denom)),
      pfAdaptiveGradientMinSamples(p.pf_adaptive_dpf_min_samples),
      pfAdaptiveGradientDeadband(p.pf_adaptive_dpf_deadband),
      pfAdaptiveImproveMarginBps(p.pf_adaptive_improve_margin_bps),
      pfAdaptiveHistoryFallback(p.pf_adaptive_history_fallback),
      pfAdaptiveBestTopK(std::max<unsigned>(1, p.pf_adaptive_best_topk)),
      pfAdaptiveTableEntries(p.pf_adaptive_table_entries),
      pfAdaptivePfBadEntries(p.pf_adaptive_pfbad_entries),
      pfAdaptiveWarmupWindows(p.pf_adaptive_warmup_windows),
      pfAdaptiveMaxSourceStep(p.pf_adaptive_max_source_step),
      pfAdaptiveWindowDemandAccesses(0),
      pfAdaptiveWindowDemandMisses(0),
      pfAdaptiveWindowPfUsefulBySource{},
      pfAdaptiveWindowPfUnusedBySource{},
      pfAdaptiveWindowPfBadHitsBySource{},
      pfAdaptiveSampleCount(0),
      pfAdaptiveSamples(),
      pfAdaptivePfBadTable(),
      pfControlWindowStart(Cycles(0)),
      pfControlWindowStarted(false),
      pfControlWindowIndex(0),
      pfControlWindowCandidates(0),
      pfControlWindowAdmitted(0),
      pfControlCurrentAdmitPct(pfControlDefaultAdmitPct),
      pfControlWindowCandidatesBySource{},
      pfControlWindowAdmittedBySource{},
      pfControlCurrentAdmitPctBySource{},
      tlbReqEvent(
          [this]{ processMissingTranslations(queueSize); },
          name()),
      statsQueued(this),
      usePFBuffer(p.use_pf_buffer),
      PFRequestBuffer(),
      max_pf_buffer_size(p.max_pf_buffer_size),
      PFReqSendEvent(
          [this]{ PFSendEventWrapper(); },
          name())

{
    panic_if(pfControl && pfControlWindow == Cycles(0),
             "pf_control_window must be non-zero when PF control "
             "is enabled");
    panic_if(pfAdaptive && !pfControl,
             "pf_control must be enabled when pf_adaptive is enabled");
    panic_if(pfAdaptive &&
             (pfAdaptiveMinPct == 0 || pfAdaptiveMinPct > 100),
             "pf_adaptive_min_pct must be in (0, 100]");
    panic_if(pfAdaptive && pfAdaptiveGradientStep < 0,
             "pf_adaptive_gradient_step must be non-negative");
    panic_if(pfAdaptive && pfAdaptiveImproveMarginBps > 10000,
             "pf_adaptive_improve_margin_bps must be in [0, 10000]");
    panic_if(pfAdaptive && pfAdaptiveMaxSourceStep == 0,
             "pf_adaptive_max_source_step must be positive");
    pfAdaptiveWindowPfUsefulBySource.fill(0);
    pfAdaptiveWindowPfUnusedBySource.fill(0);
    pfAdaptiveWindowPfBadHitsBySource.fill(0);
    refreshPfControlCurrentPcts();
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

unsigned
Queued::sanitizePfControlPct(unsigned pct) const
{
    return std::min<unsigned>(pct, 100);
}

PrefetchSourceType
Queued::sanitizePfControlSourceType(PrefetchSourceType source) const
{
    const int source_idx = int(source);
    if (source_idx < 0 || source_idx >= NUM_PF_SOURCES) {
        return PrefetchSourceType::PF_NONE;
    }
    return source;
}

unsigned
Queued::sanitizePfControlSource(PrefetchSourceType source) const
{
    return unsigned(sanitizePfControlSourceType(source));
}

bool
Queued::isPfControlSourceNone(PrefetchSourceType source) const
{
    return sanitizePfControlSourceType(source) == PrefetchSourceType::PF_NONE;
}

unsigned
Queued::getPfControlActionPct(uint64_t window_index) const
{
    if (!pfControl || window_index < pfControlWarmupWindows ||
        pfControlSweep.empty()) {
        return pfControlDefaultAdmitPct;
    }

    const uint64_t action_windows =
        std::max<uint64_t>(1, pfControlSweepWindows);
    const uint64_t action_index =
        ((window_index - pfControlWarmupWindows) / action_windows) %
        pfControlSweep.size();

    return sanitizePfControlPct(pfControlSweep[action_index]);
}

unsigned
Queued::getPfControlActionPctForSource(
    uint64_t window_index, PrefetchSourceType source) const
{
    if (!pfControl) {
        return 100;
    }

    if (pfAdaptive && isPfAdaptiveLevel()) {
        const unsigned source_idx = sanitizePfControlSource(source);
        if (pfAdaptiveSampleCount < pfAdaptiveWarmupWindows) {
            return 100;
        }
        return pfControlCurrentAdmitPctBySource[source_idx];
    }

    if (window_index >= pfControlWarmupWindows) {
        const unsigned source_idx = sanitizePfControlSource(source);
        const int source_pct = pfControlSourceAdmitPct[source_idx];
        if (source_pct >= 0) {
            return sanitizePfControlPct(source_pct);
        }
    }

    return getPfControlActionPct(window_index);
}

void
Queued::refreshPfControlCurrentPcts()
{
    pfControlCurrentAdmitPct =
        getPfControlActionPct(pfControlWindowIndex);
    statsQueued.pfControlCurrentAdmitPct = pfControlCurrentAdmitPct;

    for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
        const auto pf_source = PrefetchSourceType(source);
        const unsigned pct =
            getPfControlActionPctForSource(
                pfControlWindowIndex, pf_source);
        pfControlCurrentAdmitPctBySource[source] = pct;
        statsQueued.pfControlCurrentAdmitPctBySource[source] = pct;
    }
}

bool
Queued::isPfAdaptiveLevel() const
{
    return pfAdaptive && cache &&
        (cache->level() == 1 || cache->level() == 2);
}

unsigned
Queued::quantizePfAdaptivePct(unsigned pct) const
{
    const unsigned quantum = std::max<unsigned>(1, pfAdaptivePctQuantum);
    return quantizePercentAboveMin(pct, pfAdaptiveMinPct, quantum);
}

unsigned
Queued::clampPfAdaptivePct(int pct) const
{
    const int min_pct = int(pfAdaptiveMinPct);
    const int clamped = std::min<int>(100, std::max<int>(min_pct, pct));
    return quantizePfAdaptivePct(unsigned(clamped));
}

int
Queued::computePfAdaptiveSourceGradient(
    uint64_t useful, uint64_t pfbad_hits, uint64_t unused) const
{
    const uint64_t samples = useful + pfbad_hits + unused;
    if (samples < pfAdaptiveGradientMinSamples) {
        return 0;
    }

    // Gradient input is intentionally simple:
    //   useful     = prefetches later hit by demand,
    //   pfbad_hits = cache misses for lines evicted by this prefetch source,
    //   unused     = prefetches evicted without a demand hit.
    // FIFO overflow is diagnostic only; feeding it back here would make table
    // capacity directly change the controller decision.
    const double pfbad_weight =
        double(pfAdaptivePfBadWeightNumerator) /
        pfAdaptivePfBadWeightDenominator;
    const double useful_score = useful;
    const double bad_score =
        pfbad_hits * pfbad_weight + unused * 0.5;
    const double score = useful_score - bad_score;
    const double deadband = pfAdaptiveGradientDeadband;

    const double abs_score = score < 0.0 ? -score : score;
    if (pfAdaptiveGradientDeadband > 0 && abs_score <= deadband) {
        return 0;
    }

    return score > 0.0 ? pfAdaptiveGradientStep :
        -pfAdaptiveGradientStep;
}

uint64_t
Queued::pfAdaptiveMissRateBps(
    uint64_t misses, uint64_t accesses) const
{
    if (accesses == 0) {
        return 0;
    }
    return misses * 10000 / accesses;
}

unsigned
Queued::getPfAdaptiveBestPct(PrefetchSourceType source) const
{
    if (pfAdaptiveSamples.empty()) {
        const unsigned source_idx = sanitizePfControlSource(source);
        return pfControlCurrentAdmitPctBySource[source_idx];
    }

    std::vector<const PfAdaptiveSample *> ranked;
    ranked.reserve(pfAdaptiveSamples.size());
    for (const auto &sample : pfAdaptiveSamples) {
        if (sample.demandAccesses != 0) {
            ranked.push_back(&sample);
        }
    }

    if (ranked.empty()) {
        const unsigned source_idx = sanitizePfControlSource(source);
        return pfControlCurrentAdmitPctBySource[source_idx];
    }

    std::sort(ranked.begin(), ranked.end(),
        [this](const auto *lhs, const auto *rhs) {
            const uint64_t lhs_rate =
                pfAdaptiveMissRateBps(
                    lhs->demandMisses, lhs->demandAccesses);
            const uint64_t rhs_rate =
                pfAdaptiveMissRateBps(
                    rhs->demandMisses, rhs->demandAccesses);
            return lhs_rate < rhs_rate;
        });

    const unsigned source_idx = sanitizePfControlSource(source);
    const size_t take = std::min<size_t>(
        ranked.size(), std::max<unsigned>(1, pfAdaptiveBestTopK));
    uint64_t sum = 0;
    for (size_t idx = 0; idx < take; ++idx) {
        sum += ranked[idx]->pctBySource[source_idx];
    }
    return quantizePfAdaptivePct(
        unsigned((sum + take / 2) / take));
}

void
Queued::pushPfAdaptiveSample(const PfAdaptiveSample &sample)
{
    if (pfAdaptiveTableEntries == 0) {
        return;
    }
    if (pfAdaptiveSamples.size() >= pfAdaptiveTableEntries) {
        pfAdaptiveSamples.pop_front();
        statsQueued.pfAdaptiveTableEvictions++;
    }
    pfAdaptiveSamples.push_back(sample);
    statsQueued.pfAdaptiveTableSize = pfAdaptiveSamples.size();
}

void
Queued::applyPfAdaptiveUpdate(const PfAdaptiveSample &sample)
{
    if (!isPfAdaptiveLevel()) {
        return;
    }

    const uint64_t current_rate =
        pfAdaptiveMissRateBps(
            sample.demandMisses, sample.demandAccesses);
    uint64_t best_rate = UINT64_MAX;
    if (pfAdaptiveHistoryFallback) {
        for (const auto &hist : pfAdaptiveSamples) {
            if (hist.demandAccesses == 0) {
                continue;
            }
            best_rate = std::min(best_rate,
                pfAdaptiveMissRateBps(
                    hist.demandMisses, hist.demandAccesses));
        }
    }

    const bool have_best = best_rate != UINT64_MAX;
    const bool use_prediction =
        !pfAdaptiveHistoryFallback ||
        !have_best ||
        current_rate * 10000 <
            best_rate * (10000 - pfAdaptiveImproveMarginBps);

    for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
        const auto pf_source = PrefetchSourceType(source);
        const int current_pct =
            int(pfControlCurrentAdmitPctBySource[source]);
        const int gradient = sample.gradientBySource[source];

        const unsigned pred_pct =
            clampPfAdaptivePct(current_pct + gradient);
        unsigned next_pct = pred_pct;
        if (!use_prediction) {
            const unsigned best_pct = getPfAdaptiveBestPct(pf_source);
            next_pct = quantizePfAdaptivePct(
                (best_pct + pred_pct + 1) / 2);
        }

        const int max_step =
            int(std::max<unsigned>(1, pfAdaptiveMaxSourceStep));
        const int delta = int(next_pct) - current_pct;
        if (delta > max_step) {
            next_pct = current_pct + max_step;
        } else if (delta < -max_step) {
            next_pct = current_pct - max_step;
        }

        next_pct = clampPfAdaptivePct(int(next_pct));
        pfControlCurrentAdmitPctBySource[source] = next_pct;
        statsQueued.pfControlCurrentAdmitPctBySource[source] = next_pct;
        statsQueued.pfAdaptivePctBySource[source] = next_pct;
        statsQueued.pfAdaptiveGradientBySource[source] = gradient;
    }

    statsQueued.pfAdaptiveUpdates++;
}

void
Queued::resetPfAdaptiveWindowCounters()
{
    pfAdaptiveWindowDemandAccesses = 0;
    pfAdaptiveWindowDemandMisses = 0;
    pfAdaptiveWindowPfUsefulBySource.fill(0);
    pfAdaptiveWindowPfUnusedBySource.fill(0);
    pfAdaptiveWindowPfBadHitsBySource.fill(0);
}

void
Queued::recordPfAdaptiveDemandMiss()
{
    pfAdaptiveWindowDemandMisses++;
}

std::deque<Queued::PfBadEntry>::iterator
Queued::findPfAdaptivePfBadEntry(Addr paddr, bool is_secure)
{
    const Addr block_addr = blockAddress(paddr);
    return std::find_if(
        pfAdaptivePfBadTable.begin(),
        pfAdaptivePfBadTable.end(),
        [block_addr, is_secure](const PfBadEntry &entry) {
            return entry.valid && entry.blockAddr == block_addr &&
                entry.secure == is_secure;
        });
}

bool
Queued::recordPfAdaptivePfBadMissHit(Addr paddr, bool is_secure)
{
    auto it = findPfAdaptivePfBadEntry(paddr, is_secure);
    if (it == pfAdaptivePfBadTable.end()) {
        return false;
    }

    recordPfAdaptivePfBadHit(it->evictorSource);
    pfAdaptivePfBadTable.erase(it);
    statsQueued.pfAdaptivePfBadTableSize =
        pfAdaptivePfBadTable.size();
    return true;
}

void
Queued::recordPfAdaptivePfBadHit(PrefetchSourceType evictor_source)
{
    if (isPfControlSourceNone(evictor_source)) {
        return;
    }
    const unsigned source_idx = sanitizePfControlSource(evictor_source);

    pfAdaptiveWindowPfBadHitsBySource[source_idx]++;
    recordPfBadHit(evictor_source);
    statsQueued.pfAdaptivePfBadHits++;
    statsQueued.pfAdaptivePfBadHitsBySource[source_idx]++;
}

bool
Queued::clearPfAdaptivePfBadEntry(Addr paddr, bool is_secure)
{
    auto it = findPfAdaptivePfBadEntry(paddr, is_secure);
    if (it == pfAdaptivePfBadTable.end()) {
        return false;
    }

    pfAdaptivePfBadTable.erase(it);
    statsQueued.pfAdaptivePfBadTableSize =
        pfAdaptivePfBadTable.size();
    return true;
}

void
Queued::recordPfAdaptivePfBadOverflowEviction(
    PrefetchSourceType evictor_source)
{
    if (isPfControlSourceNone(evictor_source)) {
        return;
    }
    const unsigned source_idx = sanitizePfControlSource(evictor_source);

    statsQueued.pfAdaptivePfBadOverflowEvictions++;
    statsQueued.pfAdaptivePfBadOverflowBySource[source_idx]++;
}

void
Queued::updatePfControlWindow()
{
    if (!pfControl) {
        return;
    }

    const Cycles now = curCycle();
    if (!pfControlWindowStarted) {
        pfControlWindowStart = now;
        pfControlWindowStarted = true;
        refreshPfControlCurrentPcts();
        return;
    }

    while (now - pfControlWindowStart >= pfControlWindow) {
        const unsigned pct = pfControlCurrentAdmitPct;
        statsQueued.pfControlWindows++;
        statsQueued.pfControlWindowsByPct[pct]++;

        if (isPfAdaptiveLevel()) {
            PfAdaptiveSample sample;
            sample.windowIndex = pfControlWindowIndex;
            sample.demandAccesses = pfAdaptiveWindowDemandAccesses;
            sample.demandMisses = pfAdaptiveWindowDemandMisses;
            sample.pctBySource = pfControlCurrentAdmitPctBySource;
            sample.pfUsefulBySource = pfAdaptiveWindowPfUsefulBySource;
            sample.pfUnusedBySource = pfAdaptiveWindowPfUnusedBySource;
            sample.pfBadHitsBySource =
                pfAdaptiveWindowPfBadHitsBySource;
            for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
                sample.gradientBySource[source] =
                    computePfAdaptiveSourceGradient(
                        sample.pfUsefulBySource[source],
                        sample.pfBadHitsBySource[source],
                        sample.pfUnusedBySource[source]);
            }

            if (sample.demandAccesses != 0) {
                statsQueued.pfAdaptiveWindows++;
                statsQueued.pfAdaptiveDemandAccesses +=
                    sample.demandAccesses;
                statsQueued.pfAdaptiveDemandMisses += sample.demandMisses;
                for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
                    statsQueued.pfAdaptivePfUsefulBySource[source] +=
                        sample.pfUsefulBySource[source];
                }

                if (pfAdaptiveSampleCount < pfAdaptiveWarmupWindows) {
                    statsQueued.pfAdaptiveWarmupWindows++;
                } else {
                    applyPfAdaptiveUpdate(sample);
                }
                pfAdaptiveSampleCount++;
                pushPfAdaptiveSample(sample);
            }
            resetPfAdaptiveWindowCounters();
        }

        pfControlWindowStart += pfControlWindow;
        pfControlWindowIndex++;
        pfControlWindowCandidates = 0;
        pfControlWindowAdmitted = 0;
        pfControlWindowCandidatesBySource.fill(0);
        pfControlWindowAdmittedBySource.fill(0);
        if (!isPfAdaptiveLevel()) {
            refreshPfControlCurrentPcts();
        }
    }
}

bool
Queued::shouldPfControlAdmitLocally(
    bool pfahead, int pfahead_host) const
{
    if (!pfahead || !hasHintDownStream() || cache == nullptr) {
        return true;
    }
    return pfahead_host <= int(cache->level());
}

bool
Queued::admitPfControlCandidate(PrefetchSourceType source)
{
    if (!pfControl) {
        return true;
    }

    updatePfControlWindow();

    if (isPfControlSourceNone(source)) {
        return true;
    }
    const unsigned source_idx = sanitizePfControlSource(source);
    const unsigned pct = pfControlCurrentAdmitPctBySource[source_idx];
    statsQueued.pfControlCandidates++;
    statsQueued.pfControlCandidatesByPct[pct]++;
    statsQueued.pfControlCandidatesBySource[source_idx]++;
    pfControlWindowCandidates++;
    pfControlWindowCandidatesBySource[source_idx]++;

    const uint64_t allowed =
        (pfControlWindowCandidatesBySource[source_idx] * pct + 99) / 100;
    const bool admit =
        allowed > pfControlWindowAdmittedBySource[source_idx];

    if (admit) {
        pfControlWindowAdmitted++;
        pfControlWindowAdmittedBySource[source_idx]++;
        statsQueued.pfControlAdmitted++;
        statsQueued.pfControlAdmittedByPct[pct]++;
        statsQueued.pfControlAdmittedBySource[source_idx]++;
    } else {
        statsQueued.pfControlDropped++;
        statsQueued.pfControlDroppedByPct[pct]++;
        statsQueued.pfControlDroppedBySource[source_idx]++;
        DPRINTF(HWPrefetch,
                "PF control dropped candidate from source %i at "
                "admit_pct=%u\n",
                source, pct);
    }

    return admit;
}

bool
Queued::admitPfControlDeferredPacket(const DeferredPacket &dpp)
{
    if (!pfControl) {
        return true;
    }
    if (!shouldPfControlAdmitLocally(dpp.pfahead, dpp.pfahead_host)) {
        return true;
    }

    PrefetchSourceType source = PrefetchSourceType::PF_NONE;
    if (dpp.pkt != nullptr && dpp.pkt->req->hasXsMetadata()) {
        source = dpp.pkt->req->getXsMetadata().prefetchSource;
    } else {
        source = dpp.pfInfo.getXsMetadata().prefetchSource;
    }
    return admitPfControlCandidate(source);
}

void
Queued::notifyDemandAccess(Addr paddr, bool is_secure, bool miss)
{
    if (!isPfAdaptiveLevel()) {
        return;
    }
    updatePfControlWindow();
    pfAdaptiveWindowDemandAccesses++;
    if (miss) {
        recordPfAdaptiveDemandMiss();
    }
}

void
Queued::notifyCacheMissRequest(Addr paddr, bool is_secure)
{
    if (!isPfAdaptiveLevel()) {
        return;
    }
    updatePfControlWindow();
    recordPfAdaptivePfBadMissHit(paddr, is_secure);
}

void
Queued::notifyDemandMshrMiss(Addr paddr, bool is_secure)
{
    if (!isPfAdaptiveLevel()) {
        return;
    }
    updatePfControlWindow();
}

void
Queued::notifyPrefetchUseful(PrefetchSourceType source)
{
    if (!isPfAdaptiveLevel()) {
        return;
    }
    updatePfControlWindow();
    if (isPfControlSourceNone(source)) {
        return;
    }
    const unsigned source_idx = sanitizePfControlSource(source);
    pfAdaptiveWindowPfUsefulBySource[source_idx]++;
}

void
Queued::notifyPrefetchEvictsDemand(
    Addr victim_paddr, bool is_secure, PrefetchSourceType evictor_source)
{
    if (!isPfAdaptiveLevel() || pfAdaptivePfBadEntries == 0) {
        return;
    }
    updatePfControlWindow();

    evictor_source = sanitizePfControlSourceType(evictor_source);
    if (isPfControlSourceNone(evictor_source)) {
        return;
    }
    const unsigned source_idx = sanitizePfControlSource(evictor_source);

    statsQueued.pfAdaptivePfBadCandidates++;
    statsQueued.pfAdaptivePfBadCandidatesBySource[source_idx]++;

    const Addr block_addr = blockAddress(victim_paddr);
    auto it = findPfAdaptivePfBadEntry(block_addr, is_secure);
    if (it != pfAdaptivePfBadTable.end()) {
        it->evictorSource = evictor_source;
        it->insertWindow = pfControlWindowIndex;
        return;
    }

    if (pfAdaptivePfBadTable.size() >= pfAdaptivePfBadEntries) {
        const auto &oldest = pfAdaptivePfBadTable.front();
        if (oldest.valid) {
            recordPfAdaptivePfBadOverflowEviction(oldest.evictorSource);
        }
        pfAdaptivePfBadTable.pop_front();
        statsQueued.pfAdaptivePfBadTableEvictions++;
    }

    pfAdaptivePfBadTable.push_back(
        PfBadEntry{true, block_addr, is_secure, evictor_source,
                   pfControlWindowIndex});
    statsQueued.pfAdaptivePfBadTableSize =
        pfAdaptivePfBadTable.size();
}

void
Queued::notifyCachelineRefill(Addr paddr, bool is_secure)
{
    if (!isPfAdaptiveLevel()) {
        return;
    }
    clearPfAdaptivePfBadEntry(paddr, is_secure);
}

void
Queued::prefetchUnused(PrefetchSourceType pf_source)
{
    Base::prefetchUnused(pf_source);
    if (!isPfAdaptiveLevel()) {
        return;
    }

    updatePfControlWindow();
    if (isPfControlSourceNone(pf_source)) {
        return;
    }
    const unsigned source_idx = sanitizePfControlSource(pf_source);
    pfAdaptiveWindowPfUnusedBySource[source_idx]++;
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

    bool late_in_mshr = pkt->missOnLatePf;  // hit in pf mshr

    bool late_in_pfq = false;  // hit in pf queue
    PrefetchSourceType late_pfq_src = PrefetchSourceType::PF_NONE;

    // Squash queued prefetches if demand miss to same line
    if (queueSquash) {
        PrefetchInfo blk_pfi(pfi, blk_addr);
        auto itr = pfq.begin();
        while (itr != pfq.end()) {
            if (itr->pfInfo.sameAddr(blk_pfi)) {
                DPRINTF(HWPrefetch, "Removing pf candidate addr: %#x "
                        "(cl: %#x), demand request going to the same addr\n",
                        itr->pfInfo.getAddr(),
                        blockAddress(itr->pfInfo.getAddr()));
                late_in_pfq = true;  // hit in pf queue
                late_pfq_src = itr->pfInfo.getXsMetadata().prefetchSource;
                completeDeferredStagedPrefetch(*itr, false);
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
    pfi.setTriggerInfo(pkt);
    calculatePrefetch(pfi, addresses, pfi.isCacheMiss() && (late_in_mshr || late_in_pfq), pf_source,
                      pkt->coalescingMSHR);
    // }
    if (usePFBuffer) {
        //PFs supposed to be stored in buffer,just trigger PF send event
        if (!PFReqSendEvent.scheduled()) {
            //even if this cycle has trained,we assume it take 1 cycle to generate PFs
            schedule(PFReqSendEvent, nextCycle()); 
        } 
        return;
    }
    // Get the maximu number of prefetches that we are allowed to generate
    size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

    // Queue up generated prefetches. A staged token that is not selected by
    // this bandwidth pass is released below; it was never handed to Queued.
    size_t num_pfs = 0;
    size_t candidate_index = 0;
    for (; candidate_index < addresses.size() && num_pfs < max_pfs;
         ++candidate_index) {
        AddrPriority &addr_prio = addresses[candidate_index];

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
            if (shouldPfControlAdmitLocally(
                    addr_prio.pfahead, addr_prio.pfahead_host) &&
                !admitPfControlCandidate(addr_prio.pfSource)) {
                if (addr_prio.stagedToken) {
                    completeStagedPrefetch(*addr_prio.stagedToken, false);
                }
                continue;
            }
            addr_prio.pfSource =
                sanitizePfControlSourceType(addr_prio.pfSource);
            PrefetchInfo new_pfi(pfi, addr_prio.addr);
            new_pfi.setXsMetadata(Request::XsMetadata(addr_prio.pfSource,addr_prio.depth));
            statsQueued.pfIdentified++;
            DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, "
                    "inserting into prefetch queue.\n", new_pfi.getAddr());
            // Create and insert the request
            insert(pkt, new_pfi, addr_prio);
            num_pfs += 1;
        } else {
            DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
            if (addr_prio.stagedToken) {
                completeStagedPrefetch(*addr_prio.stagedToken, false);
            }
        }
    }

    for (; candidate_index < addresses.size(); ++candidate_index) {
        const auto &addr_prio = addresses[candidate_index];
        if (addr_prio.stagedToken) {
            releaseStagedPrefetch(*addr_prio.stagedToken);
        }
    }
}
void
Queued::PFSendEventWrapper()
{
    std::vector<AddrPriority> addresses;
    GetPFRequestsFromBuffer(addresses);

    // there may be more than 1 req in addresses because we are trying to allow max 1 PF to every cache level
    // assert(addresses.size()==1);
    // Get the maximu number of prefetches that we are allowed to generate
    size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

    // Queue up generated prefetches. GetPFRequestsFromBuffer may reserve one
    // STEP candidate per target level; release any reservation this cycle's
    // throttle does not actually submit to Queued.
    size_t num_pfs = 0;
    size_t candidate_index = 0;
    for (; candidate_index < addresses.size() && num_pfs < max_pfs;
         ++candidate_index) {
        AddrPriority &addr_prio = addresses[candidate_index];

        PacketPtr pkt = addr_prio.pf_trigger_info.pkt;
        PrefetchInfo pfi = PrefetchInfo(*addr_prio.pf_trigger_info.pfi_old);
        //override address's prio to 1 
        addr_prio.priority = 1;
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
            if (shouldPfControlAdmitLocally(
                    addr_prio.pfahead, addr_prio.pfahead_host) &&
                !admitPfControlCandidate(addr_prio.pfSource)) {
                if (addr_prio.stagedToken) {
                    completeStagedPrefetch(*addr_prio.stagedToken, false);
                }
                continue;
            }
            addr_prio.pfSource =
                sanitizePfControlSourceType(addr_prio.pfSource);
            PrefetchInfo new_pfi(pfi, addr_prio.addr);
            new_pfi.setXsMetadata(Request::XsMetadata(addr_prio.pfSource,addr_prio.depth));
            statsQueued.pfIdentified++;
            DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, "
                    "inserting into prefetch queue.\n", new_pfi.getAddr());
            insert(pkt, new_pfi, addr_prio);
            num_pfs += 1;
        } else {
            DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
            if (addr_prio.stagedToken) {
                completeStagedPrefetch(*addr_prio.stagedToken, false);
            }
        }
    }
    for (; candidate_index < addresses.size(); ++candidate_index) {
        const auto &addr_prio = addresses[candidate_index];
        if (addr_prio.stagedToken) {
            releaseStagedPrefetch(*addr_prio.stagedToken);
        }
    }
    if (hasPFRequestsInBuffer() && !PFReqSendEvent.scheduled()) {
        schedule(PFReqSendEvent, nextCycle()); // schedule next PF send event
    } 
}
bool
Queued::hasPendingPacket()
{
    return !pfq.empty();
}

bool
Queued::admitIncomingPrefetchPacket(const PacketPtr &pkt)
{
    if (!pfControl || pkt == nullptr || pkt->req == nullptr) {
        return true;
    }
    PrefetchSourceType source = PrefetchSourceType::PF_NONE;
    if (pkt->req->hasXsMetadata()) {
        source = pkt->req->getXsMetadata().prefetchSource;
    } else {
        source = pkt->getPFSource();
    }
    return admitPfControlCandidate(source);
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

    assert(pkt != nullptr);
    if (issueStatsAreAtForwarder()) {
        recordPrefetchDequeued(pkt);
    } else {
        recordIssuedPrefetch(pkt);
    }
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
             "number of prefetches that is useful and crossed the page"),
    ADD_STAT(pfRemovedFull_srcs, statistics::units::Count::get(),
        "src distribute of Removedfull prefetch"),
    ADD_STAT(pfControlCandidates, statistics::units::Count::get(),
             "prefetch control candidate prefetches"),
    ADD_STAT(pfControlAdmitted, statistics::units::Count::get(),
             "prefetch control admitted prefetches"),
    ADD_STAT(pfControlDropped, statistics::units::Count::get(),
             "prefetch control dropped prefetches"),
    ADD_STAT(pfControlWindows, statistics::units::Count::get(),
             "prefetch control completed windows"),
    ADD_STAT(pfControlCurrentAdmitPct, statistics::units::Count::get(),
             "prefetch control current admission percentage"),
    ADD_STAT(pfControlCandidatesByPct, statistics::units::Count::get(),
             "prefetch control candidates by admission percentage"),
    ADD_STAT(pfControlAdmittedByPct, statistics::units::Count::get(),
             "prefetch control admitted prefetches by admission percentage"),
    ADD_STAT(pfControlDroppedByPct, statistics::units::Count::get(),
             "prefetch control dropped prefetches by admission percentage"),
    ADD_STAT(pfControlWindowsByPct, statistics::units::Count::get(),
             "prefetch control completed windows by admission percentage"),
    ADD_STAT(pfControlCandidatesBySource, statistics::units::Count::get(),
             "prefetch control candidates by prefetch source"),
    ADD_STAT(pfControlAdmittedBySource, statistics::units::Count::get(),
             "prefetch control admitted prefetches by prefetch source"),
    ADD_STAT(pfControlDroppedBySource, statistics::units::Count::get(),
             "prefetch control dropped prefetches by prefetch source"),
    ADD_STAT(pfControlCurrentAdmitPctBySource,
             statistics::units::Count::get(),
             "prefetch control current admission percentage by "
             "prefetch source"),
    ADD_STAT(pfAdaptiveWindows, statistics::units::Count::get(),
             "adaptive prefetch completed windows"),
    ADD_STAT(pfAdaptiveUpdates, statistics::units::Count::get(),
             "adaptive prefetch threshold updates"),
    ADD_STAT(pfAdaptiveWarmupWindows, statistics::units::Count::get(),
             "adaptive prefetch warmup windows"),
    ADD_STAT(pfAdaptiveDemandAccesses, statistics::units::Count::get(),
             "adaptive prefetch windowed demand accesses"),
    ADD_STAT(pfAdaptiveDemandMisses, statistics::units::Count::get(),
             "adaptive prefetch windowed demand cache misses"),
    ADD_STAT(pfAdaptiveTableSize, statistics::units::Count::get(),
             "adaptive prefetch FIFO experience table size"),
    ADD_STAT(pfAdaptiveTableEvictions, statistics::units::Count::get(),
             "adaptive prefetch FIFO experience table evictions"),
    ADD_STAT(pfAdaptivePfBadTableSize, statistics::units::Count::get(),
             "adaptive prefetch PFBad table size"),
    ADD_STAT(pfAdaptivePfBadTableEvictions,
             statistics::units::Count::get(),
             "adaptive prefetch PFBad table evictions"),
    ADD_STAT(pfAdaptivePfBadCandidates,
             statistics::units::Count::get(),
             "adaptive prefetch PFBad candidate evictions"),
    ADD_STAT(pfAdaptivePfBadCandidatesBySource,
             statistics::units::Count::get(),
             "adaptive prefetch PFBad candidates by evictor source"),
    ADD_STAT(pfAdaptivePfBadOverflowEvictions,
             statistics::units::Count::get(),
             "adaptive prefetch PFBad table entries evicted by FIFO "
             "overflow"),
    ADD_STAT(pfAdaptivePfBadHits, statistics::units::Count::get(),
             "adaptive prefetch cache miss requests hitting PFBad table"),
    ADD_STAT(pfAdaptivePctBySource, statistics::units::Count::get(),
             "adaptive prefetch current admission percentage by source"),
    ADD_STAT(pfAdaptiveGradientBySource, statistics::units::Count::get(),
             "adaptive prefetch latest per-source threshold gradient"),
    ADD_STAT(pfAdaptivePfUsefulBySource, statistics::units::Count::get(),
             "adaptive prefetch useful counts by source"),
    ADD_STAT(pfAdaptivePfBadHitsBySource, statistics::units::Count::get(),
             "adaptive prefetch cache miss requests hitting PFBad table "
             "by evictor source"),
    ADD_STAT(pfAdaptivePfBadOverflowBySource,
             statistics::units::Count::get(),
             "adaptive prefetch PFBad FIFO overflow evictions by source")
{
    using namespace statistics;
    pfRemovedFull_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfControlCandidatesByPct
        .init(101)
        .flags(total | nozero);
    pfControlAdmittedByPct
        .init(101)
        .flags(total | nozero);
    pfControlDroppedByPct
        .init(101)
        .flags(total | nozero);
    pfControlWindowsByPct
        .init(101)
        .flags(total | nozero);
    pfControlCandidatesBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfControlAdmittedBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfControlDroppedBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfControlCurrentAdmitPctBySource
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfAdaptivePctBySource
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfAdaptiveGradientBySource
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfAdaptivePfUsefulBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfAdaptivePfBadCandidatesBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfAdaptivePfBadHitsBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfAdaptivePfBadOverflowBySource
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    for (unsigned pct = 0; pct <= 100; ++pct) {
        const auto name = std::to_string(pct);
        pfControlCandidatesByPct.subname(pct, name);
        pfControlAdmittedByPct.subname(pct, name);
        pfControlDroppedByPct.subname(pct, name);
        pfControlWindowsByPct.subname(pct, name);
    }
    for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
        const auto source_name = prefetchSourceTypeName(source);
        pfRemovedFull_srcs.subname(source, source_name);
        pfControlCandidatesBySource.subname(source, source_name);
        pfControlAdmittedBySource.subname(source, source_name);
        pfControlDroppedBySource.subname(source, source_name);
        pfControlCurrentAdmitPctBySource.subname(
            source, source_name);
        pfAdaptivePctBySource.subname(source, source_name);
        pfAdaptiveGradientBySource.subname(source, source_name);
        pfAdaptivePfUsefulBySource.subname(source, source_name);
        pfAdaptivePfBadCandidatesBySource.subname(source, source_name);
        pfAdaptivePfBadHitsBySource.subname(source, source_name);
        pfAdaptivePfBadOverflowBySource.subname(
            source, source_name);
    }
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
Queued::completeDeferredStagedPrefetch(DeferredPacket &dpp, bool accepted)
{
    if (!dpp.stagedToken) {
        return;
    }

    fatal_if(dpp.stagedCompletionOwner == nullptr,
             "staged prefetch token has no completion owner");
    dpp.stagedCompletionOwner->completeStagedPrefetch(
        *dpp.stagedToken, accepted);
    dpp.stagedToken.reset();
    dpp.stagedCompletionOwner = nullptr;
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
            if (cacheSnoop && queueFilter && (inCache(target_paddr, it->pfInfo.isSecure()) ||
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
                const bool forwards = willForwardToDownStream(pfq, *it);
                if (!forwards) {
                    // PFQ admission at this cache is the terminal success
                    // point for a local STEP target. Clear the token before
                    // addToQueue copies the deferred packet into PFQ.
                    completeDeferredStagedPrefetch(*it, true);
                }
                addToQueue(pfq, *it);
            }
        } else {
            DPRINTF(HWPrefetch, "%s Translation of vaddr %#x failed, dropping "
                    "prefetch request %#x \n", tlb->name(),
                    it->translationRequest->getVaddr());
        }
        if (failed || !it->pkt) {
            completeDeferredStagedPrefetch(*it, false);
        }
        pfqMissingTranslation.erase(it);
    } else {
        // A full pending-translation queue may have already made this
        // request terminal before its asynchronous translation returned.
        // The helper is idempotent, so retain the false completion here for
        // any future producer that moves a request into the squash queue.
        completeDeferredStagedPrefetch(*it, false);
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
    assert(pfi.hasContextId());
    RequestPtr translation_req = std::make_shared<Request>(
            addr, blkSize, pkt->req->getFlags(), requestorId, pfi.getPC(),
            pfi.contextId());
    translation_req->setFlags(Request::PF_EXCLUSIVE);
    translation_req->setPFSource(pf_src);
    translation_req->setPFDepth(pf_depth);
    translation_req->setXsMetadata(Request::XsMetadata(pf_src, pf_depth));
    DPRINTF(HWPrefetch, "Create prefetch request for vaddr %lx from prefetcher %i\n", addr, pf_src);
    assert(translation_req->hasXsMetadata());
    return translation_req;
}

Queued::InsertResult
Queued::insert(const PacketPtr &pkt, PrefetchInfo &new_pfi, const AddrPriority &addr_prio)
{
    const auto reject_staged = [this, &addr_prio]() {
        if (addr_prio.stagedToken) {
            completeStagedPrefetch(*addr_prio.stagedToken, false);
        }
    };
    int32_t priority = addr_prio.priority;
    if (queueFilter) {
        if (alreadyInQueue(pfq, new_pfi, priority)) {
            reject_staged();
            return InsertResult::Rejected;
        }
        if (alreadyInQueue(pfqMissingTranslation, new_pfi, priority)) {
            reject_staged();
            return InsertResult::Rejected;
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
            reject_staged();
            return InsertResult::Rejected;
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
            reject_staged();
            return InsertResult::Rejected;
        }
    }
    if (has_target_pa && cacheSnoop && queueFilter &&
            (inCache(target_paddr, new_pfi.isSecure()) ||
            inMissQueue(target_paddr, new_pfi.isSecure()))) {
        statsQueued.pfInCache++;
        DPRINTF(HWPrefetch, "Dropping redundant in "
                "cache/MSHR prefetch addr:%#x\n", target_paddr);
        reject_staged();
        return InsertResult::Rejected;
    }
    if (has_target_pa && !system->isMemAddr(target_paddr)) {
        DPRINTF(HWPrefetch, "wrong paddr of prefetch:%#x\n", target_paddr);
        reject_staged();
        return InsertResult::Rejected;
    }

    /* Create the packet and find the spot to insert it */
    DeferredPacket dpp(this, new_pfi, 0, priority);
    dpp.pfahead = addr_prio.pfahead;
    dpp.pfahead_host = addr_prio.pfahead_host;
    dpp.stagedToken = addr_prio.stagedToken;
    if (dpp.stagedToken) {
        dpp.stagedCompletionOwner = this;
    }
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
        if (willForwardToDownStream(pfq, dpp)) {
            addToQueue(pfq, dpp);
            return InsertResult::PendingForward;
        }
        completeDeferredStagedPrefetch(dpp, true);
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
        return InsertResult::PendingTranslation;
    }
    return InsertResult::Accepted;
}

bool
Queued::willForwardToDownStream(const std::list<DeferredPacket> &queue,
                                const DeferredPacket &dpp) const
{
    return &queue == &pfq && hasHintDownStream() && dpp.pfahead &&
        dpp.pfahead_host > cache->level();
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
        const bool step_pfahead = dpp.pfahead &&
            dpp.pfInfo.getXsMetadata().prefetchSource ==
                PrefetchSourceType::STEP;
        fatal_if(step_pfahead && dpp.pfahead_host > cache->level() &&
                     !hasHintDownStream(),
                 "STEP prefetch target L%d is unreachable from L%d",
                 dpp.pfahead_host, cache->level());
        if (willForwardToDownStream(queue, dpp)) {
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
        statsQueued.pfRemovedFull_srcs[it->pfInfo.getXsMetadata().prefetchSource]++;
        completeDeferredStagedPrefetch(*it, false);

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
        // we should not offload cdp prefetch request to lower caches
        const PrefetchSourceType source =
            dpp_it->pfInfo.getXsMetadata().prefetchSource;
        const bool step_at_target = source == PrefetchSourceType::STEP &&
            dpp_it->pfahead_host == cache->level();
        if (source != PrefetchSourceType::CDP && !step_at_target) {
            prefetchStats.pfOffloaded++;
            assert(dpp_it->pkt != nullptr);
            DPRINTF(HWPrefetch, "Offload prefetch for %#x.\n", dpp_it->pkt->getAddr());
            // down stream must copy it instead of store its pointer
            hintDownStream->rxHint(&(*dpp_it));
            dpp_it = pfq.erase(dpp_it);
        } else {
            dpp_it++;
        }
    }
    DPRINTF(HWPrefetch, "Prefetch requests left in pfq: %lu, trans pfq: %lu\n", pfq.size(),
            pfqMissingTranslation.size());
}

} // namespace prefetch
} // namespace gem5

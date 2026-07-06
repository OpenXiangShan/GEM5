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
#include <limits>

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

const char *
prefetchSourceName(int src)
{
    static const char *names[NUM_PF_SOURCES] = {
        "PF_NONE",
        "SStream",
        "SStride",
        "SPht",
        "HWP_BOP",
        "SPP",
        "CMC",
        "IPCP",
        "IPCP_CS",
        "IPCP_CPLX",
        "Berti",
        "StoreStream",
        "CDP",
        "SOpt",
        "DespacitoStream",
    };
    return src >= 0 && src < NUM_PF_SOURCES ? names[src] : "Unknown";
}

} // anonymous namespace

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
      sourceAdmissionEnabled(p.enable_source_admission),
      sourceAdmissionEpoch(std::max<uint32_t>(1, p.source_admission_epoch)),
      sourceAdmissionInitLevel(static_cast<uint8_t>(
          std::min<uint32_t>(4, p.source_admission_init_level))),
      sourceAdmissionMinProbeLevel(static_cast<uint8_t>(
          std::min<uint32_t>(4, p.source_admission_min_probe_level))),
      sourceAdmissionHighConfLevel(static_cast<uint8_t>(
          std::min<uint32_t>(4, p.source_admission_high_conf_level))),
      sourceAdmissionHysteresis(p.source_admission_hysteresis),
      sourceAdmissionPressurePfqPct(std::min<uint32_t>(100, p.source_admission_pressure_pfq_pct)),
      sourceAdmissionRescueInterval(p.source_admission_rescue_interval),
      sourceAdmissionRescueLevel(static_cast<uint8_t>(
          std::min<uint32_t>(4, p.source_admission_rescue_level))),
      sourceAdmissionUnusedWeight(p.source_admission_unused_weight),
      sourceAdmissionDropFullWeight(p.source_admission_drop_full_weight),
      sourceAdmissionMinIssued(p.source_admission_min_issued),
      sourceAdmissionMinUseful(p.source_admission_min_useful),
      sourceAdmissionDownStreakThreshold(std::min<uint32_t>(
          std::numeric_limits<uint8_t>::max(), std::max<uint32_t>(
              1, p.source_admission_down_streak_threshold))),
      sourceAdmissionWarmupEpochs(p.source_admission_warmup_epochs),
      sourceAdmissionDelayedWindowEpochs(std::max<uint32_t>(
          1, p.source_admission_delayed_window_epochs)),
      sourceAdmissionApplyToCandidates(p.source_admission_apply_to_candidates),
      sourceAdmissionApplyToHints(p.source_admission_apply_to_hints),
      sourceAdmissionSkipPfaheadCandidates(
          p.source_admission_skip_pfahead_candidates),
      sourceAdmissionHintMinLevel(static_cast<uint8_t>(
          std::min<uint32_t>(4, p.source_admission_hint_min_level))),
      sourceAdmissionHintIgnorePressureGate(
          p.source_admission_hint_ignore_pressure_gate),
      sourceAdmissionApplyToPFQ(p.source_admission_apply_to_pfq),
      l1dPfaheadFeedbackEnabled(
          p.enable_l1d_pfahead_downstream_reject_feedback),
      l1dPfaheadFeedbackInitLevel(static_cast<uint8_t>(
          std::max<uint32_t>(
              std::min<uint32_t>(4, p.l1d_pfahead_feedback_init_level),
              std::min<uint32_t>(4, p.l1d_pfahead_feedback_min_level)))),
      l1dPfaheadFeedbackMinLevel(static_cast<uint8_t>(
          std::min<uint32_t>(4, p.l1d_pfahead_feedback_min_level))),
      l1dPfaheadFeedbackMinSamples(std::max<uint32_t>(
          1, p.l1d_pfahead_feedback_min_samples)),
      l1dPfaheadFeedbackRejectPct(std::min<uint32_t>(
          100, p.l1d_pfahead_feedback_reject_pct)),
      l1dPfaheadFeedbackRecoverPct(std::min<uint32_t>(
          100, p.l1d_pfahead_feedback_recover_pct)),
      l1dPfaheadFeedbackDownStreakThreshold(std::min<uint32_t>(
          std::numeric_limits<uint8_t>::max(), std::max<uint32_t>(
              1, p.l1d_pfahead_feedback_down_streak_threshold))),
      l1dPfaheadFeedbackUpStreakThreshold(std::min<uint32_t>(
          std::numeric_limits<uint8_t>::max(), std::max<uint32_t>(
              1, p.l1d_pfahead_feedback_up_streak_threshold))),
      l1dPfaheadFeedbackRescueInterval(
          p.l1d_pfahead_feedback_rescue_interval),
      l1dPfaheadFeedbackRescueLevel(static_cast<uint8_t>(
          std::min<uint32_t>(
              l1dPfaheadFeedbackInitLevel,
              std::max<uint32_t>(
                  std::min<uint32_t>(4,
                      p.l1d_pfahead_feedback_rescue_level),
                  l1dPfaheadFeedbackMinLevel)))),
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
    for (int src = 0; src < NUM_PF_SOURCES; ++src) {
        sourceAdmission.level[src] = sourceAdmissionInitLevel;
        statsQueued.pfSourceAdmissionLevel[src] = sourceAdmissionInitLevel;
    }
    l1dPfaheadFeedbackResetState();
    l1dPfaheadFeedbackSyncStats();
}

Queued::~Queued()
{
    // Delete the queued prefetch packets
    for (DeferredPacket &p : pfq) {
        delete p.pkt;
    }
}

void
Queued::resetStats()
{
    Base::resetStats();
    l1dPfaheadFeedbackResetState();
    l1dPfaheadFeedbackSyncStats();
}

void
Queued::preDumpStats()
{
    Base::preDumpStats();
    l1dPfaheadFeedbackSyncStats();
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

bool
Queued::sourceAdmissionHighPressure() const
{
    if (queueSize == 0) {
        return false;
    }
    return (pfq.size() * 100) >=
           (static_cast<size_t>(queueSize) * sourceAdmissionPressurePfqPct);
}

uint8_t
Queued::sourceAdmissionGlobalCap() const
{
    return sourceAdmissionHighPressure() ? 2 : 4;
}

bool
Queued::sourceAdmissionRescueActive(PrefetchSourceType src) const
{
    const int idx = static_cast<int>(src);
    return idx > static_cast<int>(PrefetchSourceType::PF_NONE) &&
           idx < NUM_PF_SOURCES && sourceAdmission.rescueActive[idx];
}

bool
Queued::sourceAdmissionUsesDelayedWindow(PrefetchSourceType src) const
{
    return sourceAdmissionDelayedWindowEpochs > 1 &&
           (src == PrefetchSourceType::CDP ||
            src == PrefetchSourceType::DespacitoStream);
}

uint8_t
Queued::sourceAdmissionEffectiveLevel(PrefetchSourceType src) const
{
    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return 4;
    }

    uint8_t level = sourceAdmission.level[idx];
    if (level == 0 && sourceAdmission.rescueActive[idx]) {
        level = sourceAdmissionRescueLevel;
    }
    return std::min<uint8_t>(level, sourceAdmissionGlobalCap());
}

void
Queued::sourceAdmissionSetLevel(PrefetchSourceType src, uint8_t new_level)
{
    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return;
    }

    new_level = std::min<uint8_t>(4, new_level);
    const uint8_t old_level = sourceAdmission.level[idx];
    if (old_level == new_level) {
        return;
    }

    sourceAdmission.level[idx] = new_level;
    statsQueued.pfSourceAdmissionLevel[idx] = new_level;
}

bool
Queued::sourceAdmissionSourceReady(PrefetchSourceType src) const
{
    if (!sourceAdmissionEnabled) {
        return true;
    }

    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return true;
    }

    const uint8_t raw_level = sourceAdmission.level[idx];
    if (!sourceAdmission.rescueActive[idx] &&
        sourceAdmissionHighPressure() &&
        raw_level < sourceAdmissionHighConfLevel) {
        return false;
    }

    return sourceAdmissionEffectiveLevel(src) > 0;
}

bool
Queued::sourceAdmissionCanIssue(PrefetchSourceType src) const
{
    if (!sourceAdmissionEnabled || !sourceAdmissionApplyToCandidates) {
        return true;
    }

    return sourceAdmissionSourceReady(src);
}

bool
Queued::sourceAdmissionAllow(PrefetchSourceType src)
{
    return sourceAdmissionAllowWithStats(src,
        statsQueued.pfSourceAdmissionAccepted,
        statsQueued.pfSourceAdmissionRejected);
}

bool
Queued::sourceAdmissionAllowWithStats(PrefetchSourceType src,
                                      statistics::Vector &accepted,
                                      statistics::Vector &rejected)
{
    return sourceAdmissionAllowWithPolicy(src, accepted, rejected, 0, false);
}

bool
Queued::sourceAdmissionAllowWithPolicy(PrefetchSourceType src,
                                       statistics::Vector &accepted,
                                       statistics::Vector &rejected,
                                       uint8_t min_level,
                                       bool ignore_pressure_gate)
{
    if (!sourceAdmissionEnabled) {
        return true;
    }

    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return true;
    }

    const uint8_t raw_level = sourceAdmission.level[idx];
    bool accept = true;
    if (!ignore_pressure_gate && !sourceAdmission.rescueActive[idx] &&
        sourceAdmissionHighPressure() &&
        raw_level < sourceAdmissionHighConfLevel) {
        accept = false;
    }

    const uint8_t effective_level = std::max<uint8_t>(
        sourceAdmissionEffectiveLevel(src),
        std::min<uint8_t>(4, min_level));
    accept = accept && effective_level > 0;
    if (accept) {
        const uint8_t ctr = sourceAdmission.sampleCtr[idx]++;
        switch (effective_level) {
          case 0:
            accept = false;
            break;
          case 1:
            accept = (ctr & 0x3) == 0;
            break;
          case 2:
            accept = (ctr & 0x1) == 0;
            break;
          case 3:
            accept = (ctr & 0x3) != 3;
            break;
          default:
            accept = true;
            break;
        }
    }

    if (accept) {
        accepted[idx]++;
    } else {
        rejected[idx]++;
    }
    sourceAdmissionAccountEvent();
    return accept;
}

bool
Queued::sourceAdmissionAllowCandidate(const AddrPriority &addr_prio)
{
    const int idx = static_cast<int>(addr_prio.pfSource);
    const bool apply_candidate_admission =
        sourceAdmissionEnabled && sourceAdmissionApplyToCandidates;
    const bool apply_pfahead_feedback =
        addr_prio.pfahead && l1dPfaheadFeedbackActive();

    if (!apply_candidate_admission && !apply_pfahead_feedback) {
        return true;
    }

    if (apply_candidate_admission &&
        sourceAdmissionSkipPfaheadCandidates && addr_prio.pfahead &&
        idx > static_cast<int>(PrefetchSourceType::PF_NONE) &&
        idx < NUM_PF_SOURCES) {
        statsQueued.pfSourceAdmissionPfaheadCandidateBypassed[idx]++;
    } else if (apply_candidate_admission &&
               !sourceAdmissionAllow(addr_prio.pfSource)) {
        return false;
    }

    if (apply_pfahead_feedback) {
        return l1dPfaheadFeedbackAllow(addr_prio.pfSource);
    }

    return true;
}

bool
Queued::sourceAdmissionAllowHint(PrefetchSourceType src)
{
    if (!sourceAdmissionEnabled || !sourceAdmissionApplyToHints) {
        return true;
    }

    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return true;
    }

    return sourceAdmissionAllowWithPolicy(src,
        statsQueued.pfSourceAdmissionHintAccepted,
        statsQueued.pfSourceAdmissionHintRejected,
        sourceAdmissionHintMinLevel,
        sourceAdmissionHintIgnorePressureGate);
}

bool
Queued::sourceAdmissionAllowPFQ(const DeferredPacket &dpp)
{
    if (!sourceAdmissionEnabled || !sourceAdmissionApplyToPFQ) {
        return true;
    }

    const auto src = dpp.pfInfo.getXsMetadata().prefetchSource;
    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return true;
    }

    if (dpp.ingress == PFQIngress::UpstreamHint) {
        const bool accepted = sourceAdmissionAllowWithPolicy(src,
            statsQueued.pfSourceAdmissionPFQHintAccepted,
            statsQueued.pfSourceAdmissionPFQHintRejected,
            sourceAdmissionHintMinLevel,
            sourceAdmissionHintIgnorePressureGate);
        if (dpp.pfahead && dpp.pfaheadFeedbackSource != nullptr &&
            dpp.pfaheadFeedbackSource != this) {
            const unsigned downstream_level =
                cache != nullptr ? cache->level() : 0;
            dpp.pfaheadFeedbackSource->
                l1dPfaheadFeedbackRecordPFQResult(src, accepted,
                                                  downstream_level);
        }
        return accepted;
    }

    return sourceAdmissionAllowWithPolicy(src,
        statsQueued.pfSourceAdmissionPFQLocalAccepted,
        statsQueued.pfSourceAdmissionPFQLocalRejected,
        0, false);
}

bool
Queued::l1dPfaheadFeedbackActive() const
{
    return l1dPfaheadFeedbackEnabled && cache != nullptr &&
           cache->level() == 1;
}

bool
Queued::l1dPfaheadFeedbackAllow(PrefetchSourceType src)
{
    if (!l1dPfaheadFeedbackActive()) {
        return true;
    }

    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return true;
    }

    bool accept = false;
    l1dPfaheadFeedbackMaybeStartRescue(idx);
    const uint8_t level = l1dPfaheadFeedbackEffectiveLevel(src);
    if (level > 0) {
        const uint8_t ctr = l1dPfaheadFeedback.sampleCtr[idx]++;
        switch (level) {
          case 1:
            accept = (ctr & 0x3) == 0;
            break;
          case 2:
            accept = (ctr & 0x1) == 0;
            break;
          case 3:
            accept = (ctr & 0x3) != 3;
            break;
          default:
            accept = true;
            break;
        }
    }

    if (accept) {
        statsQueued.pfSourcePfaheadAdmissionAccepted[idx]++;
    } else {
        statsQueued.pfSourcePfaheadAdmissionRejected[idx]++;
    }

    if (!l1dPfaheadFeedback.rescueActive[idx] &&
        l1dPfaheadFeedback.level[idx] == l1dPfaheadFeedbackMinLevel &&
        l1dPfaheadFeedback.minLevelSamples[idx] <
            std::numeric_limits<uint32_t>::max()) {
        l1dPfaheadFeedback.minLevelSamples[idx]++;
        if (l1dPfaheadFeedback.minLevelSamples[idx] >=
            l1dPfaheadFeedbackMinSamples) {
            l1dPfaheadFeedback.minLevelSamples[idx] = 0;
            if (l1dPfaheadFeedback.minLevelWindows[idx] <
                std::numeric_limits<uint32_t>::max()) {
                l1dPfaheadFeedback.minLevelWindows[idx]++;
            }
        }
    }
    return accept;
}

void
Queued::l1dPfaheadFeedbackRecordPFQResult(PrefetchSourceType src,
                                          bool accepted,
                                          unsigned downstream_level)
{
    if (!l1dPfaheadFeedbackActive()) {
        return;
    }

    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return;
    }

    if (accepted) {
        statsQueued.pfSourcePfaheadFeedbackAccepted[idx]++;
        l1dPfaheadFeedback.windowAccepted[idx]++;
    } else {
        statsQueued.pfSourcePfaheadFeedbackRejected[idx]++;
        l1dPfaheadFeedback.windowRejected[idx]++;
    }
    l1dPfaheadFeedbackUpdateAggregatePct(idx);

    if (downstream_level == 2) {
        if (accepted) {
            statsQueued.pfSourcePfaheadL2FeedbackAccepted[idx]++;
            l1dPfaheadFeedback.l2WindowAccepted[idx]++;
        } else {
            statsQueued.pfSourcePfaheadL2FeedbackRejected[idx]++;
            l1dPfaheadFeedback.l2WindowRejected[idx]++;
        }
        l1dPfaheadFeedbackUpdateSource(idx);
    } else if (downstream_level == 3) {
        if (accepted) {
            statsQueued.pfSourcePfaheadL3FeedbackAccepted[idx]++;
            l1dPfaheadFeedback.l3WindowAccepted[idx]++;
        } else {
            statsQueued.pfSourcePfaheadL3FeedbackRejected[idx]++;
            l1dPfaheadFeedback.l3WindowRejected[idx]++;
        }
        l1dPfaheadFeedbackUpdateL3Pct(idx);
    }
}

uint8_t
Queued::l1dPfaheadFeedbackEffectiveLevel(PrefetchSourceType src) const
{
    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return 4;
    }

    uint8_t level = l1dPfaheadFeedback.level[idx];
    if (l1dPfaheadFeedback.rescueActive[idx]) {
        level = std::max<uint8_t>(level, l1dPfaheadFeedbackRescueLevel);
    }
    return std::min<uint8_t>(
        level, std::min<uint8_t>(4, l1dPfaheadFeedbackInitLevel));
}

void
Queued::l1dPfaheadFeedbackMaybeStartRescue(int src)
{
    if (src <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        src >= NUM_PF_SOURCES) {
        return;
    }

    if (l1dPfaheadFeedbackRescueInterval == 0 ||
        l1dPfaheadFeedbackRescueLevel == l1dPfaheadFeedbackMinLevel ||
        l1dPfaheadFeedback.level[src] != l1dPfaheadFeedbackMinLevel ||
        l1dPfaheadFeedback.rescueActive[src]) {
        return;
    }

    if (l1dPfaheadFeedback.minLevelWindows[src] <
        l1dPfaheadFeedbackRescueInterval) {
        return;
    }

    l1dPfaheadFeedback.rescueActive[src] = true;
    l1dPfaheadFeedback.minLevelWindows[src] = 0;
    l1dPfaheadFeedback.minLevelSamples[src] = 0;
    statsQueued.pfSourcePfaheadFeedbackRescueEpochs[src]++;
    statsQueued.pfSourcePfaheadFeedbackRescueActive[src] = 1;
}

void
Queued::l1dPfaheadFeedbackResetState()
{
    for (int src = 0; src < NUM_PF_SOURCES; ++src) {
        l1dPfaheadFeedback.level[src] = std::max<uint8_t>(
            l1dPfaheadFeedbackInitLevel, l1dPfaheadFeedbackMinLevel);
        l1dPfaheadFeedback.sampleCtr[src] = 0;
        l1dPfaheadFeedback.downStreak[src] = 0;
        l1dPfaheadFeedback.upStreak[src] = 0;
        l1dPfaheadFeedback.windowAccepted[src] = 0;
        l1dPfaheadFeedback.windowRejected[src] = 0;
        l1dPfaheadFeedback.l2WindowAccepted[src] = 0;
        l1dPfaheadFeedback.l2WindowRejected[src] = 0;
        l1dPfaheadFeedback.l3WindowAccepted[src] = 0;
        l1dPfaheadFeedback.l3WindowRejected[src] = 0;
        l1dPfaheadFeedback.feedbackRejectPct[src] = 0;
        l1dPfaheadFeedback.l2FeedbackRejectPct[src] = 0;
        l1dPfaheadFeedback.l3FeedbackRejectPct[src] = 0;
        l1dPfaheadFeedback.minLevelWindows[src] = 0;
        l1dPfaheadFeedback.minLevelSamples[src] = 0;
        l1dPfaheadFeedback.rescueActive[src] = false;
    }
}

void
Queued::l1dPfaheadFeedbackSyncStats()
{
    for (int src = 0; src < NUM_PF_SOURCES; ++src) {
        statsQueued.pfSourcePfaheadAdmissionLevel[src] =
            l1dPfaheadFeedback.level[src];
        statsQueued.pfSourcePfaheadFeedbackRejectPct[src] =
            l1dPfaheadFeedback.feedbackRejectPct[src];
        statsQueued.pfSourcePfaheadL2FeedbackRejectPct[src] =
            l1dPfaheadFeedback.l2FeedbackRejectPct[src];
        statsQueued.pfSourcePfaheadL3FeedbackRejectPct[src] =
            l1dPfaheadFeedback.l3FeedbackRejectPct[src];
        statsQueued.pfSourcePfaheadFeedbackRescueActive[src] =
            l1dPfaheadFeedback.rescueActive[src] ? 1 : 0;
    }
}

void
Queued::l1dPfaheadFeedbackSetLevel(PrefetchSourceType src, uint8_t level)
{
    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return;
    }

    level = std::min<uint8_t>(4, level);
    level = std::max<uint8_t>(level, l1dPfaheadFeedbackMinLevel);
    if (l1dPfaheadFeedback.level[idx] == level) {
        return;
    }

    l1dPfaheadFeedback.level[idx] = level;
    statsQueued.pfSourcePfaheadAdmissionLevel[idx] = level;
}

void
Queued::l1dPfaheadFeedbackUpdateSource(int src)
{
    if (src <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        src >= NUM_PF_SOURCES) {
        return;
    }

    const uint64_t accepted = l1dPfaheadFeedback.l2WindowAccepted[src];
    const uint64_t rejected = l1dPfaheadFeedback.l2WindowRejected[src];
    const uint64_t samples = accepted + rejected;
    if (samples < l1dPfaheadFeedbackMinSamples) {
        return;
    }

    const auto pf_src = static_cast<PrefetchSourceType>(src);
    const uint64_t reject_pct = (rejected * 100) / samples;
    l1dPfaheadFeedback.l2FeedbackRejectPct[src] = reject_pct;
    statsQueued.pfSourcePfaheadL2FeedbackRejectPct[src] = reject_pct;

    uint8_t level = l1dPfaheadFeedback.level[src];
    if (reject_pct >= l1dPfaheadFeedbackRejectPct) {
        l1dPfaheadFeedback.upStreak[src] = 0;
        if (l1dPfaheadFeedback.downStreak[src] <
            std::numeric_limits<uint8_t>::max()) {
            l1dPfaheadFeedback.downStreak[src]++;
        }
        if (l1dPfaheadFeedback.downStreak[src] >=
            l1dPfaheadFeedbackDownStreakThreshold) {
            if (level > l1dPfaheadFeedbackMinLevel) {
                level--;
                l1dPfaheadFeedbackSetLevel(pf_src, level);
                statsQueued.pfSourcePfaheadFeedbackDemotions[src]++;
            }
            l1dPfaheadFeedback.downStreak[src] = 0;
        }
    } else if (reject_pct <= l1dPfaheadFeedbackRecoverPct) {
        l1dPfaheadFeedback.downStreak[src] = 0;
        if (l1dPfaheadFeedback.upStreak[src] <
            std::numeric_limits<uint8_t>::max()) {
            l1dPfaheadFeedback.upStreak[src]++;
        }
        if (l1dPfaheadFeedback.upStreak[src] >=
            l1dPfaheadFeedbackUpStreakThreshold) {
            if (level < l1dPfaheadFeedbackInitLevel && level < 4) {
                level++;
                l1dPfaheadFeedbackSetLevel(pf_src, level);
                statsQueued.pfSourcePfaheadFeedbackPromotions[src]++;
            }
            l1dPfaheadFeedback.upStreak[src] = 0;
        }
    } else {
        l1dPfaheadFeedback.downStreak[src] = 0;
        l1dPfaheadFeedback.upStreak[src] = 0;
    }

    if (l1dPfaheadFeedback.rescueActive[src]) {
        l1dPfaheadFeedback.rescueActive[src] = false;
    }
    if (l1dPfaheadFeedback.level[src] != l1dPfaheadFeedbackMinLevel) {
        l1dPfaheadFeedback.minLevelWindows[src] = 0;
        l1dPfaheadFeedback.minLevelSamples[src] = 0;
    }

    l1dPfaheadFeedback.l2WindowAccepted[src] = 0;
    l1dPfaheadFeedback.l2WindowRejected[src] = 0;
    statsQueued.pfSourcePfaheadFeedbackRescueActive[src] =
        l1dPfaheadFeedback.rescueActive[src] ? 1 : 0;
}

void
Queued::l1dPfaheadFeedbackUpdateAggregatePct(int src)
{
    if (src <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        src >= NUM_PF_SOURCES) {
        return;
    }

    const uint64_t accepted = l1dPfaheadFeedback.windowAccepted[src];
    const uint64_t rejected = l1dPfaheadFeedback.windowRejected[src];
    const uint64_t samples = accepted + rejected;
    if (samples < l1dPfaheadFeedbackMinSamples) {
        return;
    }

    const uint32_t reject_pct = (rejected * 100) / samples;
    l1dPfaheadFeedback.feedbackRejectPct[src] = reject_pct;
    statsQueued.pfSourcePfaheadFeedbackRejectPct[src] = reject_pct;
    l1dPfaheadFeedback.windowAccepted[src] = 0;
    l1dPfaheadFeedback.windowRejected[src] = 0;
}

void
Queued::l1dPfaheadFeedbackUpdateL3Pct(int src)
{
    if (src <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        src >= NUM_PF_SOURCES) {
        return;
    }

    const uint64_t accepted = l1dPfaheadFeedback.l3WindowAccepted[src];
    const uint64_t rejected = l1dPfaheadFeedback.l3WindowRejected[src];
    const uint64_t samples = accepted + rejected;
    if (samples < l1dPfaheadFeedbackMinSamples) {
        return;
    }

    const uint32_t reject_pct = (rejected * 100) / samples;
    l1dPfaheadFeedback.l3FeedbackRejectPct[src] = reject_pct;
    statsQueued.pfSourcePfaheadL3FeedbackRejectPct[src] = reject_pct;
    l1dPfaheadFeedback.l3WindowAccepted[src] = 0;
    l1dPfaheadFeedback.l3WindowRejected[src] = 0;
}

void
Queued::sourceAdmissionAccountEvent()
{
    if (!sourceAdmissionEnabled) {
        return;
    }

    sourceAdmission.epochEvents++;
    if (sourceAdmission.epochEvents >= sourceAdmissionEpoch) {
        sourceAdmissionUpdateEpoch();
        sourceAdmission.epochEvents = 0;
    }
}

void
Queued::sourceAdmissionResetWindow(int src)
{
    sourceAdmission.windowEpochs[src] = 0;
    sourceAdmission.windowIssued[src] = 0;
    sourceAdmission.windowUseful[src] = 0;
    sourceAdmission.windowUnused[src] = 0;
    sourceAdmission.windowLate[src] = 0;
    sourceAdmission.windowDropFull[src] = 0;
}

void
Queued::sourceAdmissionAccountQueueFull(PrefetchSourceType src)
{
    if (!sourceAdmissionEnabled) {
        return;
    }

    const int idx = static_cast<int>(src);
    if (idx > static_cast<int>(PrefetchSourceType::PF_NONE) &&
        idx < NUM_PF_SOURCES) {
        sourceAdmissionAccountEvent();
    }
}

void
Queued::sourceAdmissionUpdateRescueState(PrefetchSourceType src,
                                         uint8_t level,
                                         bool decision_made)
{
    const int idx = static_cast<int>(src);
    if (idx <= static_cast<int>(PrefetchSourceType::PF_NONE) ||
        idx >= NUM_PF_SOURCES) {
        return;
    }

    if (level > 0 || sourceAdmissionRescueInterval == 0 ||
        sourceAdmissionRescueLevel == 0) {
        sourceAdmission.zeroEpochs[idx] = 0;
        sourceAdmission.rescueActive[idx] = false;
        return;
    }

    if (sourceAdmission.rescueActive[idx]) {
        if (decision_made) {
            sourceAdmission.rescueActive[idx] = false;
            sourceAdmission.zeroEpochs[idx] = 0;
        }
        return;
    }

    if (decision_made) {
        return;
    }

    if (sourceAdmission.zeroEpochs[idx] <
        std::numeric_limits<uint32_t>::max()) {
        sourceAdmission.zeroEpochs[idx]++;
    }
    if (sourceAdmission.zeroEpochs[idx] >= sourceAdmissionRescueInterval) {
        sourceAdmission.rescueActive[idx] = true;
        sourceAdmission.zeroEpochs[idx] = 0;
        statsQueued.pfSourceAdmissionRescueEpochs[idx]++;
    }
}

void
Queued::sourceAdmissionUpdateEpoch()
{
    const uint8_t min_level = sourceAdmissionMinProbeLevel;
    sourceAdmission.epochCount++;

    for (int src = static_cast<int>(PrefetchSourceType::PF_NONE) + 1;
         src < NUM_PF_SOURCES; ++src) {
        const auto pf_src = static_cast<PrefetchSourceType>(src);
        const uint64_t issued =
            static_cast<uint64_t>(prefetchStats.pfIssued_srcs[pf_src].value());
        const uint64_t useful =
            static_cast<uint64_t>(prefetchStats.pfUseful_srcs[pf_src].value());
        const uint64_t unused =
            static_cast<uint64_t>(prefetchStats.pfUnused_srcs[pf_src].value());
        const uint64_t late =
            static_cast<uint64_t>(prefetchStats.late_srcs[pf_src].value());
        const uint64_t drop_full =
            static_cast<uint64_t>(statsQueued.pfRemovedFull_srcs[pf_src].value());

        const uint64_t issued_delta = issued - sourceAdmission.lastIssued[src];
        const uint64_t useful_delta = useful - sourceAdmission.lastUseful[src];
        const uint64_t unused_delta = unused - sourceAdmission.lastUnused[src];
        const uint64_t late_delta = late - sourceAdmission.lastLate[src];
        const uint64_t drop_full_delta =
            drop_full - sourceAdmission.lastDropFull[src];

        sourceAdmission.lastIssued[src] = issued;
        sourceAdmission.lastUseful[src] = useful;
        sourceAdmission.lastUnused[src] = unused;
        sourceAdmission.lastLate[src] = late;
        sourceAdmission.lastDropFull[src] = drop_full;

        sourceAdmission.windowEpochs[src]++;
        sourceAdmission.windowIssued[src] += issued_delta;
        sourceAdmission.windowUseful[src] += useful_delta;
        sourceAdmission.windowUnused[src] += unused_delta;
        sourceAdmission.windowLate[src] += late_delta;
        sourceAdmission.windowDropFull[src] += drop_full_delta;
        sourceAdmissionUpdateRescueState(pf_src, sourceAdmission.level[src],
                                         false);

        const uint32_t window_epochs =
            sourceAdmissionUsesDelayedWindow(pf_src) ?
            sourceAdmissionDelayedWindowEpochs : 1;
        if (sourceAdmission.windowEpochs[src] < window_epochs) {
            statsQueued.pfSourceAdmissionEpochUpdates[src]++;
            continue;
        }

        const uint64_t window_issued = sourceAdmission.windowIssued[src];
        const uint64_t window_useful = sourceAdmission.windowUseful[src];
        const uint64_t window_unused = sourceAdmission.windowUnused[src];
        const uint64_t window_late = sourceAdmission.windowLate[src];
        const uint64_t window_drop_full = sourceAdmission.windowDropFull[src];

        const uint64_t pressure_penalty =
            sourceAdmissionHighPressure() ? window_issued : 0;
        const uint64_t good = window_useful * 8;
        const uint64_t bad = window_issued +
                             window_unused * sourceAdmissionUnusedWeight +
                             window_late +
                             window_drop_full * sourceAdmissionDropFullWeight +
                             pressure_penalty;
        const bool enough_samples =
            window_issued >= sourceAdmissionMinIssued ||
            window_useful >= sourceAdmissionMinUseful;
        const bool warmup =
            sourceAdmission.epochCount <= sourceAdmissionWarmupEpochs;

        uint8_t level = sourceAdmission.level[src];
        if (good > bad + sourceAdmissionHysteresis && level < 4) {
            level++;
            sourceAdmission.negativeStreak[src] = 0;
        } else if (!warmup && enough_samples &&
                   bad > good + sourceAdmissionHysteresis &&
                   level > min_level) {
            if (sourceAdmission.negativeStreak[src] <
                std::numeric_limits<uint8_t>::max()) {
                sourceAdmission.negativeStreak[src]++;
            }
            if (sourceAdmission.negativeStreak[src] >=
                sourceAdmissionDownStreakThreshold) {
                level--;
                sourceAdmission.negativeStreak[src] = 0;
            }
        } else if (good >= bad || !enough_samples) {
            sourceAdmission.negativeStreak[src] = 0;
        }

        level = std::max<uint8_t>(level, min_level);
        sourceAdmissionSetLevel(pf_src, level);
        sourceAdmissionUpdateRescueState(pf_src, level, true);
        sourceAdmissionResetWindow(src);
        statsQueued.pfSourceAdmissionEpochUpdates[src]++;
        statsQueued.pfSourceAdmissionNegativeStreak[src] =
            sourceAdmission.negativeStreak[src];
    }
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
            if (!sourceAdmissionAllowCandidate(addr_prio)) {
                continue;
            }
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
void
Queued::PFSendEventWrapper()
{
    std::vector<AddrPriority> addresses;
    GetPFRequestsFromBuffer(addresses);

    // there may be more than 1 req in addresses because we are trying to allow max 1 PF to every cache level
    // assert(addresses.size()==1);
    // Get the maximu number of prefetches that we are allowed to generate
    size_t max_pfs = getMaxPermittedPrefetches(addresses.size());

    // Queue up generated prefetches
    size_t num_pfs = 0;
    for (AddrPriority& addr_prio : addresses) {
            
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
            if (!sourceAdmissionAllowCandidate(addr_prio)) {
                continue;
            }
            PrefetchInfo new_pfi(pfi, addr_prio.addr);
            new_pfi.setXsMetadata(Request::XsMetadata(addr_prio.pfSource,addr_prio.depth));
            statsQueued.pfIdentified++;
            DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, "
                    "inserting into prefetch queue.\n", new_pfi.getAddr());
            insert(pkt, new_pfi, addr_prio);
            num_pfs += 1;
            if (num_pfs == max_pfs) {
                break;
            }
        } else {
            DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
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
    sourceAdmissionAccountEvent();
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
             "number of prefetches that is useful and crossed the page"),
    ADD_STAT(pfRemovedFull_srcs, statistics::units::Count::get(),
        "src distribute of Removedfull prefetch"),
    ADD_STAT(pfSourceAdmissionRejected, statistics::units::Count::get(),
        "prefetch candidates rejected by dynamic source admission"),
    ADD_STAT(pfSourceAdmissionAccepted, statistics::units::Count::get(),
        "prefetch candidates accepted by dynamic source admission"),
    ADD_STAT(pfSourceAdmissionEpochUpdates, statistics::units::Count::get(),
        "dynamic source admission epoch updates"),
    ADD_STAT(pfSourceAdmissionLevel, statistics::units::Count::get(),
        "current dynamic source admission level"),
    ADD_STAT(pfSourceAdmissionRescueEpochs, statistics::units::Count::get(),
        "dynamic source admission rescue probe activations"),
    ADD_STAT(pfSourceAdmissionNegativeStreak, statistics::units::Count::get(),
        "current consecutive negative source admission windows"),
    ADD_STAT(pfSourceAdmissionHintRejected, statistics::units::Count::get(),
        "upstream hints rejected by dynamic source admission"),
    ADD_STAT(pfSourceAdmissionHintAccepted, statistics::units::Count::get(),
        "upstream hints accepted by dynamic source admission"),
    ADD_STAT(pfSourceAdmissionPfaheadCandidateBypassed, statistics::units::Count::get(),
        "pfahead candidates bypassing local dynamic source admission"),
    ADD_STAT(pfSourceAdmissionPFQLocalRejected, statistics::units::Count::get(),
        "local PFQ insertions rejected by unified dynamic source admission"),
    ADD_STAT(pfSourceAdmissionPFQLocalAccepted, statistics::units::Count::get(),
        "local PFQ insertions accepted by unified dynamic source admission"),
    ADD_STAT(pfSourceAdmissionPFQHintRejected, statistics::units::Count::get(),
        "upstream hint PFQ insertions rejected by unified dynamic source admission"),
    ADD_STAT(pfSourceAdmissionPFQHintAccepted, statistics::units::Count::get(),
        "upstream hint PFQ insertions accepted by unified dynamic source admission"),
    ADD_STAT(pfSourcePfaheadFeedbackAccepted, statistics::units::Count::get(),
        "downstream PFQ accepted feedback for L1D pfahead requests"),
    ADD_STAT(pfSourcePfaheadFeedbackRejected, statistics::units::Count::get(),
        "downstream PFQ rejected feedback for L1D pfahead requests"),
    ADD_STAT(pfSourcePfaheadFeedbackRejectPct, statistics::units::Count::get(),
        "last L1D pfahead downstream feedback reject percentage"),
    ADD_STAT(pfSourcePfaheadL2FeedbackAccepted, statistics::units::Count::get(),
        "L2 PFQ accepted feedback for L1D pfahead requests"),
    ADD_STAT(pfSourcePfaheadL2FeedbackRejected, statistics::units::Count::get(),
        "L2 PFQ rejected feedback for L1D pfahead requests"),
    ADD_STAT(pfSourcePfaheadL2FeedbackRejectPct, statistics::units::Count::get(),
        "last L2 PFQ feedback reject percentage for L1D pfahead"),
    ADD_STAT(pfSourcePfaheadL3FeedbackAccepted, statistics::units::Count::get(),
        "L3 PFQ accepted feedback for L1D pfahead requests"),
    ADD_STAT(pfSourcePfaheadL3FeedbackRejected, statistics::units::Count::get(),
        "L3 PFQ rejected feedback for L1D pfahead requests"),
    ADD_STAT(pfSourcePfaheadL3FeedbackRejectPct, statistics::units::Count::get(),
        "last L3 PFQ feedback reject percentage for L1D pfahead"),
    ADD_STAT(pfSourcePfaheadAdmissionLevel, statistics::units::Count::get(),
        "current L1D pfahead downstream feedback admission level"),
    ADD_STAT(pfSourcePfaheadAdmissionAccepted, statistics::units::Count::get(),
        "L1D pfahead candidates accepted by downstream feedback admission"),
    ADD_STAT(pfSourcePfaheadAdmissionRejected, statistics::units::Count::get(),
        "L1D pfahead candidates rejected by downstream feedback admission"),
    ADD_STAT(pfSourcePfaheadFeedbackDemotions, statistics::units::Count::get(),
        "L1D pfahead feedback admission demotions"),
    ADD_STAT(pfSourcePfaheadFeedbackPromotions, statistics::units::Count::get(),
        "L1D pfahead feedback admission promotions"),
    ADD_STAT(pfSourcePfaheadFeedbackRescueEpochs, statistics::units::Count::get(),
        "L1D pfahead feedback rescue probe activations"),
    ADD_STAT(pfSourcePfaheadFeedbackRescueActive, statistics::units::Count::get(),
        "current L1D pfahead feedback rescue active state")
{   using namespace statistics;
    pfRemovedFull_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionEpochUpdates
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionLevel
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionRescueEpochs
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionNegativeStreak
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionHintRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionHintAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionPfaheadCandidateBypassed
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionPFQLocalRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionPFQLocalAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionPFQHintRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourceAdmissionPFQHintAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackRejectPct
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadL2FeedbackAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadL2FeedbackRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadL2FeedbackRejectPct
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadL3FeedbackAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadL3FeedbackRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadL3FeedbackRejectPct
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadAdmissionLevel
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadAdmissionAccepted
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadAdmissionRejected
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackDemotions
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackPromotions
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackRescueEpochs
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfSourcePfaheadFeedbackRescueActive
        .init(NUM_PF_SOURCES)
        .flags(total);

    for (int src = 0; src < NUM_PF_SOURCES; ++src) {
        const char *name = prefetchSourceName(src);
        pfSourceAdmissionRejected.subname(src, name);
        pfSourceAdmissionAccepted.subname(src, name);
        pfSourceAdmissionEpochUpdates.subname(src, name);
        pfSourceAdmissionLevel.subname(src, name);
        pfSourceAdmissionRescueEpochs.subname(src, name);
        pfSourceAdmissionNegativeStreak.subname(src, name);
        pfSourceAdmissionHintRejected.subname(src, name);
        pfSourceAdmissionHintAccepted.subname(src, name);
        pfSourceAdmissionPfaheadCandidateBypassed.subname(src, name);
        pfSourceAdmissionPFQLocalRejected.subname(src, name);
        pfSourceAdmissionPFQLocalAccepted.subname(src, name);
        pfSourceAdmissionPFQHintRejected.subname(src, name);
        pfSourceAdmissionPFQHintAccepted.subname(src, name);
        pfSourcePfaheadFeedbackAccepted.subname(src, name);
        pfSourcePfaheadFeedbackRejected.subname(src, name);
        pfSourcePfaheadFeedbackRejectPct.subname(src, name);
        pfSourcePfaheadL2FeedbackAccepted.subname(src, name);
        pfSourcePfaheadL2FeedbackRejected.subname(src, name);
        pfSourcePfaheadL2FeedbackRejectPct.subname(src, name);
        pfSourcePfaheadL3FeedbackAccepted.subname(src, name);
        pfSourcePfaheadL3FeedbackRejected.subname(src, name);
        pfSourcePfaheadL3FeedbackRejectPct.subname(src, name);
        pfSourcePfaheadAdmissionLevel.subname(src, name);
        pfSourcePfaheadAdmissionAccepted.subname(src, name);
        pfSourcePfaheadAdmissionRejected.subname(src, name);
        pfSourcePfaheadFeedbackDemotions.subname(src, name);
        pfSourcePfaheadFeedbackPromotions.subname(src, name);
        pfSourcePfaheadFeedbackRescueEpochs.subname(src, name);
        pfSourcePfaheadFeedbackRescueActive.subname(src, name);
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
    if (has_target_pa && cacheSnoop && queueFilter &&
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
            if (l1dPfaheadFeedbackActive() &&
                dpp.pfaheadFeedbackSource == nullptr) {
                dpp.pfaheadFeedbackSource = this;
            }
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
        if (!sourceAdmissionAllowPFQ(dpp)) {
            DPRINTF(HWPrefetch,
                    "PFQ admission rejected addr:%#x source:%d ingress:%d\n",
                    dpp.pfInfo.getAddr(),
                    dpp.pfInfo.getXsMetadata().prefetchSource,
                    static_cast<int>(dpp.ingress));
            delete dpp.pkt;
            return;
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
        const auto removed_src = it->pfInfo.getXsMetadata().prefetchSource;
        statsQueued.pfRemovedFull_srcs[removed_src]++;
        sourceAdmissionAccountQueueFull(removed_src);

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
        if (dpp_it->pfInfo.getXsMetadata().prefetchSource != PrefetchSourceType::CDP) {
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

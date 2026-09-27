#include "mem/cache/prefetch/xs_stream.hh"

#include "debug/XsStreamPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "sim/core.hh"

namespace gem5
{
namespace prefetch
{

XsStreamPrefetcher::XsStreamPrefetcher(const XsStreamPrefetcherParams &p)
    : Queued(p),
      regionSize(p.region_size),
      regionBlks(p.region_size / p.block_size),    
      depth(p.xs_stream_depth),
      badPreNum(0),
      enableAutoDepth(p.enable_auto_depth),
      enableL3StreamPre(p.enable_l3_stream_pre),
      l2Depth(p.xs_stream_l2_depth),
      feedbackStats(this),
      stream_array(p.xs_stream_entries, p.xs_stream_entries, p.xs_stream_indexing_policy,
                   p.xs_stream_replacement_policy, STREAMEntry()),
      streamBlkFilter(pfFilterSize)
{
}

XsStreamPrefetcher::FeedbackStats::FeedbackStats(XsStreamPrefetcher *parent)
    : statistics::Group(parent, "feedback"),
      ADD_STAT(windows, statistics::units::Count::get(),
               "Completed stream feedback windows"),
      ADD_STAT(sent, statistics::units::Count::get(),
               "Stream prefetches dequeued in the last complete window"),
      ADD_STAT(tlbMisses, statistics::units::Count::get(),
               "Stream translation requests missing the L1 TLB in the last window"),
      ADD_STAT(dcacheHits, statistics::units::Count::get(),
               "Stream prefetches dropped by a dcache hit in the last window"),
      ADD_STAT(pdbHits, statistics::units::Count::get(),
               "Stream prefetches dropped by a PDB hit in the last window"),
      ADD_STAT(mshrHits, statistics::units::Count::get(),
               "Stream prefetches dropped by a MSHR hit in the last window"),
      ADD_STAT(pdbLoadUses, statistics::units::Count::get(),
               "Stream PDB lines first used by a load in the last window"),
      ADD_STAT(pdbUnusedReplacements, statistics::units::Count::get(),
               "Unused stream PDB lines capacity-replaced in the last window"),
      ADD_STAT(pdbRefills, statistics::units::Count::get(),
               "Stream prefetch lines inserted into PDB in the last window"),
      ADD_STAT(pdbRefillIntervalSamples, statistics::units::Count::get(),
               "Intervals between stream PDB refills in the last window"),
      ADD_STAT(pdbRefillIntervalAvgCycles, statistics::units::Cycle::get(),
               "Mean cycles between stream PDB refills in the last window"),
      ADD_STAT(mshrResponseSamples, statistics::units::Count::get(),
               "Stream MSHR downstream response samples in the last window"),
      ADD_STAT(mshrResponseAvgCycles, statistics::units::Cycle::get(),
               "Mean stream MSHR send-to-data-response cycles in the last window"),
      ADD_STAT(refillToUseSamples, statistics::units::Count::get(),
               "Stream PDB first-use latency samples in the last window"),
      ADD_STAT(refillToUseAvgCycles, statistics::units::Cycle::get(),
               "Mean stream PDB refill-to-first-use cycles in the last window"),
      ADD_STAT(refillToReplaceSamples, statistics::units::Count::get(),
               "Stream PDB replacement latency samples in the last window"),
      ADD_STAT(refillToReplaceAvgCycles, statistics::units::Cycle::get(),
               "Mean stream PDB refill-to-replacement cycles in the last window"),
      ADD_STAT(useToReplaceSamples, statistics::units::Count::get(),
               "Used stream PDB replacement latency samples in the last window"),
      ADD_STAT(useToReplaceAvgCycles, statistics::units::Cycle::get(),
               "Mean stream PDB first-use-to-replacement cycles in the last window"),
      ADD_STAT(depth, statistics::units::Count::get(),
               "Stream prefetch depth after the last complete window")
{
}

void
XsStreamPrefetcher::recordStreamProbe(
    PrefetchSourceType source, Base::PrefetchProbeResult result)
{
    switch (result) {
      case Base::PrefetchProbeResult::DcacheHit:
        ++feedback.dcacheHits;
        break;
      case Base::PrefetchProbeResult::PdbHit:
        ++feedback.pdbHits;
        break;
      case Base::PrefetchProbeResult::MshrHit:
        ++feedback.mshrHits;
        break;
      case Base::PrefetchProbeResult::WriteBufferHit:
      case Base::PrefetchProbeResult::Sent:
        break;
    }
    if (source == PrefetchSourceType::SStream &&
        result != Base::PrefetchProbeResult::Sent) {
        ++feedback.lateHits;
    }
    if (feedback.sent == VALIDITYCHECKINTERVAL)
        completeFeedbackWindow();
}

void
XsStreamPrefetcher::recordStreamPdbRefill()
{
    const Tick now = curTick();
    if (feedback.pdbRefills) {
        feedback.pdbRefillIntervalCycles +=
            ticksToCycles(now - feedback.lastPdbRefillTick);
        ++feedback.pdbRefillIntervalSamples;
    }
    feedback.lastPdbRefillTick = now;
    ++feedback.pdbRefills;
}

void
XsStreamPrefetcher::recordStreamMshrResponse(uint64_t latency_cycles)
{
    ++feedback.mshrResponseSamples;
    feedback.mshrResponseCycles += latency_cycles;
}

void
XsStreamPrefetcher::recordStreamPdbFirstUse(bool load,
                                             uint64_t refill_to_use)
{
    if (load)
        ++feedback.pdbLoadUses;
    ++feedback.refillToUseSamples;
    feedback.refillToUseCycles += refill_to_use;
}

void
XsStreamPrefetcher::recordStreamPdbReplacement(
    bool used, uint64_t refill_to_replace, uint64_t use_to_replace)
{
    if (!used)
        ++feedback.pdbUnusedReplacements;
    ++feedback.refillToReplaceSamples;
    feedback.refillToReplaceCycles += refill_to_replace;
    if (used) {
        ++feedback.useToReplaceSamples;
        feedback.useToReplaceCycles += use_to_replace;
    }
}

void
XsStreamPrefetcher::completeFeedbackWindow()
{
    const auto &window = feedback;
    const auto mean = [](uint64_t cycles, uint64_t samples) {
        return samples ? double(cycles) / samples : 0.0;
    };
    const double refill_to_use =
        mean(window.refillToUseCycles, window.refillToUseSamples);
    const double refill_to_replace =
        mean(window.refillToReplaceCycles, window.refillToReplaceSamples);
    const double use_to_replace =
        mean(window.useToReplaceCycles, window.useToReplaceSamples);
    const double pdb_refill_interval = mean(
        window.pdbRefillIntervalCycles, window.pdbRefillIntervalSamples);
    const double mshr_response_latency = mean(
        window.mshrResponseCycles, window.mshrResponseSamples);

    // A new depth rule can use window.tlbMisses, dcacheHits, pdbHits,
    // mshrHits, pdbLoadUses, pdbUnusedReplacements, pdb_refill_interval,
    // mshr_response_latency,
    // and the three latency means above.
    // Each mean is zero when its corresponding sample count is zero.
    const int old_depth = depth;
    if (enableAutoDepth) {
        if (double(window.lateHits) / window.sent >= LATECOVERAGE &&
            depth != DEPTHRIGHT) {
            depth <<= DEPTHSTEP;
        }
        if (badPreNum > LATEMISSTHRESHOLD && depth != DEPTHLEFT)
            depth >>= DEPTHSTEP;
    }

    ++feedbackStats.windows;
    feedbackStats.sent = window.sent;
    feedbackStats.tlbMisses = window.tlbMisses;
    feedbackStats.dcacheHits = window.dcacheHits;
    feedbackStats.pdbHits = window.pdbHits;
    feedbackStats.mshrHits = window.mshrHits;
    feedbackStats.pdbLoadUses = window.pdbLoadUses;
    feedbackStats.pdbUnusedReplacements = window.pdbUnusedReplacements;
    feedbackStats.pdbRefills = window.pdbRefills;
    feedbackStats.pdbRefillIntervalSamples = window.pdbRefillIntervalSamples;
    feedbackStats.pdbRefillIntervalAvgCycles = pdb_refill_interval;
    feedbackStats.mshrResponseSamples = window.mshrResponseSamples;
    feedbackStats.mshrResponseAvgCycles = mshr_response_latency;
    feedbackStats.refillToUseSamples = window.refillToUseSamples;
    feedbackStats.refillToUseAvgCycles = refill_to_use;
    feedbackStats.refillToReplaceSamples = window.refillToReplaceSamples;
    feedbackStats.refillToReplaceAvgCycles = refill_to_replace;
    feedbackStats.useToReplaceSamples = window.useToReplaceSamples;
    feedbackStats.useToReplaceAvgCycles = use_to_replace;
    feedbackStats.depth = depth;

    DPRINTF(XsStreamPrefetcher,
            "auto depth: %d -> %d, sent=%llu late=%llu bad=%d "
            "tlbMiss=%llu dcache=%llu pdb=%llu mshr=%llu loadUse=%llu "
            "unusedReplace=%llu pdbRefills=%llu pdbRefillInterval=%.3f/%llu "
            "mshrResponse=%.3f/%llu "
            "refillToUse=%.3f/%llu "
            "refillToReplace=%.3f/%llu useToReplace=%.3f/%llu\n",
            old_depth, depth, window.sent, window.lateHits, badPreNum,
            window.tlbMisses, window.dcacheHits, window.pdbHits,
            window.mshrHits, window.pdbLoadUses, window.pdbUnusedReplacements,
            window.pdbRefills, pdb_refill_interval,
            window.pdbRefillIntervalSamples,
            mshr_response_latency, window.mshrResponseSamples,
            refill_to_use, window.refillToUseSamples,
            refill_to_replace, window.refillToReplaceSamples,
            use_to_replace, window.useToReplaceSamples);

    feedback = {};
    badPreNum = 0;
    issuedPrefetches = 0;
}

void
XsStreamPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                      std::vector<AddrPriority> &addresses)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    PrefetchSourceType stream_type = PrefetchSourceType::SStream;
    bool in_active_page = false;
    bool decr = false;
    if (pfi.isStore()) {
        stream_type = PrefetchSourceType::StoreStream;
        DPRINTF(XsStreamPrefetcher, "prefetch trigger come from store unit\n");
    }
    if (pfi.isCacheMiss() &&
        streamBlkFilter.contains(contextKey(block_addr, context_id))) {
        badPreNum++;
    }
    STREAMEntry *entry = streamLookup(pfi, in_active_page, decr);
    if (in_active_page) {
        Addr pf_stream_l1 = decr ? block_addr - depth * blkSize : block_addr + depth * blkSize;
        sendPFWithFilter(pfi, pf_stream_l1, addresses, 1, stream_type, L1BLKDEGREE, 1, entry);
        const auto l2_depth = l2Depth ? l2Depth : (depth << l2Ratio);
        Addr pf_stream_l2 = decr ? block_addr - l2_depth * blkSize :
                                   block_addr + l2_depth * blkSize;
        sendPFWithFilter(pfi, pf_stream_l2, addresses, 1, stream_type, L2BLKDEGREE, 2, entry);
        if (enableL3StreamPre) {
            Addr pf_stream_l3 =
                decr ? block_addr - (depth << l3Ratio) * blkSize : block_addr + (depth << l3Ratio) * blkSize;
            sendPFWithFilter(pfi, pf_stream_l3, addresses, 1, stream_type, L3BLKDEGREE, 3, entry);
        }
    }
}

XsStreamPrefetcher::STREAMEntry *
XsStreamPrefetcher::streamLookup(const PrefetchInfo &pfi, bool &in_active_page, bool &decr)
{
    Addr pc = pfi.getPC();
    Addr vaddr = pfi.getAddr();
    Addr vaddr_tag_num = tagAddress(vaddr);
    Addr vaddr_offset = tagOffset(vaddr);
    bool secure = pfi.isSecure();
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;

    STREAMEntry *entry = stream_array.findEntry(
        contextKey(regionHashTag(vaddr_tag_num), context_id), secure);
    STREAMEntry *entry_plus = stream_array.findEntry(
        contextKey(regionHashTag(vaddr_tag_num + 1), context_id), secure);
    STREAMEntry *entry_min = stream_array.findEntry(
        contextKey(regionHashTag(vaddr_tag_num - 1), context_id), secure);

    bool entry_plus_active = entry_plus && entry_plus->active;
    bool entry_min_active = entry_min && entry_min->active;

    if (entry) {
        stream_array.accessEntry(entry);
        uint64_t region_bit_accessed = 1UL << vaddr_offset;
        if (entry_plus)
            entry->decrMode = true;
        if ((entry_plus_active || entry_min_active) || (entry->cnt > ACTIVETHRESHOLD))
            entry->active = true;
        in_active_page = entry->active;
        decr = entry->decrMode;
        if (!(entry->bitVec & region_bit_accessed)) {
            entry->bitVec |= region_bit_accessed;
            entry->cnt += 1;
        }
        return entry;
    }
    Addr stream_key =
        contextKey(regionHashTag(vaddr_tag_num), context_id);
    entry = stream_array.findVictim(stream_key);

    in_active_page = (entry_plus_active || entry_min_active);
    decr = entry_plus != nullptr;
    entry->tag = regionHashTag(vaddr_tag_num);
    entry->decrMode = decr;
    entry->bitVec = 1UL << vaddr_offset;
    entry->cnt = 1;
    entry->active = in_active_page;
    entry->contextId = context_id;
    stream_array.insertEntry(stream_key, secure, entry);
    return entry;
}

void
XsStreamPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
                                     int prio, PrefetchSourceType src, int pf_degree, int ahead_level, STREAMEntry *entry)
{
    uint64_t region_bit = 0;
    for (int i = 0; i < pf_degree; i++) {
        Addr pf_addr = addr + i * blkSize;
        region_bit |= (uint64_t(1) << regionOffset(pf_addr));

        // Count generated prefetch
        prefetchStats.pfGenerated++;

        Addr filter_key = sharedFilterKey(pfi, pf_addr);
        if (filter->contains(filter_key)) {
            DPRINTF(XsStreamPrefetcher, "Skip recently prefetched: %lx\n", pf_addr);
            // Count filtered prefetch
            prefetchStats.pfFiltered++;
        } else {
            DPRINTF(XsStreamPrefetcher, "Send pf: %lx\n", pf_addr);
            filter->insert(filter_key, 0);
            addresses.push_back(AddrPriority(pf_addr, prio, src));
            streamBlkFilter.insert(filter_key, 0);
            if (ahead_level > 1) {
                assert(ahead_level == 2 || ahead_level == 3);
                addresses.back().pfahead_host = ahead_level;
                addresses.back().pfahead = true;
            } else {
                addresses.back().pfahead = false;
            }
        }
    }
    pfi.setTriggerInfo_PFsrc(src);
    if (ahead_level > 1) {
        stridestream_pfFilter_l2l3->Insert(regionAddress(addr), region_bit,0,true,entry->decrMode,pfi.isSecure(),ahead_level, &pfi.trigger_info);
    } else {
        stridestream_pfFilter_l1->Insert(regionAddress(addr), region_bit,0,true,entry->decrMode,pfi.isSecure(),ahead_level, &pfi.trigger_info);
    }
}


}
}

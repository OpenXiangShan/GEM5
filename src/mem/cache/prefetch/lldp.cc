#include "mem/cache/prefetch/lldp.hh"

#include <algorithm>
#include <limits>
#include <set>

#include "cpu/base.hh"
#include "debug/LLDPrefetcher.hh"
#include "params/LLDPrefetcher.hh"

namespace gem5
{
namespace prefetch
{
LLDPrefetcher::LLDPStats::LLDPStats(statistics::Group *parent)
    : statistics::Group(parent, "lldp"),
      ADD_STAT(trainAccepted, statistics::units::Count::get(), "Accepted dependency trains"),
      ADD_STAT(trainDropped, statistics::units::Count::get(), "Training FIFO overflows"),
      ADD_STAT(trainSquashed, statistics::units::Count::get(), "Squashed training requests"),
      ADD_STAT(dualSourceTrainRejected, statistics::units::Count::get(),
               "Dependency trains excluded because their chains contain dual-register operations"),
      ADD_STAT(producerWrites, statistics::units::Count::get(), "LLDT producer allocations and updates"),
      ADD_STAT(producerReplacements, statistics::units::Count::get(), "LLDT producer evictions"),
      ADD_STAT(producerMatches, statistics::units::Count::get(), "Load PC matches in LLDT"),
      ADD_STAT(pipelineBypasses, statistics::units::Count::get(), "S1 revalidation after older S2 writes"),
      ADD_STAT(spatialLoadTrain, statistics::units::Count::get(), "Spatial-prefetch loadTrain inputs"),
      ADD_STAT(spatialHints, statistics::units::Count::get(), "Hints produced for spatial prefetches"),
      ADD_STAT(lengthChanges, statistics::units::Count::get(), "Consumer chain length changes"),
      ADD_STAT(opChanges, statistics::units::Count::get(), "Consumer operation changes"),
      ADD_STAT(immChanges, statistics::units::Count::get(), "Consumer arithmetic or load immediate changes"),
      ADD_STAT(exactImmStable, statistics::units::Count::get(), "Exact immediate repeats"),
      ADD_STAT(lineImmStable, statistics::units::Count::get(), "Cacheline immediate repeats"),
      ADD_STAT(offsetImmStable, statistics::units::Count::get(), "Byte-offset immediate repeats"),
      ADD_STAT(sameLineImmUpdates, statistics::units::Count::get(), "Immediate updates on the same line"),
      ADD_STAT(nextLineImmUpdates, statistics::units::Count::get(), "Immediate updates on the next line"),
      ADD_STAT(otherLineImmUpdates, statistics::units::Count::get(), "Immediate updates on another line"),
      ADD_STAT(immValueHist, statistics::units::Count::get(), "Immediate delta histogram"),
      ADD_STAT(lineDeltaHist, statistics::units::Count::get(), "Cacheline delta histogram"),
      ADD_STAT(offsetDeltaHist, statistics::units::Count::get(), "Byte-offset delta histogram"),
      ADD_STAT(byteOffsetHist, statistics::units::Count::get(), "Load-size and byte-offset histogram"),
      ADD_STAT(hints, statistics::units::Count::get(), "Hints retained on cache misses"),
      ADD_STAT(hitHintsDiscarded, statistics::units::Count::get(), "Hints discarded on cache hits"),
      ADD_STAT(hitHintsRetained, statistics::units::Count::get(),
               "Deprecated retained-hit counter (must remain zero)"),
      ADD_STAT(returnedHints, statistics::units::Count::get(), "MSHR targets returning hinted data"),
      ADD_STAT(staleHints, statistics::units::Count::get(), "Hints whose producer generation was evicted"),
      ADD_STAT(candidates, statistics::units::Count::get(), "LLDP virtual address candidates"),
      ADD_STAT(filtered, statistics::units::Count::get(), "Candidates filtered by recent TLB-line history"),
      ADD_STAT(duplicates, statistics::units::Count::get(), "Duplicate candidate cache lines"),
      ADD_STAT(unsupported, statistics::units::Count::get(), "Chains or data unsafe to replay"),
      ADD_STAT(samplerOutputsToMeta, statistics::units::Count::get(),
               "Stable address pairs emitted from SamplerTable to MetaTable"),
      ADD_STAT(metaTableHits, statistics::units::Count::get(),
               "loadTrain addresses hitting MetaTable"),
      ADD_STAT(metaTablePrefetches, statistics::units::Count::get(),
               "Prefetches queued directly from MetaTable"),
      ADD_STAT(samplerValidEntries, statistics::units::Count::get(),
               "Current valid SamplerTable entries"),
      ADD_STAT(metaValidEntries, statistics::units::Count::get(),
               "Current valid MetaTable entries"),
      ADD_STAT(samplerTargetMismatch, statistics::units::Count::get(),
               "Sampler mappings that changed target address"),
      ADD_STAT(samplerRepromotions, statistics::units::Count::get(),
               "Sampler stable mappings refreshed into MetaTable"),
      ADD_STAT(metaInvalidations, statistics::units::Count::get(),
               "MetaTable entries invalidated by negative feedback"),
      ADD_STAT(metaTokenStalls, statistics::units::Count::get(),
               "MetaTable lookups blocked by token or outstanding limits"),
      ADD_STAT(metaFallbacks, statistics::units::Count::get(),
               "LLDP hints retained for consumers not covered by MetaTable"),
      ADD_STAT(samplerReplacementCnt, statistics::units::Count::get(),
               "SamplerTable victim count distribution"),
      ADD_STAT(candidateGenerated, statistics::units::Count::get(), "Candidate lifecycles generated"),
      ADD_STAT(candidateQueued, statistics::units::Count::get(), "Candidate lifecycles queued"),
      ADD_STAT(candidateIssued, statistics::units::Count::get(), "Candidate lifecycles issued"),
      ADD_STAT(candidateDropped, statistics::units::Count::get(), "Candidate lifecycles dropped before issue"),
      ADD_STAT(candidateMerged, statistics::units::Count::get(), "Candidate lifecycles merged with demand"),
      ADD_STAT(candidateUseful, statistics::units::Count::get(), "Candidate lifecycles useful"),
      ADD_STAT(candidateUnused, statistics::units::Count::get(), "Candidate lifecycles unused"),
      ADD_STAT(candidateLate, statistics::units::Count::get(), "Candidate lifecycles late"),
      ADD_STAT(candidateCacheHit, statistics::units::Count::get(), "Candidates hitting cache"),
      ADD_STAT(candidateMshrHit, statistics::units::Count::get(), "Candidates hitting MSHR"),
      ADD_STAT(candidateWbHit, statistics::units::Count::get(), "Candidates hitting write buffer"),
      ADD_STAT(childrenAtReplacement, statistics::units::Count::get(), "Valid consumers per evicted producer"),
      ADD_STAT(childrenAtDump, statistics::units::Count::get(), "Current producers by valid consumer count"),
      ADD_STAT(pcpActiveHistogram, statistics::units::Count::get(), "Current producers by active consumer count"),
      ADD_STAT(pcpActivePeak, statistics::units::Count::get(), "Peak active consumers by producer row"),
      ADD_STAT(pcpConsumerReplacement, statistics::units::Count::get(), "Consumer replacements by producer row"),
      ADD_STAT(pcpConsumerOverflow, statistics::units::Count::get(), "Consumer overflows by producer row"),
      ADD_STAT(pcpCandidate, statistics::units::Count::get(), "Candidates by producer row"),
      ADD_STAT(pcpUseful, statistics::units::Count::get(), "Useful candidates by producer row"),
      ADD_STAT(pcpLate, statistics::units::Count::get(), "Late candidates by producer row"),
      ADD_STAT(pcpProducerPC, statistics::units::Count::get(), "Producer PC by row"),
      ADD_STAT(pcpContext, statistics::units::Count::get(), "Producer context by row"),
      ADD_STAT(pcpValid, statistics::units::Count::get(), "Producer valid bit by row"),
      ADD_STAT(pcpConf, statistics::units::Count::get(), "Producer confidence by row"),
      ADD_STAT(pcpDemandPC, statistics::units::Count::get(), "Last demand PC matched to a candidate by row"),
      ADD_STAT(pcpDemandChainId, statistics::units::Count::get(), "Last matched LLDT generation by row"),
      ADD_STAT(pcpDemandSource, statistics::units::Count::get(), "Last matched prefetch source by row"),
      ADD_STAT(pcpDemandHits, statistics::units::Count::get(), "Candidate demand matches by row"),
      ADD_STAT(consumerValid, statistics::units::Count::get(), "Consumer valid bit by row and column"),
      ADD_STAT(consumerPC, statistics::units::Count::get(), "Consumer PC by row and column"),
      ADD_STAT(consumerLength, statistics::units::Count::get(), "Consumer chain length by row and column"),
      ADD_STAT(consumerOp1, statistics::units::Count::get(), "Consumer first operation by row and column"),
      ADD_STAT(consumerOp2, statistics::units::Count::get(), "Consumer second operation by row and column"),
      ADD_STAT(consumerImmLoad, statistics::units::Count::get(), "Consumer load immediate by row and column"),
      ADD_STAT(consumerImmConf, statistics::units::Count::get(), "Consumer exact-immediate confidence"),
      ADD_STAT(consumerLineConf, statistics::units::Count::get(), "Consumer immediate-line confidence"),
      ADD_STAT(consumerOffsetConf, statistics::units::Count::get(), "Consumer immediate-offset confidence"),
      ADD_STAT(consumerConf, statistics::units::Count::get(), "Consumer confidence by row and column"),
      ADD_STAT(consumerUpdates, statistics::units::Count::get(), "Consumer updates by row and column"),
      ADD_STAT(consumerReplacements, statistics::units::Count::get(), "Consumer replacements by row and column"),
      ADD_STAT(consumerExactImmStable, statistics::units::Count::get(), "Consumer exact-immediate stable updates"),
      ADD_STAT(consumerLineImmStable, statistics::units::Count::get(), "Consumer immediate-line stable updates"),
      ADD_STAT(consumerCandidates, statistics::units::Count::get(), "Consumer candidates by row and column"),
      ADD_STAT(consumerUseful, statistics::units::Count::get(), "Consumer useful candidates by row and column"),
      ADD_STAT(consumerLate, statistics::units::Count::get(), "Consumer late candidates by row and column"),
      ADD_STAT(consumerDemandHits, statistics::units::Count::get(), "Consumer candidate demand matches"),
      ADD_STAT(validProducers, statistics::units::Count::get(), "Current valid LLDT producers")
{
    samplerReplacementCnt.init(256);
    immValueHist.init(256);
    lineDeltaHist.init(129);
    offsetDeltaHist.init(127);
    byteOffsetHist.init(5 * 64);
    childrenAtReplacement.init(SubEntries + 1);
    childrenAtDump.init(SubEntries + 1);
    pcpActiveHistogram.init(SubEntries + 1);
    pcpActivePeak.init(TableEntries);
    pcpConsumerReplacement.init(TableEntries);
    pcpConsumerOverflow.init(TableEntries);
    pcpCandidate.init(TableEntries);
    pcpUseful.init(TableEntries);
    pcpLate.init(TableEntries);
    pcpProducerPC.init(TableEntries);
    pcpContext.init(TableEntries);
    pcpValid.init(TableEntries);
    pcpConf.init(TableEntries);
    pcpDemandPC.init(TableEntries);
    pcpDemandChainId.init(TableEntries);
    pcpDemandSource.init(TableEntries);
    pcpDemandHits.init(TableEntries);
    consumerValid.init(TableEntries * SubEntries);
    consumerPC.init(TableEntries * SubEntries);
    consumerLength.init(TableEntries * SubEntries);
    consumerOp1.init(TableEntries * SubEntries);
    consumerOp2.init(TableEntries * SubEntries);
    consumerImmLoad.init(TableEntries * SubEntries);
    consumerImmConf.init(TableEntries * SubEntries);
    consumerLineConf.init(TableEntries * SubEntries);
    consumerOffsetConf.init(TableEntries * SubEntries);
    consumerConf.init(TableEntries * SubEntries);
    consumerUpdates.init(TableEntries * SubEntries);
    consumerReplacements.init(TableEntries * SubEntries);
    consumerExactImmStable.init(TableEntries * SubEntries);
    consumerLineImmStable.init(TableEntries * SubEntries);
    consumerCandidates.init(TableEntries * SubEntries);
    consumerUseful.init(TableEntries * SubEntries);
    consumerLate.init(TableEntries * SubEntries);
    consumerDemandHits.init(TableEntries * SubEntries);
}

LLDPrefetcher::LLDPrefetcher(const LLDPrefetcherParams &p)
    : Queued(p), learningEvent([this] { learningTick(); }, name() + ".learning"),
      trainingQueueSize(p.training_queue_size),
      maxConf((1U << std::min(p.confidence_bits, 8U)) - 1),
      initialConf(p.initial_confidence), producerInitialConf(p.producer_initial_confidence),
      producerThreshold(p.producer_threshold), consumerThreshold(p.consumer_threshold),
      immediateThreshold(p.immediate_threshold), trainingCPU(p.training_cpu), stats(this)
{
    fatal_if(p.confidence_bits == 0 || p.confidence_bits > 8 || !trainingQueueSize ||
        initialConf > maxConf || producerInitialConf > maxConf ||
        producerThreshold > maxConf || consumerThreshold > maxConf ||
        immediateThreshold > maxConf, "Invalid LLDP confidence/FIFO parameters");
    fatal_if(!useVirtualAddresses, "LLDP replay generates virtual addresses");
}

void
LLDPrefetcher::regProbeListeners()
{
    Queued::regProbeListeners();
    if (trainingCPU) {
        dependenceListener = trainingCPU->getProbeManager()->connect<DependenceListener>(
            *this, "dependenceTrain");
    }
    fatal_if(!tlb, "LLDP requires a registered data TLB");
    // Use the timing PTW retry path on a TLB miss, rather than functional lookup.
    functionalTLB = false;
}

int
LLDPrefetcher::findProducer(Addr pc, ContextID context) const
{
    for (unsigned i = 0; i < table.size(); ++i)
        if (table[i].valid && table[i].producerPC == pc && table[i].context == context)
            return i;
    return -1;
}

int
LLDPrefetcher::findConsumer(const Entry &entry, Addr pc) const
{
    for (unsigned i = 0; i < SubEntries; ++i)
        if (entry.consumers[i].valid && entry.consumers[i].consumerPC == pc)
            return i;
    return -1;
}

bool
LLDPrefetcher::isSpatialPrefetch(const PacketPtr &pkt) const
{
    if (!pkt || !pkt->req || !pkt->req->isPrefetch() ||
        !pkt->req->hasXsMetadata())
        return false;
    switch (pkt->req->getXsMetadata().prefetchSource) {
      case PrefetchSourceType::SStream:
      case PrefetchSourceType::SStride:
      case PrefetchSourceType::SPht:
      case PrefetchSourceType::HWP_BOP:
      case PrefetchSourceType::SPP:
      case PrefetchSourceType::IPCP:
      case PrefetchSourceType::IPCP_CS:
      case PrefetchSourceType::IPCP_CPLX:
      case PrefetchSourceType::Berti:
      case PrefetchSourceType::SOpt:
      case PrefetchSourceType::DespacitoStream:
        return true;
      default:
        return false;
    }
}

bool
LLDPrefetcher::isLldpSource(PrefetchSourceType source)
{
    return source == PrefetchSourceType::LLDP ||
        source == PrefetchSourceType::LLDPS ||
        source == PrefetchSourceType::LLDPT;
}

unsigned
LLDPrefetcher::samplerSet(Addr addr_p) const
{
    return (addr_p ^ (addr_p >> 6)) & (SamplerSets - 1);
}

unsigned
LLDPrefetcher::metaSet(Addr addr_p) const
{
    return (addr_p ^ (addr_p >> 6)) & (MetaSets - 1);
}

unsigned
LLDPrefetcher::samplerVictim(unsigned set)
{
    auto &ways = samplerTable[set];
    for (;;) {
        unsigned victim = 0;
        uint8_t best = 0;
        for (unsigned way = 0; way < AddressTableWays; ++way) {
            if (!ways[way].valid)
                return way;
            if (ways[way].rrpv >= best) {
                best = ways[way].rrpv;
                victim = way;
            }
        }
        if (best >= 3)
            return victim;
        for (auto &entry : ways)
            entry.rrpv = std::min<uint8_t>(3, entry.rrpv + 1);
    }
}

unsigned
LLDPrefetcher::metaVictim(unsigned set)
{
    auto &ways = metaTable[set];
    for (;;) {
        unsigned victim = 0;
        uint8_t best = 0;
        for (unsigned way = 0; way < AddressTableWays; ++way) {
            if (!ways[way].valid)
                return way;
            if (ways[way].rrpv >= best) {
                best = ways[way].rrpv;
                victim = way;
            }
        }
        if (best >= 3)
            return victim;
        for (auto &entry : ways)
            entry.rrpv = std::min<uint8_t>(3, entry.rrpv + 1);
    }
}

void
LLDPrefetcher::updateMetaTable(const SamplerEntry &sample)
{
    const unsigned set = metaSet(sample.addrP ^ sample.producerPC ^
                                  sample.consumerPC);
    auto &ways = metaTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        auto &entry = ways[way];
        if (entry.valid && entry.addrP == sample.addrP &&
            entry.producerPC == sample.producerPC &&
            entry.consumerPC == sample.consumerPC &&
            entry.context == sample.context) {
            if (entry.addrC != sample.addrC) {
                entry.trainConf = entry.trainConf > 1 ? entry.trainConf - 2 : 0;
                entry.tokens = 0;
                return;
            }
            entry.trainConf = std::min<uint8_t>(7, entry.trainConf + 1);
            entry.lastTrainEpoch = tableEpoch;
            entry.rrpv = 0;
            return;
        }
    }

    const unsigned victim = metaVictim(set);
    auto &entry = ways[victim];
    const uint32_t next_generation = entry.generation + 1;
    entry = {};
    entry.valid = true;
    entry.addrP = sample.addrP;
    entry.addrC = sample.addrC;
    entry.producerPC = sample.producerPC;
    entry.consumerPC = sample.consumerPC;
    entry.context = sample.context;
    entry.generation = next_generation;
    entry.trainConf = 3;
    entry.qualityConf = 4;
    entry.timelyConf = 4;
    entry.tokens = 2;
    entry.lastTrainEpoch = tableEpoch;
    entry.rrpv = 0;
}

void
LLDPrefetcher::trainAddressPair(Addr addr_p, Addr addr_c, Addr producer_pc,
                                Addr consumer_pc, ContextID context)
{
    const unsigned set = samplerSet(addr_p);
    auto &ways = samplerTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        auto &entry = ways[way];
        if (!entry.valid || entry.addrP != addr_p ||
            entry.producerPC != producer_pc || entry.consumerPC != consumer_pc ||
            entry.context != context)
            continue;
        entry.lastSeenEpoch = tableEpoch;
        if (entry.addrC != addr_c) {
            stats.samplerTargetMismatch++;
            entry.mismatchCount = std::min<uint8_t>(7, entry.mismatchCount + 1);
            if (entry.stableCount)
                --entry.stableCount;
            if (!entry.stableCount) {
                entry.addrC = addr_c;
                entry.stableCount = 1;
                entry.mismatchCount = 0;
                entry.promotionVersion++;
            }
            entry.rrpv = 3;
            return;
        }
        entry.stableCount = std::min<uint8_t>(7, entry.stableCount + 1);
        entry.mismatchCount = 0;
        entry.rrpv = 0;
        if (entry.stableCount == SamplerThreshold) {
            updateMetaTable(entry);
            stats.samplerOutputsToMeta++;
            entry.matchesSincePromotion = 0;
        } else if (entry.stableCount > SamplerThreshold &&
                   ++entry.matchesSincePromotion >= 8) {
            updateMetaTable(entry);
            stats.samplerRepromotions++;
            entry.matchesSincePromotion = 0;
        }
        return;
    }

    const unsigned victim = samplerVictim(set);
    if (ways[victim].valid)
        stats.samplerReplacementCnt[ways[victim].stableCount]++;
    ways[victim] = {};
    ways[victim].valid = true;
    ways[victim].addrP = addr_p;
    ways[victim].producerPC = producer_pc;
    ways[victim].consumerPC = consumer_pc;
    ways[victim].context = context;
    ways[victim].addrC = addr_c;
    ways[victim].stableCount = 1;
    ways[victim].rrpv = 3;
    ways[victim].lastSeenEpoch = tableEpoch;
}

std::optional<LLDPrefetcher::MetaHit>
LLDPrefetcher::lookupMetaTable(Addr addr_p, Addr producer_pc,
                               Addr consumer_pc, ContextID context)
{
    const unsigned set = metaSet(addr_p ^ producer_pc ^ consumer_pc);
    auto &ways = metaTable[set];
    for (unsigned way = 0; way < AddressTableWays; ++way) {
        auto &entry = ways[way];
        if (entry.valid && entry.addrP == addr_p &&
            entry.producerPC == producer_pc &&
            entry.consumerPC == consumer_pc && entry.context == context) {
            if (entry.trainConf < 3 || entry.qualityConf < 2 ||
                !entry.tokens || entry.outstanding) {
                stats.metaTokenStalls++;
                return std::nullopt;
            }
            return MetaHit{entry.addrC, set, way, entry.generation};
        }
    }
    return std::nullopt;
}

void
LLDPrefetcher::ageMetaTable()
{
    if (++tableEpoch % 4096)
        return;
    for (auto &ways : metaTable) {
        for (auto &entry : ways) {
            if (!entry.valid || tableEpoch - entry.lastTrainEpoch < 32768)
                continue;
            if (entry.trainConf)
                --entry.trainConf;
            entry.lastTrainEpoch = tableEpoch;
            if (!entry.trainConf) {
                entry.valid = false;
                stats.metaInvalidations++;
            }
        }
    }
}

void
LLDPrefetcher::updateMetaOwner(uint64_t candidate_id, int result)
{
    const auto it = candidateOwners.find(candidate_id);
    if (it == candidateOwners.end() || !it->second.meta)
        return;
    auto &entry = metaTable[it->second.metaSet][it->second.metaWay];
    if (!entry.valid || entry.generation != it->second.metaGeneration)
        return;
    entry.outstanding = entry.outstanding ? entry.outstanding - 1 : 0;
    switch (result) {
      case 0: // useful
        entry.qualityConf = std::min<uint8_t>(7, entry.qualityConf + 2);
        entry.timelyConf = std::min<uint8_t>(7, entry.timelyConf + 1);
        entry.tokens = std::min<uint8_t>(15, entry.tokens + 4);
        entry.lastUsefulEpoch = tableEpoch;
        entry.rrpv = 0;
        break;
      case 1: // merged
        entry.qualityConf = std::min<uint8_t>(7, entry.qualityConf + 1);
        entry.timelyConf = entry.timelyConf ? entry.timelyConf - 1 : 0;
        entry.tokens = std::min<uint8_t>(15, entry.tokens + 1);
        entry.rrpv = 0;
        break;
      case 2: // late
        entry.timelyConf = entry.timelyConf > 1 ? entry.timelyConf - 2 : 0;
        entry.tokens = entry.tokens ? entry.tokens - 1 : 0;
        break;
      case 3: // unused
        entry.qualityConf = entry.qualityConf > 1 ? entry.qualityConf - 2 : 0;
        entry.timelyConf = entry.timelyConf ? entry.timelyConf - 1 : 0;
        entry.tokens = 0;
        if (!entry.qualityConf) {
            entry.valid = false;
            stats.metaInvalidations++;
        }
        break;
      case 4: // dropped before issue
        entry.tokens = std::min<uint8_t>(15, entry.tokens + 1);
        break;
      default:
        break;
    }
}

bool
LLDPrefetcher::filterCandidate(Addr line)
{
    if (tlbFilterSet.count(line))
        return true;
    tlbFilter.push_back(line);
    tlbFilterSet.insert(line);
    if (tlbFilter.size() > TlbFilterEntries) {
        tlbFilterTranslations.erase(tlbFilter.front());
        tlbFilterSet.erase(tlbFilter.front());
        tlbFilter.pop_front();
    }
    return false;
}

bool
LLDPrefetcher::rejectTranslatedPrefetch(const DeferredPacket &dpp,
                                        Addr paddr)
{
    const auto metadata = dpp.pfInfo.getXsMetadata();
    if (!metadata.prefetchCandidateId)
        return false;
    const bool translated_source =
        metadata.prefetchSource == PrefetchSourceType::LLDP ||
        metadata.prefetchSource == PrefetchSourceType::LLDPS;
    if (translated_source) {
        trainAddressPair(metadata.prefetchLldpAddrP, blockAddress(paddr),
                         metadata.prefetchProducerPC,
                         metadata.prefetchConsumerPC,
                         dpp.pfInfo.contextId());
    }
    const Addr virtual_line = blockAddress(dpp.pfInfo.getAddr());
    const bool duplicate = filterCandidate(virtual_line);
    if (translated_source)
        tlbFilterTranslations[virtual_line] = blockAddress(paddr);
    if (!duplicate)
        return false;
    stats.filtered++;
    return true;
}

bool
LLDPrefetcher::rejectPrefetchCandidate(const PrefetchInfo &pfi,
                                       const AddrPriority &addr_prio)
{
    const bool translated_source =
        addr_prio.pfSource == PrefetchSourceType::LLDP ||
        addr_prio.pfSource == PrefetchSourceType::LLDPS;
    const Addr virtual_line = blockAddress(pfi.getAddr());
    const bool reject = translated_source &&
        pfi.getXsMetadata().prefetchCandidateId &&
        tlbFilterSet.count(virtual_line);
    if (reject) {
        const auto translation = tlbFilterTranslations.find(virtual_line);
        if (translation != tlbFilterTranslations.end()) {
            trainAddressPair(
                pfi.getXsMetadata().prefetchLldpAddrP,
                translation->second,
                pfi.getXsMetadata().prefetchProducerPC,
                pfi.getXsMetadata().prefetchConsumerPC,
                pfi.contextId());
        }
    }
    stats.filtered += reject;
    return reject;
}

void
LLDPrefetcher::prefetchDropped(const DeferredPacket &dpp)
{
    const auto metadata = dpp.pfInfo.getXsMetadata();
    if (metadata.prefetchCandidateId) {
        updateMetaOwner(metadata.prefetchCandidateId, 4);
        if (candidateOwners.erase(metadata.prefetchCandidateId))
            stats.candidateDropped++;
    }
}

void
LLDPrefetcher::dependenceTrain(const o3::XsDynInstMetaPtr &meta)
{
    if (!meta || !meta->lldpLoad || !meta->lldpChain.valid ||
        meta->lldpContext == InvalidContextID)
        return;
    if (meta->squashed) {
        stats.trainSquashed++;
        return;
    }
    if (meta->lldpChain.dualSrcRegisterOps != 0) {
        stats.dualSourceTrainRejected++;
        DPRINTF(LLDPrefetcher, "reject dual-source PCp=%#x PCc=%#x len=%u dual=%u imm=%u\n",
                meta->lldpChain.producerPC, meta->instAddr, meta->lldpChain.length,
                meta->lldpChain.dualSrcRegisterOps, meta->lldpChain.singleSrcImmediateOps);
        return;
    }
    if (!meta->lldpChain.trainable())
        return;
    if (input.size() >= trainingQueueSize) {
        stats.trainDropped++;
        return;
    }
    input.push_back({meta, meta->lldpChain, meta->instAddr,
                     meta->lldpContext, meta->lldpLoadImm, meta->lldpSize,
                     int64_t(meta->lldpLoadLine), meta->lldpLoadOffset,
                     meta->lldpLoadAddressValid});
    stats.trainAccepted++;
    if (!learningEvent.scheduled())
        schedule(learningEvent, nextCycle());
}

LLDPrefetcher::Update
LLDPrefetcher::makeUpdate(Training train)
{
    if (train.owner->lldpLoadAddressValid) {
        train.loadLine = train.owner->lldpLoadLine;
        train.loadOffset = train.owner->lldpLoadOffset;
        train.loadAddressValid = true;
    }
    if (!train.loadAddressValid) {
        train.loadLine = train.loadImm >= 0 ?
            (train.loadImm / 64) * 64 :
            -(((-train.loadImm + 63) / 64) * 64);
        train.loadOffset = uint8_t(uint64_t(train.loadImm) & 63);
        train.loadAddressValid = true;
    }
    // An older S2 write may allocate/replace the row observed in S0. Revalidate
    // the lookup against that write before constructing the next row image.
    if (train.version != version) {
        stats.pipelineBypasses++;
        train.producer = findProducer(train.chain.producerPC, train.context);
        train.consumer = train.producer < 0 ? -1 :
            findConsumer(table[train.producer], train.consumerPC);
    }
    const bool allocate = train.producer < 0;
    unsigned row = allocate ? replacement.victim() : train.producer;
    if (allocate) {
        for (unsigned i = 0; i < TableEntries; ++i)
            if (!table[i].valid) { row = i; break; }
    }
    Entry entry = allocate ? Entry() : table[row];
    if (allocate) {
        if (table[row].valid)
            entry.replacementCount = table[row].replacementCount + 1;
        entry.valid = true;
        entry.producerPC = train.chain.producerPC;
        entry.context = train.context;
        entry.generation = ++generation;
        entry.pConf = producerInitialConf;
    }
    const bool allocateSub = allocate || train.consumer < 0;
    unsigned col = allocateSub ? entry.replacement.victim() : train.consumer;
    if (allocateSub) {
        if (!allocate && validChildren(entry) == SubEntries)
            entry.consumerOverflow++;
        for (unsigned i = 0; i < SubEntries; ++i)
            if (!entry.consumers[i].valid) { col = i; break; }
        if (!allocate)
            entry.consumerReplacement++;
    }
    auto &sub = entry.consumers[col];
    if (allocateSub) {
        const uint64_t replacements = sub.replacementCount + sub.valid;
        sub = {};
        sub.replacementCount = replacements;
        sub.valid = true;
        sub.consumerPC = train.consumerPC;
        sub.immLine = train.loadImm >= 0 ? train.loadImm / 64 :
            -(((-train.loadImm + 63) / 64));
        sub.loadLine = train.loadLine;
        sub.immOffset = uint8_t(uint64_t(train.loadImm) & 63);
        sub.loadSize = train.loadSize;
        sub.loadAddressValid = train.loadAddressValid;
        sub.cConf = sub.immConf = sub.lineConf = sub.offsetConf = initialConf;
        if (!allocate)
            entry.pConf = std::min<unsigned>(maxConf, entry.pConf + 1);
    } else {
        const int64_t newLine = train.loadImm >= 0 ? train.loadImm / 64 :
            -(((-train.loadImm + 63) / 64));
        const uint8_t newOffset = uint8_t(uint64_t(train.loadImm) & 63);
        if (sub.loadImm == train.loadImm) {
            sub.immConf = std::min<unsigned>(maxConf, sub.immConf + 1);
            stats.exactImmStable++;
            sub.exactImmStableCount++;
        } else if (sub.immConf)
            --sub.immConf;
        if (sub.immLine == newLine) {
            stats.sameLineImmUpdates++;
            stats.lineImmStable++;
            sub.lineImmStableCount++;
            sub.lineConf = std::min<unsigned>(maxConf, sub.lineConf + 1);
        } else if (newLine == sub.immLine + 1) {
            stats.nextLineImmUpdates++;
            if (sub.lineConf)
                --sub.lineConf;
        } else {
            stats.otherLineImmUpdates++;
            if (sub.lineConf)
                --sub.lineConf;
        }
        const bool sameOffset = newOffset == sub.immOffset;
        if (sameOffset) {
            stats.offsetImmStable++;
            sub.offsetConf = std::min<unsigned>(maxConf, sub.offsetConf + 1);
        } else if (sub.offsetConf) {
            --sub.offsetConf;
        }
        const int64_t lineDelta = newLine - sub.immLine;
        stats.lineDeltaHist[std::clamp<int64_t>(lineDelta, -64, 64) + 64]++;
        const int offsetDelta = int(newOffset) - int(sub.immOffset);
        stats.offsetDeltaHist[std::clamp(offsetDelta, -63, 63) + 63]++;
        stats.immValueHist[uint8_t(uint64_t(train.loadImm - sub.loadImm) & 255)]++;
        const unsigned sizeBucket = train.loadSize == 1 ? 0 :
            train.loadSize == 2 ? 1 : train.loadSize == 4 ? 2 :
            train.loadSize == 8 ? 3 : 4;
        stats.byteOffsetHist[sizeBucket * 64 + newOffset]++;
        sub.cConf = std::min<unsigned>(maxConf, sub.cConf + 1);
    }
    sub.chain = train.chain;
    sub.loadImm = train.loadImm;
    sub.immLine = train.loadImm >= 0 ? train.loadImm / 64 :
        -(((-train.loadImm + 63) / 64));
    sub.loadLine = train.loadLine;
    sub.immOffset = uint8_t(uint64_t(train.loadImm) & 63);
    sub.loadSize = train.loadSize;
    sub.loadAddressValid = train.loadAddressValid;
    sub.updateCount++;
    entry.updateCount++;
    entry.activePeak = std::max<uint64_t>(entry.activePeak, validChildren(entry));
    entry.replacement.touch(col);
    return {train, row, entry};
}

unsigned
LLDPrefetcher::validChildren(const Entry &entry) const
{
    return std::count_if(entry.consumers.begin(), entry.consumers.end(),
                         [](const SubEntry &s) { return s.valid; });
}

void
LLDPrefetcher::commitUpdate(const Update &update)
{
    const auto &old = table[update.row];
    Entry committed = update.entry;
    const auto &next = committed;
    if (old.valid && old.generation != next.generation) {
        stats.producerReplacements++;
        stats.childrenAtReplacement[validChildren(old)]++;
    } else if (old.valid) {
        const int before = findConsumer(old, update.training.consumerPC);
        const int after = findConsumer(next, update.training.consumerPC);
        if (before >= 0 && after >= 0) {
            const auto &a = old.consumers[before];
            const auto &b = next.consumers[after];
            stats.lengthChanges += a.chain.length != b.chain.length;
            bool opChange = false, immChange = a.loadImm != b.loadImm;
            for (unsigned i = 0; i < 2; ++i) {
                opChange |= a.chain.ops[i].op != b.chain.ops[i].op ||
                    a.chain.ops[i].word != b.chain.ops[i].word ||
                    a.chain.ops[i].replayable != b.chain.ops[i].replayable;
                immChange |= a.chain.ops[i].imm != b.chain.ops[i].imm;
            }
            stats.opChanges += opChange;
            stats.immChanges += immChange;
        }
    }
    if (old.valid && old.generation == committed.generation) {
        committed.candidateCount = std::max(
            committed.candidateCount, old.candidateCount);
        committed.usefulCount = std::max(
            committed.usefulCount, old.usefulCount);
        committed.lateCount = std::max(committed.lateCount, old.lateCount);
        committed.demandHitCount = std::max(
            committed.demandHitCount, old.demandHitCount);
        if (old.demandHitCount > update.entry.demandHitCount) {
            committed.lastDemandPC = old.lastDemandPC;
            committed.lastDemandChainId = old.lastDemandChainId;
            committed.lastDemandSource = old.lastDemandSource;
        }
        for (const auto &oldSub : old.consumers) {
            if (!oldSub.valid)
                continue;
            const int col = findConsumer(committed, oldSub.consumerPC);
            if (col < 0)
                continue;
            auto &sub = committed.consumers[col];
            sub.candidateCount = std::max(
                sub.candidateCount, oldSub.candidateCount);
            sub.usefulCount = std::max(sub.usefulCount, oldSub.usefulCount);
            sub.lateCount = std::max(sub.lateCount, oldSub.lateCount);
            sub.demandHitCount = std::max(
                sub.demandHitCount, oldSub.demandHitCount);
        }
    }
    table[update.row] = committed;
    replacement.touch(update.row);
    ++version;
    stats.producerWrites++;
    DPRINTF(LLDPrefetcher, "s2 PCp=%#x PCc=%#x len=%u row=%u\n",
            next.producerPC, update.training.consumerPC,
            update.training.chain.length, update.row);
}

void
LLDPrefetcher::learningTick()
{
    // s2 write, s1 construct, s0 lookup: one accepted train per cycle.
    if (s1) {
        if (!s1->training.owner->squashed)
            commitUpdate(*s1);
        else
            stats.trainSquashed++;
        s1.reset();
    }
    if (s0) {
        if (!s0->owner->squashed)
            s1 = makeUpdate(*s0);
        else
            stats.trainSquashed++;
        s0.reset();
    }
    if (!s0 && !input.empty()) {
        s0 = input.front();
        input.pop_front();
        s0->producer = findProducer(s0->chain.producerPC, s0->context);
        s0->consumer = s0->producer < 0 ? -1 :
            findConsumer(table[s0->producer], s0->consumerPC);
        s0->version = version;
    }
    if (!input.empty() || s0 || s1)
        schedule(learningEvent, nextCycle());
}

lldp::Hint
LLDPrefetcher::pfHint(const PacketPtr &pkt)
{
    lldp::Hint hint;
    const int row = findProducer(pkt->req->getPC(), pkt->req->contextId());
    if (row < 0)
        return hint;
    stats.producerMatches++;
    replacement.touch(row);
    const auto &entry = table[row];
    if (entry.pConf < producerThreshold)
        return hint;
    auto meta = pkt->req->getXsMetadata().instXsMetadata;
    hint.valid = true;
    hint.producerPC = entry.producerPC;
    hint.generation = entry.generation;
    hint.offset = pkt->req->getPaddr() & (blkSize - 1);
    if (meta) {
        hint.size = meta->lldpSize;
        hint.signExtend = meta->lldpSigned;
    } else {
        const auto &pf_meta = pkt->req->getXsMetadata();
        hint.offset = pf_meta.prefetchDataOffset;
        hint.size = pf_meta.prefetchDataSize;
        hint.signExtend = pf_meta.prefetchDataSignExtend;
    }
    if (!hint.size || hint.offset + hint.size > blkSize ||
        (!pkt->req->isPrefetch() && pkt->req->getSize() != hint.size)) {
        hint.valid = false;
        stats.unsupported++;
    }
    return hint;
}

lldp::Hint
LLDPrefetcher::loadTrain(const PacketPtr &pkt, bool miss)
{
    ageMetaTable();
    const bool spatial_pf = isSpatialPrefetch(pkt);
    if (!pkt->isRead() || (!pkt->isDemand() && !spatial_pf) ||
        pkt->req->isInstFetch() || pkt->req->isUncacheable() ||
        pkt->req->isCacheMaintenance() ||
        (!pkt->req->hasVaddr() && !spatial_pf) ||
        !pkt->req->hasPC() || !pkt->req->hasContextId() ||
        !pkt->req->hasXsMetadata())
        return {};
    if (spatial_pf)
        stats.spatialLoadTrain++;
    const auto meta = pkt->req->getXsMetadata().instXsMetadata;
    if (!spatial_pf && (!meta || !meta->lldpLoad || meta->squashed))
        return {};
    // L1 learns at successful IQ issue. L2 sees only requests forwarded by
    // L1, and learns those at its own tag-result boundary (hit or miss).
    if (!spatial_pf && !trainingCPU)
        dependenceTrain(meta);
    auto hint = pfHint(pkt);
    if (hint.valid) {
        hint.spatial = spatial_pf;
        const Addr addr_p = blockAddress(pkt->req->getPaddr()) | hint.offset;
        const int producer = findProducer(
            hint.producerPC, pkt->req->contextId());
        if (producer < 0)
            return hint;
        uint8_t covered = 0;
        unsigned eligible = 0;
        for (unsigned col = 0; col < SubEntries; ++col) {
            const auto &sub = table[producer].consumers[col];
            if (!sub.valid || sub.cConf < consumerThreshold ||
                sub.immConf < immediateThreshold)
                continue;
            ++eligible;
            const auto meta_hit = lookupMetaTable(
                addr_p, hint.producerPC, sub.consumerPC,
                pkt->req->contextId());
            if (!meta_hit)
                continue;
            stats.metaTableHits++;
            if (queueCandidate(pkt, hint, addr_p, meta_hit->addrC,
                               PrefetchSourceType::LLDPT, col, meta_hit))
                covered |= uint8_t(1U << col);
        }
        hint.metaCoveredMask = covered;
        if (eligible > unsigned(__builtin_popcount(covered)))
            stats.metaFallbacks++;
        if (covered && unsigned(__builtin_popcount(covered)) >= eligible) {
            hint.valid = false;
            return hint;
        }
        if (!miss && covered) {
            hint.valid = false;
            return hint;
        }
        if (miss) {
            stats.hints++;
            if (spatial_pf)
                stats.spatialHints++;
        } else {
            stats.hitHintsDiscarded++;
            hint.valid = false;
        }
    }
    return hint;
}

bool
LLDPrefetcher::queueCandidate(const PacketPtr &demand,
                              const lldp::Hint &hint, Addr addr_p,
                              Addr addr_c, PrefetchSourceType source,
                              std::optional<unsigned> consumer,
                              std::optional<MetaHit> meta_hit)
{
    stats.candidates++;
    stats.candidateGenerated++;
    if (!admitPfControlCandidate(source))
        return false;
    const Addr origin_addr = demand->req->hasVaddr() ?
        demand->req->getVaddr() : demand->req->getPaddr();
    PrefetchInfo origin(demand, origin_addr, true,
                        Request::XsMetadata(source));
    PrefetchInfo candidate(origin, addr_c);
    candidateId = (candidateId + 1) & ((uint64_t(1) << 48) - 1);
    if (!candidateId)
        ++candidateId;
    const uint64_t id = (uint64_t(requestorId) << 48) | candidateId;
    auto metadata = Request::XsMetadata(
        source, 0, hint.producerPC, hint.generation, id);
    metadata.prefetchLldpAddrP = addr_p;
    if (consumer) {
        const int producer = findProducer(
            hint.producerPC, demand->req->contextId());
        if (producer >= 0)
            metadata.prefetchConsumerPC =
                table[producer].consumers[*consumer].consumerPC;
    } else if (meta_hit) {
        metadata.prefetchConsumerPC =
            metaTable[meta_hit->set][meta_hit->way].consumerPC;
    }
    candidate.setXsMetadata(metadata);

    AddrPriority command(addr_c, 0, source);
    command.isVA = source != PrefetchSourceType::LLDPT;
    command.forceTranslation = command.isVA;
    statsQueued.pfIdentified++;

    const int row = findProducer(
        hint.producerPC, demand->req->contextId());
    if (row >= 0 && table[row].generation == hint.generation) {
        table[row].candidateCount++;
        if (consumer && *consumer < SubEntries)
            table[row].consumers[*consumer].candidateCount++;
    }
    if (!insert(demand, candidate, command))
        return false;

    stats.candidateQueued++;
    if (source == PrefetchSourceType::LLDPT)
        stats.metaTablePrefetches++;
    if (source == PrefetchSourceType::LLDPT && meta_hit) {
        auto &entry = metaTable[meta_hit->set][meta_hit->way];
        if (entry.valid && entry.generation == meta_hit->generation) {
            entry.tokens = entry.tokens ? entry.tokens - 1 : 0;
            entry.outstanding++;
            entry.rrpv = 0;
        }
    }
    if (source == PrefetchSourceType::LLDPT && meta_hit) {
        candidateOwners[id] = {
            0, 0, hint.generation, metadata.prefetchConsumerPC, source,
            true, meta_hit->set, meta_hit->way, meta_hit->generation};
    } else if (row >= 0 && consumer && *consumer < SubEntries) {
        const auto &sub = table[row].consumers[*consumer];
        candidateOwners[id] = {
            unsigned(row), *consumer, hint.generation, sub.consumerPC,
            source, false, 0, 0, 0};
    }
    return true;
}

void
LLDPrefetcher::hintData(const lldp::Hint &hint, const PacketPtr &demand,
                       Addr addr_p, const uint8_t *data, unsigned size)
{
    if (!hint.valid || !data || hint.offset + hint.size > size)
        return;
    const auto meta = demand->req->hasXsMetadata() ?
        demand->req->getXsMetadata().instXsMetadata : nullptr;
    if (meta && meta->squashed)
        return;
    stats.returnedHints++;
    const int row = findProducer(hint.producerPC, demand->req->contextId());
    if (row < 0 || table[row].generation != hint.generation) {
        stats.staleHints++;
        return;
    }
    uint64_t value = 0;
    for (unsigned byte = 0; byte < hint.size; ++byte)
        value |= uint64_t(data[hint.offset + byte]) << (8 * byte);
    if (hint.signExtend && hint.size < 8 &&
        (value & (uint64_t(1) << (hint.size * 8 - 1))))
        value |= (~uint64_t(0)) << (hint.size * 8);
    std::set<Addr> generated;
    for (unsigned col = 0; col < SubEntries; ++col) {
        auto &sub = table[row].consumers[col];
        if (hint.metaCoveredMask & uint8_t(1U << col))
            continue;
        if (!sub.valid || sub.cConf < consumerThreshold || sub.immConf < immediateThreshold)
            continue;
        bool valid = sub.chain.trainable();
        uint64_t address = value;
        for (unsigned i = 0; valid && i + 1 < sub.chain.length; ++i)
            valid &= lldp::apply(sub.chain.ops[i], address);
        if (!valid) {
            stats.unsupported++;
            continue;
        }
        address = blockAddress(address + uint64_t(sub.loadImm));
        if (!generated.insert(address).second) {
            stats.duplicates++;
            continue;
        }
        const auto source = hint.spatial ? PrefetchSourceType::LLDPS :
            PrefetchSourceType::LLDP;
        queueCandidate(demand, hint, addr_p, address, source, col);
        DPRINTF(LLDPrefetcher, "prefetch PCp=%#x PCc=%#x value=%#x va=%#x offset=%u\n",
                hint.producerPC, sub.consumerPC, value, address, hint.offset);
    }
}

void
LLDPrefetcher::addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    Queued::addToQueue(queue, dpp);
    if (&queue == &pfq && !pfq.empty() && packetReady)
        packetReady(pfq.front().tick);
}

void
LLDPrefetcher::rxHint(BaseMMU::Translation *translation)
{
    // Upstream prefetch-ahead packets use the existing downstream interface.
    // Copy the descriptor as WorkerPrefetcher does; its packet is transferred.
    auto *incoming = static_cast<DeferredPacket *>(translation);
    DeferredPacket dpp = *incoming;
    dpp.owner = this;
    if (dpp.pkt && dpp.pkt->req->hasXsMetadata() &&
        dpp.pkt->req->getXsMetadata().prefetchCandidateId &&
        tlbFilterSet.count(blockAddress(dpp.pfInfo.getAddr()))) {
        stats.filtered++;
        prefetchDropped(dpp);
        delete dpp.pkt;
        return;
    }
    if (admitPfControlDeferredPacket(dpp))
        addToQueue(pfq, dpp);
    else {
        prefetchDropped(dpp);
        delete dpp.pkt;
    }
}

void
LLDPrefetcher::preDumpStats()
{
    Queued::preDumpStats();
    stats.validProducers = 0;
    stats.samplerValidEntries = 0;
    stats.metaValidEntries = 0;
    for (const auto &set : samplerTable)
        for (const auto &entry : set)
            stats.samplerValidEntries += entry.valid;
    for (const auto &set : metaTable)
        for (const auto &entry : set)
            stats.metaValidEntries += entry.valid;
    for (unsigned i = 0; i <= SubEntries; ++i)
        stats.childrenAtDump[i] = 0;
    for (unsigned i = 0; i <= SubEntries; ++i)
        stats.pcpActiveHistogram[i] = 0;
    for (unsigned row = 0; row < table.size(); ++row) {
        const auto &entry = table[row];
        stats.pcpProducerPC[row] = entry.producerPC;
        stats.pcpContext[row] = entry.context;
        stats.pcpValid[row] = entry.valid;
        stats.pcpConf[row] = entry.pConf;
        stats.pcpDemandPC[row] = entry.lastDemandPC;
        stats.pcpDemandChainId[row] = entry.lastDemandChainId;
        stats.pcpDemandSource[row] = unsigned(entry.lastDemandSource);
        stats.pcpDemandHits[row] = entry.demandHitCount;
        stats.pcpActivePeak[row] = entry.activePeak;
        stats.pcpConsumerReplacement[row] = entry.consumerReplacement;
        stats.pcpConsumerOverflow[row] = entry.consumerOverflow;
        stats.pcpCandidate[row] = entry.candidateCount;
        stats.pcpUseful[row] = entry.usefulCount;
        stats.pcpLate[row] = entry.lateCount;
        for (unsigned col = 0; col < SubEntries; ++col) {
            const auto &consumer = entry.consumers[col];
            const unsigned idx = row * SubEntries + col;
            stats.consumerValid[idx] = consumer.valid;
            stats.consumerPC[idx] = consumer.consumerPC;
            stats.consumerLength[idx] = consumer.chain.length;
            stats.consumerOp1[idx] = unsigned(consumer.chain.ops[0].op);
            stats.consumerOp2[idx] = unsigned(consumer.chain.ops[1].op);
            stats.consumerImmLoad[idx] = consumer.loadImm;
            stats.consumerImmConf[idx] = consumer.immConf;
            stats.consumerLineConf[idx] = consumer.lineConf;
            stats.consumerOffsetConf[idx] = consumer.offsetConf;
            stats.consumerConf[idx] = consumer.cConf;
            stats.consumerUpdates[idx] = consumer.updateCount;
            stats.consumerReplacements[idx] = consumer.replacementCount;
            stats.consumerExactImmStable[idx] =
                consumer.exactImmStableCount;
            stats.consumerLineImmStable[idx] =
                consumer.lineImmStableCount;
            stats.consumerCandidates[idx] = consumer.candidateCount;
            stats.consumerUseful[idx] = consumer.usefulCount;
            stats.consumerLate[idx] = consumer.lateCount;
            stats.consumerDemandHits[idx] = consumer.demandHitCount;
        }
        if (entry.valid) {
            stats.validProducers++;
            stats.childrenAtDump[validChildren(entry)]++;
            stats.pcpActiveHistogram[validChildren(entry)]++;
        }
    }
}

void
LLDPrefetcher::notifyPrefetchUseful(PrefetchSourceType source)
{
    Queued::notifyPrefetchUseful(source);
    if (isLldpSource(source))
        stats.candidateUseful++;
}

void
LLDPrefetcher::notifyPrefetchUseful(PrefetchSourceType source,
                                    uint64_t candidate_id)
{
    Queued::notifyPrefetchUseful(source);
    if (isLldpSource(source) && candidate_id) {
        stats.candidateUseful++;
        updateMetaOwner(candidate_id, 0);
        const auto it = candidateOwners.find(candidate_id);
        if (it != candidateOwners.end() && !it->second.meta &&
            table[it->second.row].generation == it->second.generation &&
            table[it->second.row].consumers[it->second.col].consumerPC ==
                it->second.consumerPC) {
            table[it->second.row].usefulCount++;
            table[it->second.row].consumers[it->second.col].usefulCount++;
        }
        candidateOwners.erase(candidate_id);
    }
}

void
LLDPrefetcher::prefetchUnused(PrefetchSourceType source)
{
    Queued::prefetchUnused(source);
    if (isLldpSource(source))
        stats.candidateUnused++;
}

void
LLDPrefetcher::prefetchUnused(PrefetchSourceType source,
                              uint64_t candidate_id)
{
    Queued::prefetchUnused(source);
    if (isLldpSource(source) && candidate_id) {
        stats.candidateUnused++;
        updateMetaOwner(candidate_id, 3);
        candidateOwners.erase(candidate_id);
    }
}

void
LLDPrefetcher::prefetchUnused(Addr paddr, PrefetchSourceType source,
                              uint64_t candidate_id)
{
    prefetchUnused(source, candidate_id);
}

void
LLDPrefetcher::notifyPrefetchMerged(uint64_t candidate_id)
{
    if (!candidate_id)
        return;
    stats.candidateMerged++;
    updateMetaOwner(candidate_id, 1);
    candidateOwners.erase(candidate_id);
}

void
LLDPrefetcher::notifyCandidateDemand(uint64_t candidate_id,
                                     const PacketPtr &demand)
{
    if (!candidate_id || !demand || !demand->req)
        return;
    const auto it = candidateOwners.find(candidate_id);
    if (it == candidateOwners.end() || it->second.meta ||
        table[it->second.row].generation != it->second.generation ||
        table[it->second.row].consumers[it->second.col].consumerPC !=
            it->second.consumerPC)
        return;
    auto &entry = table[it->second.row];
    entry.lastDemandPC = demand->req->hasPC() ? demand->req->getPC() : 0;
    entry.lastDemandChainId = it->second.generation;
    entry.lastDemandSource = it->second.source;
    entry.demandHitCount++;
    entry.consumers[it->second.col].demandHitCount++;
}

void
LLDPrefetcher::pfHitInCache(PrefetchSourceType source)
{
    Queued::pfHitInCache(source);
    if (isLldpSource(source)) {
        stats.candidateCacheHit++;
        stats.candidateLate++;
    }
}

void
LLDPrefetcher::pfHitInCache(PrefetchSourceType source,
                            uint64_t candidate_id)
{
    Queued::pfHitInCache(source);
    if (isLldpSource(source)) {
        stats.candidateCacheHit++;
        stats.candidateLate++;
    }
    if (isLldpSource(source) && candidate_id) {
        updateMetaOwner(candidate_id, 2);
        const auto it = candidateOwners.find(candidate_id);
        if (it != candidateOwners.end() && !it->second.meta &&
            table[it->second.row].generation == it->second.generation &&
            table[it->second.row].consumers[it->second.col].consumerPC ==
                it->second.consumerPC) {
            table[it->second.row].lateCount++;
            table[it->second.row].consumers[it->second.col].lateCount++;
        }
        candidateOwners.erase(candidate_id);
    }
}

void
LLDPrefetcher::pfHitInMSHR(PrefetchSourceType source)
{
    Queued::pfHitInMSHR(source);
    if (isLldpSource(source)) {
        stats.candidateMshrHit++;
        stats.candidateLate++;
    }
}

void
LLDPrefetcher::pfHitInMSHR(PrefetchSourceType source,
                           uint64_t candidate_id)
{
    Queued::pfHitInMSHR(source);
    if (isLldpSource(source)) {
        stats.candidateMshrHit++;
        stats.candidateLate++;
    }
    if (isLldpSource(source) && candidate_id) {
        updateMetaOwner(candidate_id, 2);
        const auto it = candidateOwners.find(candidate_id);
        if (it != candidateOwners.end() && !it->second.meta &&
            table[it->second.row].generation == it->second.generation &&
            table[it->second.row].consumers[it->second.col].consumerPC ==
                it->second.consumerPC) {
            table[it->second.row].lateCount++;
            table[it->second.row].consumers[it->second.col].lateCount++;
        }
        candidateOwners.erase(candidate_id);
    }
}

void
LLDPrefetcher::pfHitInWB(PrefetchSourceType source)
{
    Queued::pfHitInWB(source);
    if (isLldpSource(source)) {
        stats.candidateWbHit++;
        stats.candidateLate++;
    }
}

void
LLDPrefetcher::pfHitInWB(PrefetchSourceType source,
                         uint64_t candidate_id)
{
    Queued::pfHitInWB(source);
    if (isLldpSource(source)) {
        stats.candidateWbHit++;
        stats.candidateLate++;
    }
    if (isLldpSource(source) && candidate_id) {
        updateMetaOwner(candidate_id, 2);
        const auto it = candidateOwners.find(candidate_id);
        if (it != candidateOwners.end() && !it->second.meta &&
            table[it->second.row].generation == it->second.generation &&
            table[it->second.row].consumers[it->second.col].consumerPC ==
                it->second.consumerPC) {
            table[it->second.row].lateCount++;
            table[it->second.row].consumers[it->second.col].lateCount++;
        }
        candidateOwners.erase(candidate_id);
    }
}

void
LLDPrefetcher::recordIssuedPrefetchStats(const PacketPtr &pkt)
{
    Base::recordIssuedPrefetchStats(pkt);
    if (pkt && pkt->req && pkt->req->hasXsMetadata() &&
        pkt->req->getXsMetadata().prefetchCandidateId)
        stats.candidateIssued++;
}
} // namespace prefetch
} // namespace gem5

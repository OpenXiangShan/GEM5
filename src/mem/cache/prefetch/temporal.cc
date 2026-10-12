#include "mem/cache/prefetch/temporal.hh"

#include "cpu/base.hh"
#include "debug/TemporalPrefetcher.hh"
#include "params/TemporalPrefetcher.hh"

namespace gem5::prefetch
{
TemporalPrefetcher::TPStats::TPStats(statistics::Group *parent)
    : statistics::Group(parent, "tp"),
      ADD_STAT(trains, statistics::units::Count::get(), "Demand load observations"),
      ADD_STAT(filterHits, statistics::units::Count::get(), "Filter PC hits"),
      ADD_STAT(samplerHits, statistics::units::Count::get(), "Sampler base address hits"),
      ADD_STAT(pairs, statistics::units::Count::get(), "Pairs sent to Recorder"),
      ADD_STAT(records, statistics::units::Count::get(), "Records installed in Meta/Data"),
      ADD_STAT(recorderReplacements, statistics::units::Count::get(), "Partial records emitted on replacement"),
      ADD_STAT(metaHits, statistics::units::Count::get(), "Trigger hits in Meta/Data"),
      ADD_STAT(metaBlocked, statistics::units::Count::get(), "Confidence blocked record installs"),
      ADD_STAT(issueBlocked, statistics::units::Count::get(), "Confidence blocked groups"),
      ADD_STAT(issueProbes, statistics::units::Count::get(), "Sparse group probes"),
      ADD_STAT(metaProbes, statistics::units::Count::get(), "Sparse install probes"),
      ADD_STAT(candidates, statistics::units::Count::get(), "TP replay candidates"),
      ADD_STAT(queued, statistics::units::Count::get(), "TP accepted queue entries"),
      ADD_STAT(useful, statistics::units::Count::get(), "TP useful feedback"),
      ADD_STAT(late, statistics::units::Count::get(), "TP late feedback"),
      ADD_STAT(dropped, statistics::units::Count::get(), "TP queue drops"),
      ADD_STAT(ownerExpired, statistics::units::Count::get(), "Bounded feedback owners evicted"),
      ADD_STAT(trainRoles, statistics::units::Count::get(), "LLDP roles on training PCs"),
      ADD_STAT(recordRoles, statistics::units::Count::get(), "LLDP roles on replay records")
{
    for (auto *vector : {&trainRoles, &recordRoles}) {
        vector->init(4);
        vector->subname(0, "ordinary");
        vector->subname(1, "producer");
        vector->subname(2, "consumer");
        vector->subname(3, "both");
    }
}

TemporalPrefetcher::TemporalPrefetcher(const TemporalPrefetcherParams &p)
    : Queued(p), engine(p.record_length), trainingCPU(p.training_cpu),
      throttleCycles(p.throttle_cycles), stats(this)
{
    fatal_if(p.record_length < 9 || p.record_length > 16 || !throttleCycles,
             "TP requires record_length in [9,16] and a positive throttle");
}

void
TemporalPrefetcher::DependenceListener::notify(const o3::XsDynInstMetaPtr &meta)
{
    if (!meta || meta->squashed || !meta->lldpLoad || !meta->lldpChain.valid ||
        meta->lldpContext == InvalidContextID)
        return;
    parent.engine.mark(meta->lldpChain.producerPC, meta->lldpContext, 1);
    parent.engine.mark(meta->instAddr, meta->lldpContext, 2);
}

void
TemporalPrefetcher::regProbeListeners()
{
    Queued::regProbeListeners();
    if (trainingCPU && !dependenceListener)
        dependenceListener = trainingCPU->getProbeManager()->connect<DependenceListener>(*this);
}

lldp::Hint
TemporalPrefetcher::loadTrain(const PacketPtr &pkt, bool miss)
{
    if (!pkt->isRead() || !pkt->isDemand() || pkt->req->isPrefetch() ||
        pkt->req->isInstFetch() || pkt->req->isUncacheable() ||
        pkt->req->isCacheMaintenance() || !pkt->req->hasPC() ||
        !pkt->req->hasContextId())
        return {};
    if (pkt->req->hasXsMetadata()) {
        const auto owner = pkt->req->getXsMetadata().instXsMetadata;
        if (owner) {
            if (owner->squashed)
                return {};
            const auto identity = std::make_pair(pkt->req->contextId(), owner->seqNum);
            if (std::find(observedLoads.begin(), observedLoads.end(), identity) != observedLoads.end())
                return {};
            observedLoads.push_back(identity);
            if (observedLoads.size() > 1024)
                observedLoads.pop_front();
        }
    }
    const Addr pc = pkt->req->getPC();
    const ContextID context = pkt->req->contextId();
    stats.trainRoles[engine.roles(pc, context)]++;
    const Addr line = pkt->req->getPaddr() / blkSize;
    const auto record = engine.observe(pc, context, line);
    if (!record)
        return {};
    DPRINTF(TemporalPrefetcher, "replay pc=%#x trigger=%#x length=%u roles=%u\n",
            pc, line * blkSize, record->lines.size(), record->roles);
    stats.recordRoles[record->roles]++;
    unsigned accepted = 0;
    for (const auto target : record->lines) {
        if (target == line)
            continue;
        stats.candidates++;
        PrefetchInfo origin(pkt, pkt->req->getPaddr(), miss,
                            Request::XsMetadata(PrefetchSourceType::TP));
        PrefetchInfo candidate(origin, target * blkSize);
        candidateId = (candidateId + 1) & ((1ULL << 48) - 1);
        if (!candidateId)
            ++candidateId;
        const uint64_t id = (uint64_t(requestorId) << 48) | candidateId;
        auto metadata = Request::XsMetadata(PrefetchSourceType::TP, 0, pc, 0, id);
        candidate.setXsMetadata(metadata);
        AddrPriority command(target * blkSize, 0, PrefetchSourceType::TP);
        command.isVA = false;
        statsQueued.pfIdentified++;
        if (admitPfControlCandidate(PrefetchSourceType::TP) &&
            insert(pkt, candidate, command)) {
            owners[id] = {pc, context};
            ownerOrder.push_back(id);
            if (ownerOrder.size() > 4096) {
                stats.ownerExpired += owners.erase(ownerOrder.front());
                ownerOrder.pop_front();
            }
            ++accepted;
            stats.queued++;
        }
    }
    if (accepted)
        engine.issued(pc, context, accepted);
    return {};
}

void
TemporalPrefetcher::addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    dpp.tick = std::max(dpp.tick, nextSend);
    nextSend = dpp.tick + throttleCycles * clockPeriod();
    Queued::addToQueue(queue, dpp);
    if (packetReady)
        packetReady(dpp.tick);
}

Tick
TemporalPrefetcher::nextPrefetchReadyTime() const
{
    return std::max(Queued::nextPrefetchReadyTime(), nextIssue);
}

bool
TemporalPrefetcher::hasPendingPacket()
{
    return nextPrefetchReadyTime() <= curTick();
}

PacketPtr
TemporalPrefetcher::getPacket()
{
    if (!hasPendingPacket())
        return nullptr;
    PacketPtr pkt = Queued::getPacket();
    if (pkt) {
        nextIssue = curTick() + throttleCycles * clockPeriod();
        DPRINTF(TemporalPrefetcher, "issue pc=%#x pa=%#x id=%llu\n",
                pkt->req->getPC(), pkt->getAddr(),
                pkt->req->getXsMetadata().prefetchCandidateId);
        if (!pfq.empty() && packetReady)
            packetReady(nextPrefetchReadyTime());
    }
    return pkt;
}

void
TemporalPrefetcher::prefetchDropped(const DeferredPacket &dpp)
{
    owners.erase(dpp.pfInfo.getXsMetadata().prefetchCandidateId);
    stats.dropped++;
}

void
TemporalPrefetcher::feedback(uint64_t id, bool late)
{
    const auto it = owners.find(id);
    if (it == owners.end())
        return;
    engine.useful(it->second.pc, it->second.context, late);
    if (late)
        stats.late++;
    else
        stats.useful++;
    owners.erase(it);
}

void
TemporalPrefetcher::notifyPrefetchUseful(PrefetchSourceType source, uint64_t id)
{
    Queued::notifyPrefetchUseful(source);
    if (source == PrefetchSourceType::TP)
        feedback(id, false);
}
void
TemporalPrefetcher::pfHitInCache(PrefetchSourceType source, uint64_t id)
{
    Queued::pfHitInCache(source, id);
    if (source == PrefetchSourceType::TP)
        owners.erase(id);
}
void
TemporalPrefetcher::pfHitInWB(PrefetchSourceType source, uint64_t id)
{
    Queued::pfHitInWB(source, id);
    if (source == PrefetchSourceType::TP)
        owners.erase(id);
}
void
TemporalPrefetcher::pfHitInMSHR(PrefetchSourceType source, uint64_t id)
{
    Queued::pfHitInMSHR(source, id);
    if (source == PrefetchSourceType::TP)
        feedback(id, true);
}
void
TemporalPrefetcher::prefetchUnused(PrefetchSourceType source, uint64_t id)
{
    Queued::prefetchUnused(source);
    if (source == PrefetchSourceType::TP)
        owners.erase(id); // confidence already paid the issue penalty
}
void
TemporalPrefetcher::prefetchUnused(Addr, PrefetchSourceType source, uint64_t id)
{
    prefetchUnused(source, id);
}
void
TemporalPrefetcher::notifyPrefetchMerged(uint64_t id)
{
    feedback(id, true);
}

void
TemporalPrefetcher::preDumpStats()
{
    Queued::preDumpStats();
    const auto &c = engine.counters;
    stats.trains = c.trains;
    stats.filterHits = c.filterHits;
    stats.samplerHits = c.samplerHits;
    stats.pairs = c.pairs;
    stats.records = c.records;
    stats.recorderReplacements = c.replacements;
    stats.metaHits = c.metaHits;
    stats.metaBlocked = c.metaBlocked;
    stats.issueBlocked = c.issueBlocked;
    stats.issueProbes = c.issueProbes;
    stats.metaProbes = c.metaProbes;
}
} // namespace gem5::prefetch

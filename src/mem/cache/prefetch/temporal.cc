#include "mem/cache/prefetch/temporal.hh"

#include "cpu/base.hh"
#include "debug/TemporalPrefetcher.hh"
#include "params/TemporalPrefetcher.hh"

namespace gem5::prefetch
{
TemporalPrefetcher::TPStats::TPStats(statistics::Group *parent, unsigned metaSetsCount)
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
      ADD_STAT(recordRoles, statistics::units::Count::get(), "LLDP roles on replay records"),
      ADD_STAT(roleSets, statistics::units::Count::get(), "Role table set access/eviction/occupancy"),
      ADD_STAT(filterSets, statistics::units::Count::get(), "Filter set access/eviction/occupancy"),
      ADD_STAT(samplerSets, statistics::units::Count::get(), "Sampler set access/eviction/occupancy"),
      ADD_STAT(recorderSets, statistics::units::Count::get(), "Recorder set access/eviction/occupancy"),
      ADD_STAT(metaSets, statistics::units::Count::get(), "Meta set access/eviction/occupancy"),
      ADD_STAT(confidenceSets, statistics::units::Count::get(),
               "Confidence set access/eviction/occupancy"),
      ADD_STAT(feedbackEvents, statistics::units::Count::get(),
               "TP candidate lifecycle events"),
      ADD_STAT(previousWindowFeedback, statistics::units::Count::get(),
               "Lifecycle events for earlier-window candidates"),
      ADD_STAT(recordControl, statistics::units::Count::get(),
               "TP record feedback control events"),
      ADD_STAT(trainingActivity, statistics::units::Count::get(),
               "Raw observations and consecutive-line training activity"),
      ADD_STAT(compressionPotential, statistics::units::Count::get(),
               "Accepted records with lossless signed 16-bit relative lines"),
      ADD_STAT(compressionStorage, statistics::units::Count::get(),
               "Compact/full record pool allocations and resident records"),
      ADD_STAT(onlineRoleKinds, statistics::units::Count::get(),
               "Role observations by integer/FP producer and consumer"),
      ADD_STAT(replacementControl, statistics::units::Count::get(),
               "RRIP aging and first-use promotion"),
      ADD_STAT(trainingKinds, statistics::units::Count::get(),
               "Training observations by scalar type and LLDP roles"),
      ADD_STAT(lifecycleKinds, statistics::units::Count::get(),
               "Queued/sent/timely/unused candidates by trigger type/role"),
      ADD_STAT(storageKinds, statistics::units::Count::get(),
               "Installed/evicted records by scalar type/role")
{
    const std::array<const char *, unsigned(FeedbackEvent::Count)> events = {
        "queued", "sent", "timely", "notReady", "permissionMiss",
        "lateDemand", "unused", "cacheDuplicate", "mshrDuplicate",
        "wbDuplicate", "dropped", "expired", "staleRecord",
        "duplicateTerminal", "unknown", "generationExpired", "unownedSent",
        "readTimely", "otherTimely"};
    feedbackEvents.init(events.size());
    previousWindowFeedback.init(events.size());
    for (unsigned i = 0; i < events.size(); ++i) {
        feedbackEvents.subname(i, events[i]);
        previousWindowFeedback.subname(i, events[i]);
    }
    const std::array<const char *, 6> controls = {
        "blocked", "probes", "sameContentRefresh", "staleFeedback",
        "generationWrap", "sentCost"};
    recordControl.init(controls.size());
    for (unsigned i = 0; i < controls.size(); ++i)
        recordControl.subname(i, controls[i]);
    const std::array<const char *, 6> activity = {
        "sameLine", "coalesced", "probes", "crossLine",
        "crossLineTrained", "samplerWrites"};
    trainingActivity.init(activity.size());
    for (unsigned i = 0; i < activity.size(); ++i)
        trainingActivity.subname(i, activity[i]);
    const std::array<const char *, 8> poolMetrics = {
        "compactWrites", "fullWrites", "fallback", "failure", "formatSwitch",
        "compactLive", "fullLive", "hostPayloadBytes"};
    compressionStorage.init(poolMetrics.size());
    for (unsigned i = 0; i < poolMetrics.size(); ++i)
        compressionStorage.subname(i, poolMetrics[i]);
    compressionPotential.init(3);
    compressionPotential.subname(0, "encodableRecords");
    compressionPotential.subname(1, "installedLines");
    compressionPotential.subname(2, "encodableLines");
    onlineRoleKinds.init(4);
    for (unsigned i = 0; i < 4; ++i)
        onlineRoleKinds.subname(i, std::to_string(i));
    replacementControl.init(3);
    replacementControl.subname(0, "roleProbe");
    replacementControl.subname(1, "rripAge");
    replacementControl.subname(2, "timelyPromotion");
    const std::array<const char *, 3> kinds = {"integer", "fp", "unknown"};
    const std::array<const char *, 4> roles = {
        "ordinary", "producer", "consumer", "both"};
    const std::array<const char *, 4> lifecycle = {
        "queued", "sent", "timely", "unused"};
    const std::array<const char *, 3> storage = {"install", "evict", "probe"};
    trainingKinds.init(12);
    lifecycleKinds.init(48);
    storageKinds.init(36);
    for (unsigned kind = 0; kind < 3; ++kind)
        for (unsigned role = 0; role < 4; ++role) {
            const unsigned index = kind * 4 + role;
            const auto label = std::string(kinds[kind]) + "_" + roles[role];
            trainingKinds.subname(index, label);
            for (unsigned event = 0; event < lifecycle.size(); ++event)
                lifecycleKinds.subname(event * 12 + index,
                    std::string(lifecycle[event]) + "_" + label);
            for (unsigned event = 0; event < storage.size(); ++event)
                storageKinds.subname(event * 12 + index,
                    std::string(storage[event]) + "_" + label);
        }
    const std::array<statistics::Vector *, 6> tables = {
        &roleSets, &filterSets, &samplerSets, &recorderSets,
        &metaSets, &confidenceSets};
    const std::array<unsigned, 6> sets = {256, 32, 1024, 128, metaSetsCount, 64};
    const std::array<const char *, 3> metrics = {
        "access", "eviction", "occupancy"};
    for (unsigned table = 0; table < tables.size(); ++table) {
        tables[table]->init(sets[table] * metrics.size());
        for (unsigned metric = 0; metric < metrics.size(); ++metric)
            for (unsigned set = 0; set < sets[table]; ++set)
                tables[table]->subname(metric * sets[table] + set,
                    std::string(metrics[metric]) + "_" + std::to_string(set));
    }
    for (auto *vector : {&trainRoles, &recordRoles}) {
        vector->init(4);
        vector->subname(0, "ordinary");
        vector->subname(1, "producer");
        vector->subname(2, "consumer");
        vector->subname(3, "both");
    }
}

TemporalPrefetcher::TemporalPrefetcher(const TemporalPrefetcherParams &p)
    : Queued(p), engine(p.record_length),
      trainingCPU(p.training_cpu), throttleCycles(p.throttle_cycles),
      stats(this, temporal::Engine::META_SETS)
{
    fatal_if(p.record_length < 9 || p.record_length > 16 || !throttleCycles,
             "TP requires record_length in [9,16] and a positive throttle");
    engine.setInvalidationHandler([this](unsigned slot) {
        invalidateRecord(slot);
    });
}

bool
TemporalPrefetcher::owns(uint64_t id) const
{
    return id && (id >> 48) == requestorId;
}

void
TemporalPrefetcher::event(FeedbackEvent kind, const Owner *owner)
{
    stats.feedbackEvents[unsigned(kind)]++;
    if (owner && owner->window != feedbackWindow)
        stats.previousWindowFeedback[unsigned(kind)]++;
    if (owner) {
        const int index = kind == FeedbackEvent::Queued ? 0 :
            kind == FeedbackEvent::Sent ? 1 :
            kind == FeedbackEvent::Timely ? 2 :
            kind == FeedbackEvent::Unused ? 3 : -1;
        if (index >= 0)
            stats.lifecycleKinds[index * 12 + owner->kindRole]++;
    }
}

void
TemporalPrefetcher::invalidateRecord(unsigned slot)
{
    for (auto &[id, owner] : owners) {
        if (owner.record.slot() == slot && !owner.record.settled()) {
            owner.record.settle();
            event(FeedbackEvent::GenerationExpired, &owner);
        }
    }
}

void
TemporalPrefetcher::finish(uint64_t id, FeedbackEvent kind,
    std::optional<temporal::FeedbackKind> reward)
{
    if (!owns(id))
        return;
    const auto it = owners.find(id);
    if (it == owners.end()) {
        event(FeedbackEvent::Unknown);
        return;
    }
    auto &owner = it->second;
    if (owner.record.settled()) {
        event(FeedbackEvent::DuplicateTerminal, &owner);
        return;
    }
    event(kind, &owner);
    if (reward) {
        if (*reward == temporal::FeedbackKind::Timely ||
            *reward == temporal::FeedbackKind::PermissionMiss)
            stats.useful++;
        else if (*reward == temporal::FeedbackKind::Late)
            stats.late++;
        if (!engine.feedback(owner.record, *reward)) {
            event(FeedbackEvent::StaleRecord, &owner);
        }
    }
    // Preserve the v3.1 PC credit, independently of record replacement.
    if (kind == FeedbackEvent::Timely || kind == FeedbackEvent::NotReady ||
        kind == FeedbackEvent::PermissionMiss)
        engine.useful(owner.pc, owner.context, false);
    else if (kind == FeedbackEvent::LateDemand ||
             kind == FeedbackEvent::MshrDuplicate) {
        engine.useful(owner.pc, owner.context, true);
        if (kind == FeedbackEvent::MshrDuplicate)
            stats.late++;
    }
    // A demand may merge after MSHR allocation but before downstream send.
    // Keep the sent bit independently so that send is still counted once.
    owner.record.settle();
}

void
TemporalPrefetcher::DependenceListener::notify(const o3::XsDynInstMetaPtr &meta)
{
    if (!meta || meta->squashed ||
        !meta->lldpLoad ||
        !meta->lldpChain.valid ||
        meta->lldpContext == InvalidContextID)
        return;
    const unsigned kind = unsigned(meta->lldpChain.producerFloating) * 2 +
        !meta->lldpLoad;
    parent.stats.onlineRoleKinds[kind]++;
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
    const auto inst = pkt->req->hasXsMetadata() ?
        pkt->req->getXsMetadata().instXsMetadata : nullptr;
    const uint8_t kind = inst && inst->lldpLoad ? 0 :
        inst && inst->lldpDirectLoad ? 1 : 2;
    const auto role = engine.roles(pc, context);
    stats.trainRoles[role]++;
    stats.trainingKinds[kind * 4 + role]++;
    const Addr line = pkt->req->getPaddr() / blkSize;
    const auto record = engine.observe(pc, context, line, kind);
    if (!record)
        return {};
    replay(pkt, pc, context, line, kind, *record, miss);
    return {};
}

unsigned
TemporalPrefetcher::replay(const PacketPtr &pkt, Addr pc, ContextID context,
    Addr line, uint8_t kind, const temporal::Replay &record, bool miss)
{
    DPRINTF(TemporalPrefetcher, "replay pc=%#x trigger=%#x length=%u roles=%u\n",
            pc, line * blkSize, record.lines.size(), record.roles);
    stats.recordRoles[record.roles]++;
    unsigned accepted = 0;
    for (const auto target : record.lines) {
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
            owners[id] = {pc, context, record.reference, feedbackWindow,
                          uint8_t(kind * 4 + record.roles)};
            ownerOrder.push_back(id);
            event(FeedbackEvent::Queued, &owners.at(id));
            if (ownerOrder.size() > 4096) {
                const auto old = owners.find(ownerOrder.front());
                if (old != owners.end()) {
                    if (!old->second.record.settled()) {
                        stats.ownerExpired++;
                        event(FeedbackEvent::Expired, &old->second);
                    }
                    owners.erase(old);
                }
                ownerOrder.pop_front();
            }
            ++accepted;
            stats.queued++;
        }
    }
    if (accepted)
        engine.issued(pc, context, accepted);
    return accepted;
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
    const auto id = dpp.pfInfo.getXsMetadata().prefetchCandidateId;
    finish(id, FeedbackEvent::Dropped);
    stats.dropped++;
}

void
TemporalPrefetcher::notifyPrefetchUseful(PrefetchSourceType source, uint64_t id)
{
    Queued::notifyPrefetchUseful(source);
}

void
TemporalPrefetcher::notifyPrefetchDemandHit(PrefetchSourceType source,
    uint64_t id, const PacketPtr &demand, bool ready, bool miss)
{
    Queued::notifyPrefetchUseful(source);
    if (source != PrefetchSourceType::TP)
        return;
    const auto it = owners.find(id);
    const bool first = it != owners.end() && !it->second.record.settled();
    if (first && ready)
        event(demand->isRead() ? FeedbackEvent::ReadTimely :
              FeedbackEvent::OtherTimely, &it->second);
    finish(id, ready ? FeedbackEvent::Timely : miss ?
        FeedbackEvent::PermissionMiss : FeedbackEvent::NotReady,
        ready ? temporal::FeedbackKind::Timely :
                temporal::FeedbackKind::PermissionMiss);
}

void
TemporalPrefetcher::notifyPrefetchSent(const PacketPtr &pkt)
{
    if (!pkt->req->hasXsMetadata())
        return;
    const auto &metadata = pkt->req->getXsMetadata();
    const auto id = metadata.prefetchCandidateId;
    if (metadata.prefetchSource != PrefetchSourceType::TP || !owns(id))
        return;
    const auto it = owners.find(id);
    if (it == owners.end()) {
        event(FeedbackEvent::UnownedSent);
        return;
    }
    auto &owner = it->second;
    if (owner.record.sent())
        return;
    event(FeedbackEvent::Sent, &owner);
    owner.record.markSent();
}
void
TemporalPrefetcher::pfHitInCache(PrefetchSourceType source, uint64_t id)
{
    Queued::pfHitInCache(source, id);
    if (source == PrefetchSourceType::TP)
        finish(id, FeedbackEvent::CacheDuplicate);
}
void
TemporalPrefetcher::pfHitInWB(PrefetchSourceType source, uint64_t id)
{
    Queued::pfHitInWB(source, id);
    if (source == PrefetchSourceType::TP)
        finish(id, FeedbackEvent::WbDuplicate);
}
void
TemporalPrefetcher::pfHitInMSHR(PrefetchSourceType source, uint64_t id)
{
    Queued::pfHitInMSHR(source, id);
    if (source == PrefetchSourceType::TP)
        finish(id, FeedbackEvent::MshrDuplicate);
}
void
TemporalPrefetcher::prefetchUnused(PrefetchSourceType source, uint64_t id)
{
    Queued::prefetchUnused(source);
    if (source == PrefetchSourceType::TP)
        finish(id, FeedbackEvent::Unused, temporal::FeedbackKind::Unused);
}
void
TemporalPrefetcher::prefetchUnused(Addr, PrefetchSourceType source, uint64_t id)
{
    prefetchUnused(source, id);
}
void
TemporalPrefetcher::notifyPrefetchMerged(uint64_t id)
{
    finish(id, FeedbackEvent::LateDemand, temporal::FeedbackKind::Late);
}

void
TemporalPrefetcher::resetStats()
{
    Queued::resetStats();
    ++feedbackWindow;
    // Reset the reporting baseline, preserving the learned state and the
    // engine's lifetime counters (which also drive its aging epochs).
    lastDumpCounters = engine.counters;
    const auto &pool = engine.recordPools();
    lastPoolCounters = {pool.compactWrites, pool.fullWrites, pool.fallbacks,
                        pool.failures, pool.formatSwitches};
    engine.visitTables([](unsigned, auto &table) {
        table.reportedAccesses = table.setAccesses;
        table.reportedEvictions = table.setEvictions;
    });
}

void
TemporalPrefetcher::preDumpStats()
{
    Queued::preDumpStats();
    const auto &c = engine.counters;
    const auto &pool = engine.recordPools();
    const std::array<uint64_t, 5> poolCounters = {
        pool.compactWrites, pool.fullWrites, pool.fallbacks,
        pool.failures, pool.formatSwitches};
    for (unsigned i = 0; i < poolCounters.size(); ++i)
        stats.compressionStorage[i] += poolCounters[i] - lastPoolCounters[i];
    lastPoolCounters = poolCounters;
    stats.compressionStorage[5] = pool.compactOccupancy();
    stats.compressionStorage[6] = pool.fullOccupancy();
    stats.compressionStorage[7] = pool.allocatedBytes();
    // Engine counters survive a gem5 statistics reset. Publish increments
    // since the preceding dump or reset, including resets without a dump.
    stats.trains += c.trains - lastDumpCounters.trains;
    stats.filterHits += c.filterHits - lastDumpCounters.filterHits;
    stats.samplerHits += c.samplerHits - lastDumpCounters.samplerHits;
    stats.pairs += c.pairs - lastDumpCounters.pairs;
    stats.records += c.records - lastDumpCounters.records;
    stats.recorderReplacements += c.replacements - lastDumpCounters.replacements;
    stats.metaHits += c.metaHits - lastDumpCounters.metaHits;
    stats.metaBlocked += c.metaBlocked - lastDumpCounters.metaBlocked;
    stats.issueBlocked += c.issueBlocked - lastDumpCounters.issueBlocked;
    stats.issueProbes += c.issueProbes - lastDumpCounters.issueProbes;
    stats.metaProbes += c.metaProbes - lastDumpCounters.metaProbes;
    const std::array<uint64_t, 6> controls = {
        0, 0,
        c.recordRefreshes - lastDumpCounters.recordRefreshes,
        c.staleFeedback - lastDumpCounters.staleFeedback,
        c.generationWraps - lastDumpCounters.generationWraps,
        0};
    for (unsigned i = 0; i < controls.size(); ++i)
        stats.recordControl[i] += controls[i];
    const std::array<uint64_t, 6> activity = {
        c.sameLine - lastDumpCounters.sameLine,
        0, 0,
        c.crossLine - lastDumpCounters.crossLine,
        c.crossLineTrained - lastDumpCounters.crossLineTrained,
        c.samplerWrites - lastDumpCounters.samplerWrites};
    for (unsigned i = 0; i < activity.size(); ++i)
        stats.trainingActivity[i] += activity[i];
    stats.compressionPotential[0] +=
        c.encodableRecords - lastDumpCounters.encodableRecords;
    stats.compressionPotential[1] +=
        c.installedLines - lastDumpCounters.installedLines;
    stats.compressionPotential[2] +=
        c.encodableLines - lastDumpCounters.encodableLines;
    stats.replacementControl[1] += c.rripAges - lastDumpCounters.rripAges;
    stats.replacementControl[2] +=
        c.replacementPromotions - lastDumpCounters.replacementPromotions;
    for (unsigned i = 0; i < 12; ++i) {
        stats.storageKinds[i] +=
            c.installsByKind[i] - lastDumpCounters.installsByKind[i];
        stats.storageKinds[12 + i] +=
            c.evictionsByKind[i] - lastDumpCounters.evictionsByKind[i];
        stats.storageKinds[24 + i] +=
            c.probesByKind[i] - lastDumpCounters.probesByKind[i];
    }
    lastDumpCounters = c;
    const std::array<statistics::Vector *, 6> vectors = {
        &stats.roleSets, &stats.filterSets, &stats.samplerSets,
        &stats.recorderSets, &stats.metaSets, &stats.confidenceSets};
    engine.visitTables([&](unsigned index, auto &table) {
        const auto sets = table.data.size();
        for (unsigned set = 0; set < sets; ++set) {
            auto &vector = *vectors[index];
            vector[set] += table.setAccesses[set] - table.reportedAccesses[set];
            vector[sets + set] +=
                table.setEvictions[set] - table.reportedEvictions[set];
            vector[2 * sets + set] = std::count_if(
                table.data[set].begin(), table.data[set].end(),
                [](const auto &entry) { return entry.valid; });
            table.reportedAccesses[set] = table.setAccesses[set];
            table.reportedEvictions[set] = table.setEvictions[set];
        }
    });
}
} // namespace gem5::prefetch

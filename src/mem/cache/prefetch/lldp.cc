#include "mem/cache/prefetch/lldp.hh"

#include <algorithm>
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
      ADD_STAT(lengthChanges, statistics::units::Count::get(), "Consumer chain length changes"),
      ADD_STAT(opChanges, statistics::units::Count::get(), "Consumer operation changes"),
      ADD_STAT(immChanges, statistics::units::Count::get(), "Consumer arithmetic or load immediate changes"),
      ADD_STAT(hints, statistics::units::Count::get(), "Hints retained on cache misses"),
      ADD_STAT(hitHintsDiscarded, statistics::units::Count::get(), "Hints discarded on cache hits"),
      ADD_STAT(returnedHints, statistics::units::Count::get(), "MSHR targets returning hinted data"),
      ADD_STAT(staleHints, statistics::units::Count::get(), "Hints whose producer generation was evicted"),
      ADD_STAT(candidates, statistics::units::Count::get(), "LLDP virtual address candidates"),
      ADD_STAT(duplicates, statistics::units::Count::get(), "Duplicate candidate cache lines"),
      ADD_STAT(unsupported, statistics::units::Count::get(), "Chains or data unsafe to replay"),
      ADD_STAT(childrenAtReplacement, statistics::units::Count::get(), "Valid consumers per evicted producer"),
      ADD_STAT(childrenAtDump, statistics::units::Count::get(), "Current producers by valid consumer count"),
      ADD_STAT(validProducers, statistics::units::Count::get(), "Current valid LLDT producers")
{
    childrenAtReplacement.init(SubEntries + 1);
    childrenAtDump.init(SubEntries + 1);
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
                     meta->lldpContext, meta->lldpLoadImm});
    stats.trainAccepted++;
    if (!learningEvent.scheduled())
        schedule(learningEvent, nextCycle());
}

LLDPrefetcher::Update
LLDPrefetcher::makeUpdate(Training train)
{
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
        entry.valid = true;
        entry.producerPC = train.chain.producerPC;
        entry.context = train.context;
        entry.generation = ++generation;
        entry.pConf = producerInitialConf;
    }
    const bool allocateSub = allocate || train.consumer < 0;
    unsigned col = allocateSub ? entry.replacement.victim() : train.consumer;
    if (allocateSub) {
        for (unsigned i = 0; i < SubEntries; ++i)
            if (!entry.consumers[i].valid) { col = i; break; }
    }
    auto &sub = entry.consumers[col];
    if (allocateSub) {
        sub = {};
        sub.valid = true;
        sub.consumerPC = train.consumerPC;
        sub.cConf = sub.immConf = initialConf;
        if (!allocate)
            entry.pConf = std::min<unsigned>(maxConf, entry.pConf + 1);
    } else {
        if (sub.loadImm == train.loadImm)
            sub.immConf = std::min<unsigned>(maxConf, sub.immConf + 1);
        else if (sub.immConf)
            --sub.immConf;
        sub.cConf = std::min<unsigned>(maxConf, sub.cConf + 1);
    }
    sub.chain = train.chain;
    sub.loadImm = train.loadImm;
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
    const auto &next = update.entry;
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
    table[update.row] = next;
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
    if (!input.empty()) {
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
    hint.size = meta->lldpSize;
    hint.signExtend = meta->lldpSigned;
    if (!hint.size || hint.offset + hint.size > blkSize ||
        pkt->req->getSize() != hint.size) {
        hint.valid = false;
        stats.unsupported++;
    }
    return hint;
}

lldp::Hint
LLDPrefetcher::loadTrain(const PacketPtr &pkt, bool miss)
{
    if (!pkt->isRead() || !pkt->isDemand() || pkt->req->isPrefetch() ||
        pkt->req->isInstFetch() || pkt->req->isUncacheable() ||
        pkt->req->isCacheMaintenance() || !pkt->req->hasVaddr() ||
        !pkt->req->hasPC() || !pkt->req->hasContextId() ||
        !pkt->req->hasXsMetadata())
        return {};
    const auto meta = pkt->req->getXsMetadata().instXsMetadata;
    if (!meta || !meta->lldpLoad || meta->squashed)
        return {};
    // L1 learns at successful IQ issue. L2 sees only requests forwarded by
    // L1, and learns those at its own tag-result boundary (hit or miss).
    if (!trainingCPU)
        dependenceTrain(meta);
    auto hint = pfHint(pkt);
    if (hint.valid) {
        if (miss) stats.hints++;
        else stats.hitHintsDiscarded++;
    }
    if (!miss)
        hint.valid = false;
    return hint;
}

void
LLDPrefetcher::hintData(const lldp::Hint &hint, const PacketPtr &demand,
                       const uint8_t *data, unsigned size)
{
    if (!hint.valid || !data || hint.offset + hint.size > size)
        return;
    const auto meta = demand->req->getXsMetadata().instXsMetadata;
    if (!meta || meta->squashed)
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
    PrefetchInfo origin(demand, demand->req->getVaddr(), true,
                         Request::XsMetadata(PrefetchSourceType::LLDP));
    for (const auto &sub : table[row].consumers) {
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
        stats.candidates++;
        if (!admitPfControlCandidate(PrefetchSourceType::LLDP))
            continue;
        PrefetchInfo candidate(origin, address);
        candidate.setXsMetadata(Request::XsMetadata(PrefetchSourceType::LLDP));
        AddrPriority command(address, 0, PrefetchSourceType::LLDP);
        // All LLDP values are virtual pointers and require a TLB lookup,
        // even if they happen to be on the trigger's page.
        command.forceTranslation = true;
        statsQueued.pfIdentified++;
        insert(demand, candidate, command);
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
    if (admitPfControlDeferredPacket(dpp))
        addToQueue(pfq, dpp);
    else
        delete dpp.pkt;
}

void
LLDPrefetcher::preDumpStats()
{
    Queued::preDumpStats();
    stats.validProducers = 0;
    for (unsigned i = 0; i <= SubEntries; ++i)
        stats.childrenAtDump[i] = 0;
    for (const auto &entry : table) {
        if (entry.valid) {
            stats.validProducers++;
            stats.childrenAtDump[validChildren(entry)]++;
        }
    }
}
} // namespace prefetch
} // namespace gem5

#include "cpu/o3/upstream_pdip.hh"

#include <algorithm>

#include "base/random.hh"

namespace gem5
{
namespace o3
{

UpstreamPDIP::UpstreamPDIP(const Config &config_)
    : config(config_)
{
    if (config.sets == 0)
        config.sets = 1;
    if (config.assoc == 0)
        config.assoc = 1;
    if (config.targetsPerEntry == 0)
        config.targetsPerEntry = 1;
    if (config.queueSize == 0)
        config.queueSize = 1;
    if (config.blockSize == 0)
        config.blockSize = 64;
    if (config.tagBits == 0)
        config.tagBits = 1;
    if (config.tagBits >= sizeof(Addr) * 8)
        config.tagBits = sizeof(Addr) * 8 - 1;
    if (config.insertionProbability > 100)
        config.insertionProbability = 100;
    table.resize(config.sets, std::vector<Entry>(config.assoc));
}

unsigned
UpstreamPDIP::setIndex(Addr block) const
{
    return static_cast<unsigned>((block / config.blockSize) % config.sets);
}

Addr
UpstreamPDIP::tagMask() const
{
    return (static_cast<Addr>(1) << config.tagBits) - 1;
}

Addr
UpstreamPDIP::tagFor(Addr block) const
{
    const Addr blockNumber = block / config.blockSize;
    return (blockNumber / config.sets) & tagMask();
}

UpstreamPDIP::Entry *
UpstreamPDIP::find(Addr trigger)
{
    auto &set = table[setIndex(trigger)];
    const Addr tag = tagFor(trigger);
    for (auto &entry : set) {
        if (entry.valid && entry.tag == tag)
            return &entry;
    }
    return nullptr;
}

UpstreamPDIP::Entry *
UpstreamPDIP::allocate(Addr trigger)
{
    auto &set = table[setIndex(trigger)];
    Entry *victim = &set.front();
    for (auto &entry : set) {
        if (!entry.valid) {
            victim = &entry;
            break;
        }
        if (entry.lru < victim->lru)
            victim = &entry;
    }
    victim->valid = true;
    victim->tag = tagFor(trigger);
    victim->lru = ++lruTick;
    victim->targets.clear();
    return victim;
}

void
UpstreamPDIP::clearQueue()
{
    for (auto &queue : queues)
        queue.clear();
}

void
UpstreamPDIP::clearQueue(ThreadID tid)
{
    if (tid < MaxThreads)
        queues[tid].clear();
}

void
UpstreamPDIP::notifyResteer(Addr trigger, ThreadID tid)
{
    if (tid < MaxThreads)
        lastTrigger[tid] = trigger - trigger % config.blockSize;
}

void
UpstreamPDIP::clearTrigger(ThreadID tid)
{
    if (tid < MaxThreads)
        lastTrigger[tid] = 0;
}

Addr
UpstreamPDIP::currentTrigger(ThreadID tid) const
{
    return tid < MaxThreads ? lastTrigger[tid] : 0;
}

bool
UpstreamPDIP::promoteFec(Addr candidate, Addr trigger, ThreadID tid,
                         unsigned mask)
{
    if (tid >= MaxThreads || candidate == 0 || trigger == 0)
        return false;

    ++stats_.fecPromotions;
    candidate -= candidate % config.blockSize;
    trigger -= trigger % config.blockSize;
    const uint8_t effectiveMask = static_cast<uint8_t>(mask & 0xf);
    if (effectiveMask == 0)
        return false;

    if (config.insertionProbability == 0)
        return false;

    // The paper uses a probabilistic insertion policy.  Use gem5's seeded
    // generator so runs remain reproducible for a fixed simulator seed.
    if (config.insertionProbability < 100 &&
        random_mt.random<unsigned>(0, 99) >= config.insertionProbability)
        return false;

    Entry *entry = find(trigger);
    if (!entry)
        entry = allocate(trigger);
    entry->lru = ++lruTick;

    auto target = std::find_if(entry->targets.begin(), entry->targets.end(),
                               [candidate](const Target &t) {
                                   return t.line == candidate;
                               });
    if (target != entry->targets.end()) {
        target->mask |= effectiveMask;
    } else {
        if (entry->targets.size() >= config.targetsPerEntry)
            entry->targets.erase(entry->targets.begin());
        entry->targets.push_back({candidate, effectiveMask});
    }
    ++stats_.tableInsertions;
    return true;
}

std::vector<Addr>
UpstreamPDIP::lookup(Addr trigger, ThreadID tid)
{
    std::vector<Addr> result;
    if (tid >= MaxThreads || trigger == 0)
        return result;
    Entry *entry = find(trigger);
    if (!entry) {
        ++stats_.tableMisses;
        return result;
    }
    ++stats_.tableHits;
    entry->lru = ++lruTick;
    for (const auto &target : entry->targets) {
        for (unsigned bit = 0; bit < 4; ++bit) {
            if (target.mask & (1U << bit))
                result.push_back(target.line + bit * config.blockSize);
        }
    }
    stats_.triggerPrefetches += result.size();
    return result;
}

bool
UpstreamPDIP::enqueue(Addr addr, ThreadID tid)
{
    if (tid >= MaxThreads)
        return false;
    addr -= addr % config.blockSize;
    auto &queue = queues[tid];
    if (std::find(queue.begin(), queue.end(), addr) != queue.end()) {
        ++stats_.duplicateDrops;
        return false;
    }
    if (queue.size() >= config.queueSize) {
        ++stats_.queueDrops;
        return false;
    }
    queue.push_back(addr);
    return true;
}

bool
UpstreamPDIP::dequeue(Addr &addr, ThreadID tid)
{
    if (tid >= MaxThreads || queues[tid].empty())
        return false;
    addr = queues[tid].front();
    queues[tid].pop_front();
    return true;
}

bool
UpstreamPDIP::empty(ThreadID tid) const
{
    return tid >= MaxThreads || queues[tid].empty();
}

unsigned
UpstreamPDIP::queueSize(ThreadID tid) const
{
    return tid >= MaxThreads ? 0 : queues[tid].size();
}

} // namespace o3
} // namespace gem5

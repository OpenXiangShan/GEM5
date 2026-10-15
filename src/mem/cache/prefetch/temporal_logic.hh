// Temporal prefetch behavior derived from coupledL2 TemporalPrefetch.scala.
#ifndef __MEM_CACHE_PREFETCH_TEMPORAL_LOGIC_HH__
#define __MEM_CACHE_PREFETCH_TEMPORAL_LOGIC_HH__

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "mem/lldp.hh"

namespace gem5::prefetch::temporal
{
inline uint64_t
hashPC(uint64_t pc)
{
    constexpr uint64_t mask = (1ULL << 22) - 1;
    // RISC-V compressed instruction PCs carry useful information in bit 1.
    pc >>= 1;
    const uint64_t folded = (pc ^ (pc >> 22) ^ (pc >> 44)) & mask;
    return (folded ^ (folded >> 11) ^ ((folded & 511) << 13) ^
            (folded >> 5)) & mask;
}

// Fixed-capacity set-associative tables with tree PLRU. Context is additional
// gem5 state: RTL instantiates the tables per hart.
template <class T, unsigned Sets, unsigned Ways> struct Table
{
    std::array<std::array<T, Ways>, Sets> data{};
    std::array<lldp::PLRU<Ways>, Sets> replacement{};
    std::array<uint64_t, Sets> setAccesses{}, setEvictions{};
    std::array<uint64_t, Sets> reportedAccesses{}, reportedEvictions{};
    template <class Match> T &access(unsigned set, Match match, bool &hit,
                                     bool touchOnMiss = true)
    {
        ++setAccesses[set];
        unsigned way = 0;
        hit = false;
        for (; way < Ways; ++way) {
            if (data[set][way].valid && match(data[set][way])) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            way = replacement[set].victim();
            for (unsigned i = 0; i < Ways; ++i)
                if (!data[set][i].valid) { way = i; break; }
            if (touchOnMiss && data[set][way].valid)
                ++setEvictions[set];
        }
        if (hit || touchOnMiss)
            replacement[set].touch(way);
        return data[set][way];
    }
};

struct PCEntry
{
    bool valid{false};
    uint64_t pc{0};
    int context{-1};
    uint8_t roles{0}; // 1: LLDP producer, 2: LLDP consumer
};
struct Filter : PCEntry
{
    uint64_t last{0};
    uint8_t count{0}, epoch{0};
};
struct Sample : PCEntry
{
    uint64_t base{0}, target{0};
    uint8_t matches{0};
};
struct Record : PCEntry
{
    uint64_t trigger{0};
    std::vector<uint64_t> lines;
};
struct Confidence : PCEntry
{
    uint8_t issue{32}, meta{32};
};
struct Counters
{
    uint64_t trains{0}, filterHits{0}, samplerHits{0}, pairs{0};
    uint64_t records{0}, replacements{0}, metaHits{0}, metaBlocked{0};
    uint64_t issueBlocked{0}, issueProbes{0}, metaProbes{0};
};

class Engine
{
    Table<PCEntry, 256, 4> roleTable;
    Table<Filter, 32, 8> filter;
    Table<Sample, 1024, 16> sampler;
    Table<Record, 128, 8> recorder;
    Table<Record, 1024, 16> meta;
    Table<Confidence, 64, 8> confidence;
    unsigned issueProbe{0}, metaProbe{0};
    const unsigned maxLength;

    Confidence &conf(uint64_t pc, int context)
    {
        bool hit;
        auto &c = confidence.access(pc & 63,
            [=](const auto &e) { return e.pc == pc && e.context == context; }, hit);
        if (!hit) {
            c = {};
            c.valid = true;
            c.pc = pc;
            c.context = context;
        }
        return c;
    }
    unsigned metaSet(uint64_t pc, uint64_t trigger) const
    {
        // RTL mixes a 42-bit PA line and a rotated 22-bit PC hash.
        const uint64_t rotated = ((pc << 5) | (pc >> 37)) & ((1ULL << 42) - 1);
        return (trigger ^ (trigger >> 10) ^ (trigger >> 20) ^
                (trigger >> 30) ^ rotated ^ (pc >> 6)) & 1023;
    }
    void install(const Record &record)
    {
        auto &c = conf(record.pc, record.context);
        if (c.meta < 32 && (++metaProbe & 63)) {
            ++counters.metaBlocked;
            return;
        }
        if (c.meta < 32)
            ++counters.metaProbes;
        bool hit;
        auto &dst = meta.access(metaSet(record.pc, record.trigger),
            [&](const auto &e) { return e.pc == record.pc &&
                e.context == record.context && e.trigger == record.trigger; }, hit);
        dst = record;
        ++counters.records;
    }
    void recordPair(uint64_t pc, int context, uint64_t base, uint64_t target,
                    uint8_t roles)
    {
        bool hit;
        auto &r = recorder.access(pc & 127,
            [=](const auto &e) { return e.pc == pc && e.context == context; }, hit);
        if (!hit) {
            if (r.valid && r.lines.size() > 8) {
                install(r);
                ++counters.replacements;
            }
            r = {};
            r.valid = true;
            r.pc = pc;
            r.context = context;
            r.roles = roles;
            r.trigger = base;
            r.lines = {target};
            return;
        }
        r.roles |= roles;
        const bool addBase = std::find(r.lines.begin(), r.lines.end(), base) == r.lines.end();
        const bool addTarget = std::find(r.lines.begin(), r.lines.end(), target) == r.lines.end();
        const bool chainFull = addBase && addTarget && r.lines.size() == maxLength - 1;
        if (addBase && r.lines.size() < maxLength)
            r.lines.push_back(base);
        if (addTarget && r.lines.size() < maxLength)
            r.lines.push_back(target);
        if (r.lines.size() == maxLength) {
            install(r);
            if (chainFull) {
                r.trigger = base;
                r.lines = {target};
            } else {
                r = {};
            }
        }
    }

  public:
    Counters counters;
    explicit Engine(unsigned length = 12) : maxLength(length) {}

    // Expose observations, without changing table lookup or replacement.
    template <class Visitor> void visitTables(Visitor visitor)
    {
        visitor(0, roleTable);
        visitor(1, filter);
        visitor(2, sampler);
        visitor(3, recorder);
        visitor(4, meta);
        visitor(5, confidence);
    }

    void mark(uint64_t actualPC, int context, uint8_t roles)
    {
        const auto pc = hashPC(actualPC);
        bool hit;
        auto &r = roleTable.access(pc & 255,
            [=](const auto &e) { return e.pc == pc && e.context == context; }, hit);
        if (!hit) {
            r = {};
            r.valid = true;
            r.pc = pc;
            r.context = context;
        }
        r.roles |= roles;
    }
    uint8_t roles(uint64_t actualPC, int context) const
    {
        const auto pc = hashPC(actualPC);
        for (const auto &e : roleTable.data[pc & 255])
            if (e.valid && e.pc == pc && e.context == context)
                return e.roles;
        return 0;
    }

    // Line addresses in and out; byte offsets are irrelevant to TP.
    std::optional<Record> observe(uint64_t actualPC, int context, uint64_t line)
    {
        const auto pc = hashPC(actualPC);
        const auto role = roles(actualPC, context);
        ++counters.trains;
        bool hit;
        auto &m = meta.access(metaSet(pc, line),
            [=](const auto &e) { return e.pc == pc && e.context == context &&
                e.trigger == line; }, hit, false);
        std::optional<Record> output;
        if (hit) {
            ++counters.metaHits;
            auto &c = conf(pc, context);
            if (c.issue >= 32 || !(++issueProbe & 63)) {
                output = m;
                output->roles |= role;
                if (c.issue < 32)
                    ++counters.issueProbes;
            } else {
                ++counters.issueBlocked;
            }
        }
        auto &f = filter.access(pc & 31,
            [=](const auto &e) { return e.pc == pc && e.context == context; }, hit);
        const uint8_t epoch = (counters.trains / 32768) & 15;
        if (!hit) {
            f = {};
            f.valid = true;
            f.pc = pc;
            f.context = context;
            f.last = line;
            f.epoch = epoch;
            f.roles = role;
            return output;
        }
        ++counters.filterHits;
        const uint64_t base = f.last;
        const uint8_t count = f.epoch == epoch ? f.count : f.count >> 1;
        f.last = line;
        f.count = std::min<unsigned>(63, count + 1);
        f.epoch = epoch;
        f.roles |= role;
        auto &s = sampler.access(base & 1023,
            [=](const auto &e) { return e.base == base && e.context == context; }, hit);
        const bool matches = hit && s.target == line && s.pc == pc;
        const bool record = hit && (matches || s.matches);
        if (hit)
            ++counters.samplerHits;
        if (record || count >= 31) {
            ++counters.pairs;
            recordPair(pc, context, base, line, f.roles);
        }
        if (!hit || record || count >= 31) {
            const uint8_t matchCount = !hit ? 0 : matches ?
                std::min<unsigned>(7, s.matches + 1) :
                s.target != line && s.matches ? s.matches - 1 : s.matches;
            s = {};
            s.valid = true;
            s.base = base;
            s.target = line;
            s.pc = pc;
            s.context = context;
            s.matches = matchCount;
            s.roles = f.roles;
        }
        return output;
    }
    // RTL charges a group when it leaves the data queue. The gem5 adapter
    // charges accepted queue entries; later useful/late feedback rewards PCs.
    void issued(uint64_t actualPC, int context, unsigned count)
    {
        auto &c = conf(hashPC(actualPC), context);
        const unsigned issuePenalty = (count * 25 + 99) / 100;
        const unsigned metaPenalty = (count * 40 + 99) / 100;
        c.issue = c.issue > issuePenalty ? c.issue - issuePenalty : 0;
        c.meta = c.meta > metaPenalty ? c.meta - metaPenalty : 0;
    }
    void useful(uint64_t actualPC, int context, bool late = false)
    {
        auto &c = conf(hashPC(actualPC), context);
        c.issue = std::min<unsigned>(63, c.issue + 1);
        if (!late)
            c.meta = std::min<unsigned>(63, c.meta + 1);
    }
};
} // namespace gem5::prefetch::temporal
#endif

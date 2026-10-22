// Temporal prefetch behavior derived from coupledL2 TemporalPrefetch.scala.
#ifndef __MEM_CACHE_PREFETCH_TEMPORAL_LOGIC_HH__
#define __MEM_CACHE_PREFETCH_TEMPORAL_LOGIC_HH__

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "mem/cache/prefetch/temporal_feedback.hh"
#include "mem/cache/prefetch/temporal_storage.hh"
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

inline uint64_t placementPC(uint64_t identity)
{ return identity ^ (identity >> 7) ^ (identity >> 13); }

// Fixed-capacity set-associative tables with tree PLRU. Context is additional
// gem5 state: RTL instantiates the tables per hart. Meta manages RRIP
// separately from this generic table's PLRU state.
template <class T, unsigned Sets, unsigned Ways> struct Table
{
    template <class E> using Rows = std::conditional_t<Sets != 0,
        std::array<E, Sets>, std::vector<E>>;
    Rows<std::array<T, Ways>> data{};
    Rows<lldp::PLRU<Ways>> replacement{};
    Rows<uint64_t> setAccesses{}, setEvictions{};
    Rows<uint64_t> reportedAccesses{}, reportedEvictions{};
    explicit Table(unsigned sets = Sets)
    {
        if constexpr (!Sets) {
            data.resize(sets);
            replacement.resize(sets);
            setAccesses.resize(sets);
            setEvictions.resize(sets);
            reportedAccesses.resize(sets);
            reportedEvictions.resize(sets);
        }
    }
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
    uint8_t kind{2}; // Diagnostic only: 0 integer, 1 FP, 2 unclassified.
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
struct Replay : Record
{
    RecordRef reference;
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
    uint64_t recordRefreshes{0};
    uint64_t staleFeedback{0}, generationWraps{0};
    uint64_t sameLine{0};
    uint64_t crossLine{0}, crossLineTrained{0}, samplerWrites{0};
    uint64_t encodableRecords{0}, installedLines{0}, encodableLines{0};
    uint64_t rripAges{0}, replacementPromotions{0};
    std::array<uint64_t, 12> installsByKind{}, evictionsByKind{}, probesByKind{};
};

class Engine
{
  public:
    static constexpr unsigned META_ENTRIES = 24576;
    static constexpr unsigned META_SETS = META_ENTRIES / 16;

  private:
    Table<PCEntry, 256, 4> roleTable;
    Table<Filter, 32, 8> filter;
    Table<Sample, 1024, 16> sampler;
    Table<Record, 128, 8> recorder;
    Table<Record, 0, 16> meta;
    Table<Confidence, 64, 8> confidence;
    unsigned issueProbe{0}, metaProbe{0};
    const unsigned maxLength;
    RecordPools pools;
    std::vector<PoolRef> poolReferences;
    std::vector<RecordState> recordStates;
    std::vector<uint8_t> rrpv;
    std::function<void(unsigned)> invalidateSlot;

    unsigned recordClass(const Record &record) const
    { return unsigned(record.kind) * 4 + record.roles; }

    unsigned slot(unsigned set, const Record &entry) const
    { return set * 16 + (&entry - meta.data[set].data()); }

    RecordState *state(RecordRef reference)
    {
        if (reference.bits == 0xffffffff)
            return nullptr;
        const auto index = reference.slot();
        if (index >= recordStates.size())
            return nullptr;
        auto &s = recordStates[index];
        if (!meta.data[index / 16][index % 16].valid ||
            s.generation() != reference.generation())
            return nullptr;
        return &s;
    }

    Confidence &conf(uint64_t pc, int context)
    {
        bool hit;
        auto &c = confidence.access(placementPC(pc) & 63,
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
                (trigger >> 30) ^ rotated ^ (pc >> 6)) % meta.data.size();
    }

    std::vector<uint64_t> lines(unsigned set, const Record &entry) const
    {
        return pools.load(poolReferences[slot(set, entry)], entry.trigger);
    }
    Record &accessMeta(uint64_t pc, int context, uint64_t trigger,
                       bool &hit, bool allocate)
    {
        const auto set = metaSet(pc, trigger);
        auto match = [=](const auto &e) {
            return e.pc == pc && e.context == context && e.trigger == trigger;
        };
        ++meta.setAccesses[set];
        unsigned way = 0;
        hit = false;
        for (; way < 16; ++way) {
            if (meta.data[set][way].valid && match(meta.data[set][way])) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            way = 0;
            bool invalid = false;
            for (unsigned w = 0; w < 16; ++w) {
                if (!meta.data[set][w].valid) {
                    way = w;
                    invalid = true;
                    break;
                }
            }
            if (!invalid && allocate) {
                for (;;) {
                    for (way = 0; way < 16; ++way)
                        if (rrpv[set * 16 + way] == 3)
                            break;
                    if (way != 16)
                        break;
                    for (unsigned w = 0; w < 16; ++w)
                        ++rrpv[set * 16 + w];
                    ++counters.rripAges;
                }
            }
            if (allocate && meta.data[set][way].valid) {
                ++meta.setEvictions[set];
                ++counters.evictionsByKind[recordClass(meta.data[set][way])];
            }
        }
        return meta.data[set][way];
    }

    void install(const Record &record)
    {
        auto &c = conf(record.pc, record.context);
        const bool blocked = c.meta < 32 && (++metaProbe & 63);
        if (blocked) {
            ++counters.metaBlocked;
            return;
        }
        if (c.meta < 32)
            ++counters.metaProbes;
        bool hit;
        auto &dst = accessMeta(record.pc, record.context, record.trigger,
                               hit, true);
        const auto set = metaSet(record.pc, record.trigger);
        const auto index = slot(set, dst);
        const bool same = hit && lines(set, dst) == record.lines;
        if (!pools.store(poolReferences[index], record.trigger, record.lines)) {
            // Pool exhaustion retains the victim. Keep RRIP aging from the
            // attempted lookup, but do not count an actual eviction.
            if (!hit && dst.valid) {
                --meta.setEvictions[set];
                --counters.evictionsByKind[recordClass(dst)];
            }
            return;
        }
        auto &s = recordStates[index];
        if (same) {
            ++counters.recordRefreshes;
        } else {
            if (s.generation() == 0x7fff) {
                ++counters.generationWraps;
                if (!invalidateSlot)
                    throw std::overflow_error(
                        "TP generation wrap requires owner invalidation");
                invalidateSlot(index);
            }
            s.renew();
            rrpv[index] = 2;
        }
        static_cast<PCEntry &>(dst) = record;
        dst.trigger = record.trigger;
        ++counters.records;
        ++counters.installsByKind[recordClass(record)];
        counters.installedLines += record.lines.size();
        const bool encodable = std::all_of(
            record.lines.begin(), record.lines.end(), [&](uint64_t line) {
                return line >= record.trigger ?
                    line - record.trigger <= 32767 :
                    record.trigger - line <= 32768;
            });
        if (encodable) {
            ++counters.encodableRecords;
            counters.encodableLines += record.lines.size();
        }
    }
    void recordPair(uint64_t pc, int context, uint64_t base, uint64_t target,
                    uint8_t roles, uint8_t kind)
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
            r.kind = kind;
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
    explicit Engine(unsigned length = 12)
        : meta(META_SETS), maxLength(length),
          pools(length, META_ENTRIES * 3 / 4, META_ENTRIES / 4),
          poolReferences(META_ENTRIES), recordStates(META_ENTRIES),
          rrpv(META_ENTRIES)
    {
        if (length < 9 || length > 16)
            throw std::invalid_argument("Invalid TP record length");
    }

    const RecordPools &recordPools() const { return pools; }

    void setInvalidationHandler(std::function<void(unsigned)> handler)
    { invalidateSlot = std::move(handler); }

    std::optional<RecordState> inspect(RecordRef reference)
    {
        auto *s = state(reference);
        return s ? std::optional<RecordState>(*s) : std::nullopt;
    }

    std::optional<unsigned> replacementPriority(RecordRef reference)
    {
        if (!state(reference))
            return std::nullopt;
        return rrpv[reference.slot()];
    }

    bool feedback(RecordRef reference, FeedbackKind kind)
    {
        auto *s = state(reference);
        if (!s) {
            ++counters.staleFeedback;
            return false;
        }
        const auto index = reference.slot();
        rrpv[index] = kind == FeedbackKind::Timely ? 0 :
            kind == FeedbackKind::Unused ? 3 : 1;
        if (kind == FeedbackKind::Timely)
            ++counters.replacementPromotions;
        return true;
    }

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

    // Observation only: no PLRU, confidence, training or counter updates.
    std::optional<Record> peek(uint64_t actualPC, int context, uint64_t line) const
    {
        const auto pc = hashPC(actualPC);
        for (const auto &entry : meta.data[metaSet(pc, line)])
            if (entry.valid && entry.pc == pc && entry.context == context &&
                entry.trigger == line)
            {
                Record result = entry;
                result.lines = lines(metaSet(pc, line), entry);
                return result;
            }
        return std::nullopt;
    }

    // Query existing Meta only. No Filter/Sampler/Recorder or training epoch
    // updates; admission and candidate feedback remain the normal TP policy.
    std::optional<Replay>
    query(uint64_t actualPC, int context, uint64_t line)
    {
        const auto pc = hashPC(actualPC);
        const auto role = roles(actualPC, context);
        bool hit;
        auto &m = accessMeta(pc, context, line, hit, false);
        std::optional<Replay> output;
        if (hit) {
            ++counters.metaHits;
            auto &c = conf(pc, context);
            const auto index = slot(metaSet(pc, line), m);
            const auto &s = recordStates[index];
            const bool low = c.issue < 32;
            const bool probe = low && !(++issueProbe & 63);
            if (!low || probe) {
                output.emplace();
                static_cast<Record &>(*output) = m;
                output->lines = lines(metaSet(pc, line), m);
                output->reference = RecordRef(index, s.generation());
                output->roles |= role;
                if (low)
                    ++counters.issueProbes;
            } else {
                ++counters.issueBlocked;
            }
        }
        return output;
    }

    // Line addresses in and out; byte offsets are irrelevant to TP.
    std::optional<Replay>
    observe(uint64_t actualPC, int context, uint64_t line, uint8_t kind = 2)
    {
        const auto pc = hashPC(actualPC);
        const auto role = roles(actualPC, context);
        ++counters.trains;
        auto output = query(actualPC, context, line);
        bool hit;
        auto &f = filter.access(placementPC(pc) & 31,
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
            f.kind = kind;
            return output;
        }
        ++counters.filterHits;
        const uint64_t base = f.last;
        const uint8_t count = f.epoch == epoch ? f.count : f.count >> 1;
        f.last = line;
        f.count = std::min<unsigned>(63, count + 1);
        f.epoch = epoch;
        f.roles |= role;
        f.kind = kind;
        if (base == line) {
            ++counters.sameLine;
        } else {
            ++counters.crossLine;
            ++counters.crossLineTrained;
        }
        auto &s = sampler.access(base & 1023,
            [=](const auto &e) { return e.base == base && e.context == context; }, hit);
        const bool matches = hit && s.target == line && s.pc == pc;
        const bool record = hit && (matches || s.matches);
        if (hit)
            ++counters.samplerHits;
        if (record || count >= 31) {
            ++counters.pairs;
            recordPair(pc, context, base, line, f.roles, f.kind);
        }
        if (!hit || record || count >= 31) {
            ++counters.samplerWrites;
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

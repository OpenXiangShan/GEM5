// Copyright (c) 2026 OpenXiangShan
// SPDX-License-Identifier: BSD-3-Clause
#ifndef __MEM_CACHE_PREFETCH_PREFETCH_TRACE_HH__
#define __MEM_CACHE_PREFETCH_PREFETCH_TRACE_HH__

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>

#include "mem/packet.hh"
#include "params/PrefetchTrace.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_object.hh"

namespace gem5
{
class OutputStream;

/** Host-only fact recorder. No cache or prefetch decisions depend on it. */
class PrefetchTrace : public SimObject
{
  public:
    struct Record
    {
        const char *event;
        std::string actor;
        std::string cache;
        unsigned level;
        Tick period;
        RequestPtr req;
        uint64_t prefetchId = 0;
        uint64_t relatedId = 0;
        std::optional<Addr> pa;
        std::optional<Addr> va;
        bool targetAddress = false;
        int source = -1;
        std::string op;
        std::string command;
        Tick ready = MaxTick;
        std::string result;
        std::string reason;

        Record(const char *type, const std::string &name, unsigned cache_level,
               Tick clock_period, const RequestPtr &request,
               const std::string &owner = "")
            : event(type), actor(name), cache(owner.empty() ? name : owner),
              level(cache_level),
              period(clock_period), req(request) {}
    };

    PARAMS(PrefetchTrace);
    explicit PrefetchTrace(const PrefetchTraceParams &p);
    void startup() override;
    bool active() const;
    uint64_t newId() { return ++nextId; }
    uint64_t requestId(const RequestPtr &req);
    void emit(Record r);
    void packet(const char *event, const ClockedObject &actor, unsigned level,
                const PacketPtr pkt, const std::string &reason = "",
                uint64_t related = 0, const std::string &result = "",
                const std::string &cache = "");
    void fill(const ClockedObject &actor, unsigned level, PacketPtr pkt,
              const RequestPtr &prefetch, bool merged, Tick ready);
    void use(const ClockedObject &actor, unsigned level, PacketPtr pkt,
             const std::string &result);
    void registerCache(const ClockedObject &actor, unsigned level,
                       unsigned capacity, std::function<unsigned()> valid);
    void cacheOccupancy(const std::string &cache);
    void evict(const ClockedObject &actor, unsigned level, Addr line,
               bool secure, const char *reason);

  private:
    using Line = std::tuple<std::string, Addr, bool>;
    struct Resident
    {
        RequestPtr req;
        uint64_t id;
        Tick ready;
        bool used = false;
    };
    std::map<Line, Resident> residents;
    struct CacheOccupancy
    {
        unsigned level;
        Tick period;
        unsigned capacity;
        std::function<unsigned()> valid;
        unsigned prefetched = 0;
        unsigned unused = 0;
    };
    std::map<std::string, CacheOccupancy> cacheOccupancies;
    EventFunctionWrapper beginEvent;
    void begin();
    const Tick startTick;
    const Tick endTick;
    const bool afterWarmup;
    const size_t bufferSize;
    const unsigned blockSize;
    bool started;
    uint64_t nextId = 0;
    uint64_t order = 0;
    OutputStream *output = nullptr;
    std::ostringstream buffer;
    void flush();
    void finish();
};
} // namespace gem5
#endif

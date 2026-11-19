// Copyright (c) 2026 OpenXiangShan
// SPDX-License-Identifier: BSD-3-Clause
#include "mem/cache/prefetch/prefetch_trace.hh"

#include <iomanip>

#include "base/logging.hh"
#include "base/output.hh"
#include "base/statistics.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace
{
std::string
csv(const std::string &value)
{
    std::string out = "\"";
    for (char c : value) {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + '"';
}

void
address(std::ostream &out, std::optional<Addr> addr)
{
    if (addr)
        out << "0x" << std::hex << *addr << std::dec;
}
} // anonymous namespace

PrefetchTrace::PrefetchTrace(const PrefetchTraceParams &p)
    : SimObject(p), beginEvent([this] { begin(); }, name() + ".begin"),
      startTick(p.start_tick), endTick(p.end_tick),
      afterWarmup(p.after_warmup), bufferSize(p.buffer_size),
      blockSize(p.block_size), started(!p.after_warmup)
{
    fatal_if(startTick >= endTick, "Prefetch trace requires start_tick < end_tick");
    fatal_if(!blockSize || (blockSize & (blockSize - 1)),
             "Prefetch trace block_size must be a power of two");
    statistics::registerResetCallback([this] {
        if (afterWarmup && !started && curTick() > 0) {
            started = true;
            begin();
        }
    });
    registerExitCallback([this] { finish(); });
}

void
PrefetchTrace::startup()
{
    output = simout.create(params().filename, false, true);
    auto &out = *output->stream();
    out << "# prefetch_trace_version=3\n# block_size=" << blockSize
        << "\n# start_tick=" << startTick << "\n# end_tick=" << endTick
        << "\n# after_warmup=" << afterWarmup
        << "\n# cache_occupancy_version=1\n";
    out << "event,order,tick,cycle,actor,level,request_id,prefetch_id,"
           "related_id,pc,vaddr,paddr,secure,context_id,requestor_id,"
           "inst_seq,op,size,result,source,reason,ready_tick,cache,command,address_space,arrival_tick\n";
    schedule(beginEvent, startTick);
}

void
PrefetchTrace::begin()
{
    if (!active()) return;
    emit({"ROI_BEGIN", name(), 0, 1, nullptr});
    for (const auto &[cache, state] : cacheOccupancies)
        cacheOccupancy(cache);
}

void
PrefetchTrace::registerCache(const ClockedObject &actor, unsigned level,
                            unsigned capacity, std::function<unsigned()> valid)
{
    cacheOccupancies.emplace(actor.name(), CacheOccupancy{
        level, actor.clockPeriod(), capacity, std::move(valid)});
}

void
PrefetchTrace::cacheOccupancy(const std::string &cache)
{
    if (!active()) return;
    const auto &state = cacheOccupancies.at(cache);
    Record r{"CACHE_OCCUPANCY", cache, state.level, state.period, nullptr};
    r.result = "{\"valid_lines\":" + std::to_string(state.valid()) +
        ",\"prefetched_lines\":" + std::to_string(state.prefetched) +
        ",\"unused_prefetched_lines\":" + std::to_string(state.unused) +
        ",\"capacity_lines\":" + std::to_string(state.capacity) + "}";
    emit(r);
}

bool
PrefetchTrace::active() const
{
    return started && curTick() >= startTick && curTick() < endTick;
}

uint64_t
PrefetchTrace::requestId(const RequestPtr &req)
{
    if (req && !req->analysisRequestId)
        req->analysisRequestId = ++nextId;
    return req ? req->analysisRequestId : 0;
}

void
PrefetchTrace::emit(Record r)
{
    if (!active() || !output)
        return;
    const Tick tick = curTick();
    const auto &req = r.req;
    const RequestPtr meta = req && req->analysisOrigin ? req->analysisOrigin : req;
    if (!r.targetAddress && req) {
        if (req->hasPaddr()) r.pa = req->getPaddr();
        if (req->hasVaddr()) r.va = req->getVaddr();
        else if (meta && meta->hasVaddr())
            r.va = meta->getVaddr() & ~(Addr(blockSize) - 1);
    }
    buffer << r.event << ',' << ++order << ',' << tick << ','
           << tick / r.period << ',' << csv(r.actor) << ',' << r.level
           << ',' << requestId(req) << ',' << r.prefetchId << ','
           << r.relatedId << ',';
    address(buffer, meta && meta->hasPC() ?
            std::optional<Addr>(meta->getPC()) : std::nullopt);
    buffer << ',';
    address(buffer, r.va);
    buffer << ',';
    address(buffer, r.pa);
    buffer << ',' << (req && req->isSecure()) << ',';
    if (req && req->hasContextId()) buffer << req->contextId();
    buffer << ',';
    if (req) buffer << req->requestorId();
    buffer << ',';
    if (req && req->hasInstSeqNum()) {
        buffer << req->getReqInstSeqNum();
    } else if (meta && meta->hasInstSeqNum()) {
        buffer << meta->getReqInstSeqNum();
    } else if (meta && meta->hasXsMetadata() &&
               meta->getXsMetadata().instXsMetadata) {
        buffer << meta->getXsMetadata().instXsMetadata->seqNum;
    }
    buffer << ',';
    buffer << csv(r.op);
    buffer << ',';
    if (req && req->hasSize()) buffer << req->getSize();
    buffer << ',' << csv(r.result) << ',';
    if (r.source >= 0) buffer << r.source;
    else if (req && req->hasXsMetadata())
        buffer << req->getXsMetadata().prefetchSource;
    buffer << ',' << csv(r.reason) << ',';
    if (r.ready != MaxTick) buffer << r.ready;
    buffer << ',' << csv(r.cache) << ',' << csv(r.command) << ',';
    if (meta && meta->analysisAddressSpaceMode) {
        buffer << (meta->analysisAddressSpaceMode == 1 ? "riscv-fs" : "se");
        for (auto part : meta->analysisAddressSpace)
            buffer << ':' << std::hex << part << std::dec;
    }
    buffer << ',';
    if (req && req->analysisArrivalTick != MaxTick)
        buffer << req->analysisArrivalTick;
    buffer << '\n';
    if (static_cast<size_t>(buffer.tellp()) >= bufferSize)
        flush();
}

void
PrefetchTrace::packet(const char *event, const ClockedObject &actor,
                     unsigned level, const PacketPtr pkt,
                     const std::string &reason, uint64_t related,
                     const std::string &result, const std::string &cache)
{
    if (!active()) return;
    Record r{event, actor.name(), level, actor.clockPeriod(), pkt->req, cache};
    r.prefetchId = pkt->req->analysisPrefetchId;
    r.relatedId = related;
    r.result = result;
    r.reason = reason;
    r.op = pkt->req->isPrefetch() ? "prefetch" :
        pkt->isWrite() ? "store" : pkt->isRead() ? "load" : "other";
    r.command = pkt->cmd.toString();
    if (pkt->isDcacheMainPipeSbufferReq() && r.reason.empty())
        r.reason = "store_buffer_representative";
    emit(r);
}

void
PrefetchTrace::fill(const ClockedObject &actor, unsigned level, PacketPtr pkt,
                   const RequestPtr &prefetch, bool merged, Tick ready)
{
    Record r{"PF_FILL", actor.name(), level, actor.clockPeriod(), prefetch};
    r.prefetchId = prefetch->analysisPrefetchId;
    r.targetAddress = true;
    r.pa = pkt->getAddr();
    r.result = merged ? "mshr_with_demand" : "mshr_prefetch_only";
    r.ready = ready;
    emit(r);
    if (r.prefetchId) {
        const Line line{actor.name(), pkt->getBlockAddr(blockSize),
                        pkt->isSecure()};
        auto &state = cacheOccupancies.at(actor.name());
        const auto previous = residents.find(line);
        if (previous != residents.end()) {
            --state.prefetched;
            if (!previous->second.used) --state.unused;
        }
        residents[line] = {prefetch, r.prefetchId, ready, false};
        ++state.prefetched;
        ++state.unused;
        cacheOccupancy(actor.name());
    }
}

void
PrefetchTrace::use(const ClockedObject &actor, unsigned level, PacketPtr pkt,
                  const std::string &result)
{
    if (pkt->req->isPrefetch() || pkt->req->isInstFetch()) return;
    auto it = residents.find({actor.name(), pkt->getBlockAddr(blockSize),
                              pkt->isSecure()});
    if (it == residents.end() || it->second.used) return;
    it->second.used = true;
    --cacheOccupancies.at(actor.name()).unused;
    cacheOccupancy(actor.name());
    Record r{"PF_FIRST_USE", actor.name(), level, actor.clockPeriod(), pkt->req};
    r.prefetchId = it->second.id;
    r.op = pkt->isWrite() ? "store" : "load";
    r.ready = it->second.ready;
    r.result = result;
    emit(r);
}

void
PrefetchTrace::evict(const ClockedObject &actor, unsigned level, Addr line,
                    bool secure, const char *reason)
{
    auto it = residents.find({actor.name(), line, secure});
    if (it == residents.end()) return;
    Record r{"PF_EVICT", actor.name(), level, actor.clockPeriod(), it->second.req};
    r.prefetchId = it->second.id;
    r.targetAddress = true;
    r.pa = line;
    r.result = it->second.used ? "used" : "unused";
    r.reason = reason;
    emit(r);
    auto &state = cacheOccupancies.at(actor.name());
    --state.prefetched;
    if (!it->second.used) --state.unused;
    residents.erase(it);
    cacheOccupancy(actor.name());
}

void
PrefetchTrace::flush()
{
    *output->stream() << buffer.str();
    buffer.str("");
    buffer.clear();
    fatal_if(!*output->stream(), "Could not write prefetch trace");
}

void
PrefetchTrace::finish()
{
    if (!output) return;
    flush();
    // A terminal marker distinguishes a closed trace from a truncated file.
    *output->stream() << "# trace_end_tick=" << curTick()
                      << "\n# trace_complete=1\n";
    simout.close(output);
    output = nullptr;
}
} // namespace gem5

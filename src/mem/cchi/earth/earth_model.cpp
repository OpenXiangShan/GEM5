// Vendored from CHIron: cchi/cohestra/cohestra_earth/earth_model.cpp
//
// gem5 adaptations (each also marked "gem5 adaptation:" at the site):
//  - line data accesses go through the MemoryBackend hook (see earth_model.hpp)
//    instead of the old internal sparse 'memory' map
//  - the SimpleIni/ConfigurationEntry machinery (CONFIG_EARTH_* entries and
//    LoadConfiguration) is replaced by plain setters with identical defaults
//  - spdlog is replaced by the EARTH_* logging shim below; all format strings
//    are printf-style now

#include "earth_model.hpp"

#include <string>   // gem5 adaptation: std::to_string (was pulled in via spdlog)


// ---------------------------------------------------------------------------
// gem5 adaptation: logging shim (replaces spdlog).
//
// In the gem5 build the SConscript defines EARTH_HAVE_GEM5_DEBUG and the
// messages are routed to gem5's logging:
//   EARTH_ERROR / EARTH_WARN -> warn()      (gem5 has no non-fatal error level)
//   EARTH_INFO               -> inform()
//   EARTH_DEBUG              -> DPRINTFR(Earth, ...), the raw DPRINTF variant
//                               (EarthModel is not a SimObject and has no
//                               name(), which the plain DPRINTF requires)
// Without that define (e.g. standalone syntax checks, no gem5-generated
// headers available) a tiny fprintf fallback is used instead.
// ---------------------------------------------------------------------------
#ifdef EARTH_HAVE_GEM5_DEBUG

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Earth.hh"

#define EARTH_ERROR(...)    warn(__VA_ARGS__)
#define EARTH_WARN(...)     warn(__VA_ARGS__)
#define EARTH_INFO(...)     inform(__VA_ARGS__)
#define EARTH_DEBUG(...)    DPRINTFR(Earth, __VA_ARGS__)

#else

#include <cstdio>

#define EARTH_ERROR(...) \
    do { std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define EARTH_WARN(...) \
    do { std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define EARTH_INFO(...) \
    do { std::fprintf(stdout, __VA_ARGS__); std::fprintf(stdout, "\n"); } while (0)
#define EARTH_DEBUG(...) \
    do { std::fprintf(stdout, __VA_ARGS__); std::fprintf(stdout, "\n"); } while (0)

#endif


// gem5 adaptation: the CONFIG_EARTH_* ConfigurationEntryBack definitions of
// the original are removed; the same default values now appear directly in
// the constructor initializer list below and every parameter has a setter.

// Implementation of: EarthModel text decoders
namespace {

    const char* TextOfREQOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::REQ::StashShared:       return "StashShared";
        case CCHI::Opcodes::REQ::StashUnique:       return "StashUnique";
        case CCHI::Opcodes::REQ::ReadNoSnp:         return "ReadNoSnp";
        case CCHI::Opcodes::REQ::ReadOnce:          return "ReadOnce";
        case CCHI::Opcodes::REQ::ReadShared:        return "ReadShared";
        case CCHI::Opcodes::REQ::WriteNoSnpPtl:     return "WriteNoSnpPtl";
        case CCHI::Opcodes::REQ::WriteNoSnpFull:    return "WriteNoSnpFull";
        case CCHI::Opcodes::REQ::WriteUniquePtl:    return "WriteUniquePtl";
        case CCHI::Opcodes::REQ::WriteUniqueFull:   return "WriteUniqueFull";
        case CCHI::Opcodes::REQ::CleanShared:       return "CleanShared";
        case CCHI::Opcodes::REQ::CleanInvalid:      return "CleanInvalid";
        case CCHI::Opcodes::REQ::MakeInvalid:       return "MakeInvalid";
        case CCHI::Opcodes::REQ::ReadUnique:        return "ReadUnique";
        case CCHI::Opcodes::REQ::MakeUnique:        return "MakeUnique";
        case CCHI::Opcodes::REQ::EvictBack:         return "EvictBack";
        case CCHI::Opcodes::REQ::EvictClean:        return "EvictClean";
        case CCHI::Opcodes::REQ::AtomicSwap:        return "AtomicSwap";
        case CCHI::Opcodes::REQ::AtomicCompare:     return "AtomicCompare";
        default:
            if (CCHI::Opcodes::REQ::AtomicStore::Is(opcode))
                return "AtomicStore";

            if (CCHI::Opcodes::REQ::AtomicLoad::Is(opcode))
                return "AtomicLoad";

            return "Reserved";
        }
    }

    const char* TextOfEVTOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::EVT::Evict:             return "Evict";
        case CCHI::Opcodes::EVT::WriteBackFull:     return "WriteBackFull";
        default:                                    return "Reserved";
        }
    }

    const char* TextOfSNPOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::SNP::SnpMakeInvalid:    return "SnpMakeInvalid";
        case CCHI::Opcodes::SNP::SnpToInvalid:      return "SnpToInvalid";
        case CCHI::Opcodes::SNP::SnpToShared:       return "SnpToShared";
        case CCHI::Opcodes::SNP::SnpToClean:        return "SnpToClean";
        default:                                    return "Reserved";
        }
    }

    const char* TextOfUpRSPOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::UpRSP::CompAck:         return "CompAck";
        case CCHI::Opcodes::UpRSP::SnpResp:         return "SnpResp";
        default:                                    return "Reserved";
        }
    }

    const char* TextOfUpDATOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::UpDAT::NonCopyBackWrData:   return "NonCopyBackWrData";
        case CCHI::Opcodes::UpDAT::CopyBackWrData:      return "CopyBackWrData";
        case CCHI::Opcodes::UpDAT::SnpRespData:         return "SnpRespData";
        default:                                        return "Reserved";
        }
    }

    const char* TextOfDnRSPOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::DnRSP::CompStash:       return "CompStash";
        case CCHI::Opcodes::DnRSP::Comp:            return "Comp";
        case CCHI::Opcodes::DnRSP::DBIDResp:        return "DBIDResp";
        case CCHI::Opcodes::DnRSP::CompDBIDResp:    return "CompDBIDResp";
        case CCHI::Opcodes::DnRSP::CompCMO:         return "CompCMO";
        default:                                    return "Reserved";
        }
    }

    const char* TextOfDnDATOpcode(uint32_t opcode) noexcept
    {
        switch (opcode)
        {
        case CCHI::Opcodes::DnDAT::CompData:        return "CompData";
        default:                                    return "Reserved";
        }
    }

    const char* TextOfResp(uint32_t resp) noexcept
    {
        return CCHI::Resps::ToEnum(static_cast<CCHI::Resp>(resp))->name;
    }

    const char* TextOfLineState(size_t state) noexcept
    {
        switch (state)
        {
        case 0:     return "I";
        case 1:     return "S";
        case 2:     return "U";
        default:    return "?";
        }
    }

    const char* TextOfXactKind(size_t kind) noexcept
    {
        switch (kind)
        {
        case 0:     return "ReadShared";
        case 1:     return "ReadUnique";
        case 2:     return "MakeUnique";
        case 3:     return "Evict";
        case 4:     return "WriteBackFull";
        default:    return "?";
        }
    }

    const char* TextOfXactPhase(size_t phase) noexcept
    {
        switch (phase)
        {
        case 0:     return "Snoop";
        case 1:     return "Respond";
        case 2:     return "WaitCompAck";
        case 3:     return "WaitWrData";
        case 4:     return "WaitPop";
        default:    return "?";
        }
    }
}

// Implementation of: class EarthModel
namespace Cohestra {

    // gem5 adaptation: the defaults below are exactly the defaultValues of the
    // removed CONFIG_EARTH_* entries
    EarthModel::EarthModel(size_t portCount) noexcept
        : portCount           (portCount)
        , nodeID              (0x10)
        , parallelism         (8)
        , inflightSNP         (4)
        , queueDepthEVT       (2)
        , queueDepthREQ       (2)
        , queueDepthRSP       (8)
        , queueDepthDAT       (16)
        , latencyRSP          (4)
        , latencyDAT          (4)
        , memoryStart         (0x80000000)
        , memoryEnd           (0xA0000000)
        , verbose             (true)
        , verboseFlit         (true)
        , verboseXact         (true)
        , verboseState        (true)
        , verboseInternal     (true)
        , verboseBackpressure (false)
        , upstreamNodeIDs     ()
        , ports               (portCount)
        , tracker             (8)   // = parallelism
    {
        EARTH_INFO(
            "EarthModel: configured with node ID %u, parallelism %u, snoop inflight %u, "
            "queue depths EVT %u/REQ %u/RSP %u/DAT %u, latencies RSP %u/DAT %u, memory [%#llx, %#llx).",
            nodeID, parallelism, inflightSNP,
            queueDepthEVT, queueDepthREQ, queueDepthRSP, queueDepthDAT,
            latencyRSP, latencyDAT,
            static_cast<unsigned long long>(memoryStart),
            static_cast<unsigned long long>(memoryEnd));
    }

    // gem5 adaptation: plain setters replace LoadConfiguration(); the
    // validation rules are the same as before, invalid values are rejected
    // with an error log instead of failing the whole load
    void EarthModel::SetNodeID(uint32_t nodeID) noexcept
    {
        if (nodeID >= (1u << CommonFlitConfigurationType1::downstreamNodeIdWidth))
        {
            EARTH_ERROR("EarthModel: nodeid (%u) does not fit the downstream node ID width, ignored.",
                nodeID);
            return;
        }

        this->nodeID = nodeID;
    }

    void EarthModel::SetParallelism(uint32_t parallelism) noexcept
    {
        if (!parallelism)
        {
            EARTH_ERROR("EarthModel: parallelism must not be zero, ignored.");
            return;
        }

        this->parallelism = parallelism;

        tracker.clear();
        tracker.resize(parallelism);
    }

    void EarthModel::SetInflightSNP(uint32_t inflightSNP) noexcept
    {
        if (!inflightSNP)
        {
            EARTH_ERROR("EarthModel: inflight.snp must not be zero, ignored.");
            return;
        }

        this->inflightSNP = inflightSNP;
    }

    void EarthModel::SetQueueDepthEVT(uint32_t depth) noexcept
    {
        if (!depth)
        {
            EARTH_ERROR("EarthModel: queue depths must not be zero, ignored.");
            return;
        }

        queueDepthEVT = depth;
    }

    void EarthModel::SetQueueDepthREQ(uint32_t depth) noexcept
    {
        if (!depth)
        {
            EARTH_ERROR("EarthModel: queue depths must not be zero, ignored.");
            return;
        }

        queueDepthREQ = depth;
    }

    void EarthModel::SetQueueDepthRSP(uint32_t depth) noexcept
    {
        if (!depth)
        {
            EARTH_ERROR("EarthModel: queue depths must not be zero, ignored.");
            return;
        }

        queueDepthRSP = depth;
    }

    void EarthModel::SetQueueDepthDAT(uint32_t depth) noexcept
    {
        if (!depth)
        {
            EARTH_ERROR("EarthModel: queue depths must not be zero, ignored.");
            return;
        }

        queueDepthDAT = depth;
    }

    void EarthModel::SetLatencyRSP(uint32_t latency) noexcept
    {
        latencyRSP = latency;
    }

    void EarthModel::SetLatencyDAT(uint32_t latency) noexcept
    {
        latencyDAT = latency;
    }

    void EarthModel::SetMemoryWindow(uint64_t start, uint64_t end) noexcept
    {
        if (end <= start)
        {
            EARTH_ERROR("EarthModel: memory.end (%#llx) must be greater than memory.start (%#llx), ignored.",
                static_cast<unsigned long long>(end),
                static_cast<unsigned long long>(start));
            return;
        }

        memoryStart = start;
        memoryEnd   = end;
    }

    void EarthModel::SetVerbose(bool verbose) noexcept
    {
        this->verbose = verbose;

        // the total switch mutes every verbose category
        if (!verbose)
        {
            verboseFlit         = false;
            verboseXact         = false;
            verboseState        = false;
            verboseInternal     = false;
            verboseBackpressure = false;
        }
    }

    void EarthModel::SetVerboseFlit(bool verbose) noexcept
    {
        verboseFlit = verbose;
    }

    void EarthModel::SetVerboseXact(bool verbose) noexcept
    {
        verboseXact = verbose;
    }

    void EarthModel::SetVerboseState(bool verbose) noexcept
    {
        verboseState = verbose;
    }

    void EarthModel::SetVerboseInternal(bool verbose) noexcept
    {
        verboseInternal = verbose;
    }

    void EarthModel::SetVerboseBackpressure(bool verbose) noexcept
    {
        verboseBackpressure = verbose;
    }

    void EarthModel::SetMemoryBackend(std::shared_ptr<MemoryBackend> backend) noexcept
    {
        memoryBackend = std::move(backend);
    }

    void EarthModel::SetUpstreamNodeIDs(const std::vector<uint32_t>& nodeIDs) noexcept
    {
        upstreamNodeIDs = nodeIDs;
    }

    void EarthModel::Tick(uint64_t time) noexcept
    {
        this->time = time;

        // CCHI channel priority: EVT > SNP > REQ
        TickEVT();
        TickSNP();
        TickREQ();

        // response emission onto the DnRSP/DnDAT channels
        TickEmit();
    }

    void EarthModel::TickEVT() noexcept
    {
        // highest priority: admit and process pending EVT flits
        for (size_t port = 0; port < portCount; ++port)
        {
            Port& p = ports[port];

            if (p.evtFIFO.empty())
                continue;

            const FlitEVT& flit = p.evtFIFO.front().flit;
            uint64_t     line = flit.Addr >> 6;

            if (flit.Opcode != CCHI::Opcodes::EVT::Evict
             && flit.Opcode != CCHI::Opcodes::EVT::WriteBackFull)
            {
                ReportViolation("EVT with unsupported opcode, dropped", port);
                p.evtFIFO.pop_front();
                continue;
            }

            // one active eviction per line; an eviction may cut ahead of an
            // active REQ transaction on the same line (see class notes)
            if (lineEVT.find(line) != lineEVT.end())
                continue;

            std::optional<size_t> slot = AllocateSlot();

            if (!slot)
                continue;

            CheckAddressWindow(flit.Addr, "EVT", port);

            int dbid = -1;

            if (flit.Opcode == CCHI::Opcodes::EVT::WriteBackFull)
            {
                std::optional<int> id = AllocateID(port);

                if (!id)
                    continue;

                dbid = *id;
            }

            p.evtFIFO.pop_front();

            Xaction& x  = tracker[*slot];
            x           = Xaction{};
            x.active    = true;
            x.kind      = flit.Opcode == CCHI::Opcodes::EVT::Evict ? XactKind::Evict
                                                                   : XactKind::WriteBackFull;
            x.port      = port;
            x.line      = line;
            x.addr      = flit.Addr;
            x.txnID     = static_cast<uint8_t>(flit.TxnID);
            x.srcNodeID = static_cast<uint32_t>(flit.SrcID);
            x.traceTag  = static_cast<uint8_t>(flit.TraceTag);
            x.phase     = XactPhase::Respond;
            x.readyTime = time + latencyRSP;
            x.dbid      = dbid;

            lineEVT[line] = *slot;

            if (verboseXact)
                EARTH_DEBUG("EarthModel: EVT admitted on slot %zu (opcode %s, line %#llx, port %zu, txnID %u) at time %llu.",
                    *slot, TextOfEVTOpcode(static_cast<uint32_t>(flit.Opcode)),
                    static_cast<unsigned long long>(line << 6), port,
                    static_cast<uint32_t>(flit.TxnID), static_cast<unsigned long long>(time));

            if (x.kind == XactKind::Evict)
            {
                // clean eviction: the directory entry is dropped immediately
                DirRemove(port, line);
            }
            else
            {
                // write-back: the directory entry is dropped on data completion
                p.dbids[static_cast<uint8_t>(dbid)] = { *slot, DBIDKind::WriteBackData };
            }

            countEVT++;
        }
    }

    void EarthModel::TickSNP() noexcept
    {
        // middle priority: snoop channel progress, which covers consuming
        // snoop responses, write-back data and CompAcks, and issuing snoops

        // consume UpRSP flits, one per port per cycle
        for (size_t port = 0; port < portCount; ++port)
        {
            Port& p = ports[port];

            if (p.uprspFIFO.empty())
                continue;

            FlitUpRSP flit = p.uprspFIFO.front();
            p.uprspFIFO.pop_front();

            if (flit.Opcode == CCHI::Opcodes::UpRSP::SnpResp)
            {
                auto it = p.snoops.find(static_cast<uint8_t>(flit.TxnID));

                if (it == p.snoops.end())
                {
                    ReportViolation("SnpResp with unknown TxnID, dropped", port);
                    continue;
                }

                SnoopEntry& se = it->second;

                if (se.gotData)
                {
                    ReportViolation("SnpResp received after SnpRespData on the same TxnID", port);
                    p.snoops.erase(it);
                    FreeID(port, static_cast<uint8_t>(flit.TxnID));
                    continue;
                }

                se.gotResp = true;

                size_t slot = se.slot;

                Snoop& snoop    = tracker[slot].snoops[se.snoop];
                snoop.done      = true;
                snoop.respNotI  = flit.Resp != CCHI::Resps::I;

                if (verboseXact)
                    EARTH_DEBUG("EarthModel: SnpResp completed (resp %s, port %zu, txnID %u, for slot %zu) at time %llu.",
                        TextOfResp(static_cast<uint32_t>(flit.Resp)), port, static_cast<uint32_t>(flit.TxnID),
                        slot, static_cast<unsigned long long>(time));

                p.snoops.erase(it);
                FreeID(port, static_cast<uint8_t>(flit.TxnID));

                CheckSnoopsDone(slot);
            }
            else if (flit.Opcode == CCHI::Opcodes::UpRSP::CompAck)
            {
                auto it = p.dbids.find(static_cast<uint8_t>(flit.TxnID));

                if (it == p.dbids.end())
                {
                    ReportViolation("CompAck with unknown DBID, dropped", port);
                    continue;
                }

                DBIDEntry& de = it->second;

                if (de.kind != DBIDKind::CompAckWait)
                {
                    ReportViolation("CompAck colliding with a write-back DBID", port);
                    continue;
                }

                Xaction& x = tracker[de.slot];

                if (x.ackReceived)
                    ReportViolation("duplicated CompAck on the same DBID", port);

                x.ackReceived = true;

                if (verboseXact)
                    EARTH_DEBUG("EarthModel: CompAck received (port %zu, DBID %u, for slot %zu in phase %s) at time %llu.",
                        port, static_cast<uint32_t>(flit.TxnID), de.slot,
                        TextOfXactPhase(static_cast<size_t>(x.phase)), static_cast<unsigned long long>(time));

                if (x.phase == XactPhase::WaitCompAck)
                    Retire(de.slot);
            }
            else
                ReportViolation("UpRSP with unsupported opcode, dropped", port);
        }

        // consume UpDAT flits, one per port per cycle
        for (size_t port = 0; port < portCount; ++port)
        {
            Port& p = ports[port];

            if (p.updatFIFO.empty())
                continue;

            FlitUpDAT flit = p.updatFIFO.front();
            p.updatFIFO.pop_front();

            size_t dataID = static_cast<size_t>(flit.DataID);

            if (flit.Opcode == CCHI::Opcodes::UpDAT::SnpRespData)
            {
                auto it = p.snoops.find(static_cast<uint8_t>(flit.TxnID));

                if (it == p.snoops.end())
                {
                    ReportViolation("SnpRespData with unknown TxnID, dropped", port);
                    continue;
                }

                SnoopEntry& se = it->second;

                if (se.gotResp)
                {
                    ReportViolation("SnpRespData received after SnpResp on the same TxnID", port);
                    continue;
                }

                if (se.beatMask & (1u << dataID))
                {
                    ReportViolation("SnpRespData with duplicated DataID, dropped", port);
                    continue;
                }

                if (se.beatMask && se.resp != static_cast<uint8_t>(flit.Resp))
                    ReportViolation("SnpRespData with mismatched Resp across beats", port);

                se.gotData   = true;
                se.resp      = static_cast<uint8_t>(flit.Resp);
                se.beatMask |= (1u << dataID);

                if (CCHI::Resps::ToEnum(static_cast<CCHI::Resp>(flit.Resp))->IsPD())
                    MergeBeat(se.line, dataID, flit.Data, static_cast<uint32_t>(flit.BE));

                if (se.beatMask == ((1u << BEATS_PER_LINE) - 1))
                {
                    size_t slot = se.slot;

                    Snoop& snoop    = tracker[slot].snoops[se.snoop];
                    snoop.done      = true;
                    snoop.respNotI  = se.resp != CCHI::Resps::I;

                    if (verboseXact)
                        EARTH_DEBUG("EarthModel: SnpRespData completed (resp %s, port %zu, txnID %u, for slot %zu) at time %llu.",
                            TextOfResp(se.resp), port, static_cast<uint32_t>(flit.TxnID),
                            slot, static_cast<unsigned long long>(time));

                    p.snoops.erase(it);
                    FreeID(port, static_cast<uint8_t>(flit.TxnID));

                    CheckSnoopsDone(slot);
                }
            }
            else if (flit.Opcode == CCHI::Opcodes::UpDAT::CopyBackWrData)
            {
                auto it = p.dbids.find(static_cast<uint8_t>(flit.TxnID));

                if (it == p.dbids.end())
                {
                    ReportViolation("CopyBackWrData with unknown DBID, dropped", port);
                    continue;
                }

                DBIDEntry& de = it->second;

                if (de.kind != DBIDKind::WriteBackData)
                {
                    ReportViolation("CopyBackWrData colliding with a CompAck DBID", port);
                    continue;
                }

                Xaction& x = tracker[de.slot];

                if (x.wrBeatMask & (1u << dataID))
                {
                    ReportViolation("CopyBackWrData with duplicated DataID, dropped", port);
                    continue;
                }

                x.wrBeatMask |= (1u << dataID);

                MergeBeat(x.line, dataID, flit.Data, static_cast<uint32_t>(flit.BE));

                if (verboseState)
                    EARTH_DEBUG("EarthModel: CopyBackWrData beat merged (dataID %zu, BE %#x, port %zu, DBID %u, for slot %zu) at time %llu.",
                        dataID, static_cast<uint32_t>(flit.BE), port, static_cast<uint32_t>(flit.TxnID),
                        de.slot, static_cast<unsigned long long>(time));

                if (x.wrBeatMask == ((1u << BEATS_PER_LINE) - 1))
                {
                    // write-back data completed: memory is up to date and the
                    // directory ownership of the line is dropped
                    DirRemove(x.port, x.line);

                    Retire(de.slot);
                }
            }
            else
                ReportViolation("UpDAT with unsupported opcode, dropped", port);
        }

        // issue pending snoops, at most one per target port per cycle
        std::vector<bool> snpIssued(portCount, false);

        for (size_t slotIdx = 0; slotIdx < tracker.size(); ++slotIdx)
        {
            Xaction& x = tracker[slotIdx];

            if (!x.active || x.phase != XactPhase::Snoop)
                continue;

            if (!x.snoopPlanBuilt)
                BuildSnoopPlan(slotIdx);

            if (x.phase != XactPhase::Snoop)
                continue;

            for (size_t si = 0; si < x.snoops.size(); ++si)
            {
                Snoop& snoop = x.snoops[si];

                if (snoop.issued)
                    continue;

                Port& tp = ports[snoop.port];

                if (tp.snoops.size() >= inflightSNP)
                    continue;

                if (snpIssued[snoop.port])
                    continue;

                std::optional<int> id = AllocateID(snoop.port);

                if (!id)
                    continue;

                snoop.txnID  = *id;
                snoop.issued = true;

                uint32_t tgtNodeID = snoop.port < upstreamNodeIDs.size()
                                   ? upstreamNodeIDs[snoop.port] : static_cast<uint32_t>(snoop.port);

                FlitSNP flit;
                flit.TxnID    = static_cast<FlitSNP::txnid_t>(*id);
                flit.SrcID    = static_cast<FlitSNP::srcid_t>(nodeID);
                flit.TgtID    = static_cast<FlitSNP::tgtid_t>(tgtNodeID);
                flit.Opcode   = static_cast<FlitSNP::opcode_t>(snoop.opcode);
                flit.Addr     = static_cast<FlitSNP::addr_t>(x.line << 3);
                flit.NS       = 0;
                flit.TraceTag = 0;

                tp.snoops[static_cast<uint8_t>(*id)] = { slotIdx, si, x.line, 0, false, false, 0 };
                tp.snpQueue.push_back(flit);

                snpIssued[snoop.port] = true;

                if (verboseFlit)
                    EARTH_DEBUG("EarthModel: SNP issued (opcode %s, line %#llx, port %zu, txnID %d, for slot %zu) at time %llu.",
                        TextOfSNPOpcode(static_cast<uint32_t>(snoop.opcode)),
                        static_cast<unsigned long long>(x.line << 6), snoop.port, *id,
                        slotIdx, static_cast<unsigned long long>(time));

                countSNP++;
            }
        }
    }

    void EarthModel::TickREQ() noexcept
    {
        // lowest priority: admit pending REQ flits
        for (size_t port = 0; port < portCount; ++port)
        {
            Port& p = ports[port];

            if (p.reqFIFO.empty())
                continue;

            StampedFlit<FlitREQ>&   head = p.reqFIFO.front();
            const FlitREQ&          flit = head.flit;
            uint64_t                line = flit.Addr >> 6;

            if (flit.Opcode != CCHI::Opcodes::REQ::ReadShared
             && flit.Opcode != CCHI::Opcodes::REQ::ReadUnique
             && flit.Opcode != CCHI::Opcodes::REQ::MakeUnique)
            {
                ReportViolation("REQ with unsupported opcode, dropped", port);
                p.reqFIFO.pop_front();
                continue;
            }

            // same-line requests are serialized in arrival order
            bool lineBusy = lineREQ.find(line) != lineREQ.end()
                         || lineEVT.find(line) != lineEVT.end();

            auto wit = lineWaiters.find(line);

            bool waitersPending = wit != lineWaiters.end() && !wit->second.empty();
            bool headWaiter     = waitersPending
                               && wit->second.front().first == head.stamp
                               && wit->second.front().second == port;

            if (lineBusy || (waitersPending && !headWaiter))
            {
                if (!head.registered)
                {
                    lineWaiters[line].push_back({ head.stamp, port });
                    head.registered = true;

                    if (verboseInternal)
                        EARTH_DEBUG("EarthModel: REQ waiting registered (opcode %s, line %#llx, port %zu, stamp %llu) at time %llu.",
                            TextOfREQOpcode(static_cast<uint32_t>(flit.Opcode)),
                            static_cast<unsigned long long>(line << 6), port,
                            static_cast<unsigned long long>(head.stamp), static_cast<unsigned long long>(time));
                }

                continue;
            }

            std::optional<size_t> slot = AllocateSlot();

            if (!slot)
                continue;

            CheckAddressWindow(flit.Addr, "REQ", port);

            p.reqFIFO.pop_front();

            if (headWaiter)
            {
                wit->second.pop_front();

                if (wit->second.empty())
                    lineWaiters.erase(wit);
            }

            Xaction& x    = tracker[*slot];
            x             = Xaction{};
            x.active      = true;
            x.kind        = flit.Opcode == CCHI::Opcodes::REQ::ReadShared ? XactKind::ReadShared
                          : flit.Opcode == CCHI::Opcodes::REQ::ReadUnique ? XactKind::ReadUnique
                                                                          : XactKind::MakeUnique;
            x.port        = port;
            x.line        = line;
            x.addr        = flit.Addr;
            x.txnID       = static_cast<uint8_t>(flit.TxnID);
            x.srcNodeID   = static_cast<uint32_t>(flit.SrcID);
            x.expCompData = static_cast<uint8_t>(flit.ExpCompData) != 0;
            x.traceTag    = static_cast<uint8_t>(flit.TraceTag);
            x.phase       = XactPhase::Snoop;

            lineREQ[line] = *slot;

            if (verboseXact)
                EARTH_DEBUG("EarthModel: REQ admitted on slot %zu (opcode %s, line %#llx, port %zu, txnID %u, ExpCompData %u) at time %llu.",
                    *slot, TextOfREQOpcode(static_cast<uint32_t>(flit.Opcode)),
                    static_cast<unsigned long long>(line << 6), port,
                    static_cast<uint32_t>(flit.TxnID), static_cast<uint32_t>(flit.ExpCompData),
                    static_cast<unsigned long long>(time));

            countREQ++;
        }
    }

    void EarthModel::TickEmit() noexcept
    {
        // emit scheduled responses, at most one DnRSP and one DnDAT per port
        // per cycle (channel width)
        for (size_t port = 0; port < portCount; ++port)
        {
            Port& p = ports[port];

            bool emittedRSP = false;
            bool emittedDAT = false;

            for (size_t slotIdx = 0; slotIdx < tracker.size() && !(emittedRSP && emittedDAT); ++slotIdx)
            {
                Xaction& x = tracker[slotIdx];

                if (!x.active || x.port != port
                 || x.phase != XactPhase::Respond
                 || x.readyTime > time)
                    continue;

                switch (x.kind)
                {
                case XactKind::ReadShared:
                case XactKind::ReadUnique:

                    if (x.kind == XactKind::ReadShared || x.expCompData)
                    {
                        // CompData beats, both carrying the same allocated DBID
                        if (emittedDAT)
                            continue;

                        if (x.dbid < 0)
                        {
                            std::optional<int> id = AllocateID(port);

                            if (!id)
                                continue;

                            x.dbid = *id;
                            p.dbids[static_cast<uint8_t>(*id)] = { slotIdx, DBIDKind::CompAckWait };
                        }

                        // gem5 adaptation: line data is read from the attached
                        // MemoryBackend instead of the old internal sparse map
                        // ('memory[x.line]' in the original)
                        std::array<uint64_t, LINE_WORDS> lineData;
                        ReadLine(x.line, lineData);

                        FlitDnDAT flit;
                        flit.TxnID      = static_cast<FlitDnDAT::txnid_t>(x.txnID);
                        flit.SrcID      = static_cast<FlitDnDAT::srcid_t>(nodeID);
                        flit.TgtID      = static_cast<FlitDnDAT::tgtid_t>(x.srcNodeID);
                        flit.DBID       = static_cast<FlitDnDAT::dbid_t>(x.dbid);
                        flit.Opcode     = CCHI::Opcodes::DnDAT::CompData;
                        flit.RespErr    = 0;
                        flit.Resp       = x.kind == XactKind::ReadShared ? CCHI::Resps::SC
                                                                         : CCHI::Resps::UC;
                        flit.DataSource = 0;
                        flit.CBusy      = 0;
                        flit.WayValid   = 0;
                        flit.Way        = 0;
                        flit.DataID     = static_cast<FlitDnDAT::dataid_t>(x.compDataBeats);

                        for (size_t w = 0; w < WORDS_PER_BEAT; ++w)
                            flit.Data[w] = lineData[x.compDataBeats * WORDS_PER_BEAT + w];

                        flit.TraceTag   = static_cast<FlitDnDAT::tracetag_t>(x.traceTag);

                        p.dndatQueue.push_back(flit);

                        if (verboseFlit)
                            EARTH_DEBUG("EarthModel: %s emitted (dataID %zu, resp %s, port %zu, txnID %u, DBID %d, for slot %zu) at time %llu.",
                                TextOfDnDATOpcode(static_cast<uint32_t>(flit.Opcode)),
                                x.compDataBeats, TextOfResp(static_cast<uint32_t>(flit.Resp)), port,
                                static_cast<unsigned>(x.txnID), x.dbid, slotIdx,
                                static_cast<unsigned long long>(time));

                        x.compDataBeats++;
                        emittedDAT = true;

                        // every following CompData beat is scheduled on its own,
                        // so the upstream CompAck may legitimately arrive before
                        // or after any of them
                        if (x.compDataBeats == BEATS_PER_LINE)
                        {
                            DirGrant(slotIdx);

                            if (x.ackReceived)
                                Retire(slotIdx);
                            else
                                x.phase = XactPhase::WaitCompAck;
                        }
                        else
                            x.readyTime = time + latencyDAT;
                    }
                    else
                    {
                        // ReadUnique with data already held upstream: Comp only
                        if (emittedRSP)
                            continue;

                        if (x.dbid < 0)
                        {
                            std::optional<int> id = AllocateID(port);

                            if (!id)
                                continue;

                            x.dbid = *id;
                            p.dbids[static_cast<uint8_t>(*id)] = { slotIdx, DBIDKind::CompAckWait };
                        }

                        FlitDnRSP flit;
                        flit.TxnID    = static_cast<FlitDnRSP::txnid_t>(x.txnID);
                        flit.SrcID    = static_cast<FlitDnRSP::srcid_t>(nodeID);
                        flit.TgtID    = static_cast<FlitDnRSP::tgtid_t>(x.srcNodeID);
                        flit.DBID     = static_cast<FlitDnRSP::dbid_t>(x.dbid);
                        flit.Opcode   = CCHI::Opcodes::DnRSP::Comp;
                        flit.RespErr  = 0;
                        flit.Resp     = CCHI::Resps::UC;
                        flit.CBusy    = 0;
                        flit.WayValid = 0;
                        flit.Way      = 0;
                        flit.TraceTag = static_cast<FlitDnRSP::tracetag_t>(x.traceTag);

                        p.dnrspQueue.push_back(flit);

                        if (verboseFlit)
                            EARTH_DEBUG("EarthModel: %s emitted (%s, port %zu, txnID %u, DBID %d, for slot %zu) at time %llu.",
                                TextOfDnRSPOpcode(static_cast<uint32_t>(flit.Opcode)),
                                TextOfXactKind(static_cast<size_t>(x.kind)), port,
                                static_cast<unsigned>(x.txnID), x.dbid, slotIdx,
                                static_cast<unsigned long long>(time));

                        DirGrant(slotIdx);

                        emittedRSP = true;

                        if (x.ackReceived)
                            Retire(slotIdx);
                        else
                            x.phase = XactPhase::WaitCompAck;
                    }

                    break;

                case XactKind::MakeUnique:

                    if (emittedRSP)
                        continue;

                    if (x.dbid < 0)
                    {
                        std::optional<int> id = AllocateID(port);

                        if (!id)
                            continue;

                        x.dbid = *id;
                        p.dbids[static_cast<uint8_t>(*id)] = { slotIdx, DBIDKind::CompAckWait };
                    }

                    {
                        FlitDnRSP flit;
                        flit.TxnID    = static_cast<FlitDnRSP::txnid_t>(x.txnID);
                        flit.SrcID    = static_cast<FlitDnRSP::srcid_t>(nodeID);
                        flit.TgtID    = static_cast<FlitDnRSP::tgtid_t>(x.srcNodeID);
                        flit.DBID     = static_cast<FlitDnRSP::dbid_t>(x.dbid);
                        flit.Opcode   = CCHI::Opcodes::DnRSP::Comp;
                        flit.RespErr  = 0;
                        flit.Resp     = CCHI::Resps::UC;
                        flit.CBusy    = 0;
                        flit.WayValid = 0;
                        flit.Way      = 0;
                        flit.TraceTag = static_cast<FlitDnRSP::tracetag_t>(x.traceTag);

                        p.dnrspQueue.push_back(flit);
                    }

                    if (verboseFlit)
                        EARTH_DEBUG("EarthModel: %s emitted (%s, port %zu, txnID %u, DBID %d, for slot %zu) at time %llu.",
                            TextOfDnRSPOpcode(CCHI::Opcodes::DnRSP::Comp),
                            TextOfXactKind(static_cast<size_t>(x.kind)), port,
                            static_cast<unsigned>(x.txnID), x.dbid, slotIdx,
                            static_cast<unsigned long long>(time));

                    DirGrant(slotIdx);

                    emittedRSP = true;

                    if (x.ackReceived)
                        Retire(slotIdx);
                    else
                        x.phase = XactPhase::WaitCompAck;

                    break;

                case XactKind::Evict:

                    if (emittedRSP)
                        continue;

                    {
                        FlitDnRSP flit;
                        flit.TxnID    = static_cast<FlitDnRSP::txnid_t>(x.txnID);
                        flit.SrcID    = static_cast<FlitDnRSP::srcid_t>(nodeID);
                        flit.TgtID    = static_cast<FlitDnRSP::tgtid_t>(x.srcNodeID);
                        flit.DBID     = 0;
                        flit.Opcode   = CCHI::Opcodes::DnRSP::Comp;
                        flit.RespErr  = 0;
                        flit.Resp     = 0;
                        flit.CBusy    = 0;
                        flit.WayValid = 0;
                        flit.Way      = 0;
                        flit.TraceTag = static_cast<FlitDnRSP::tracetag_t>(x.traceTag);

                        p.dnrspQueue.push_back(flit);
                    }

                    if (verboseFlit)
                        EARTH_DEBUG("EarthModel: %s emitted (%s, port %zu, txnID %u, for slot %zu) at time %llu.",
                            TextOfDnRSPOpcode(CCHI::Opcodes::DnRSP::Comp),
                            TextOfXactKind(static_cast<size_t>(x.kind)), port,
                            static_cast<unsigned>(x.txnID), slotIdx,
                            static_cast<unsigned long long>(time));

                    emittedRSP = true;

                    // an Evict retires once its Comp is delivered downstream
                    x.phase = XactPhase::WaitPop;

                    break;

                case XactKind::WriteBackFull:

                    if (emittedRSP)
                        continue;

                    {
                        FlitDnRSP flit;
                        flit.TxnID    = static_cast<FlitDnRSP::txnid_t>(x.txnID);
                        flit.SrcID    = static_cast<FlitDnRSP::srcid_t>(nodeID);
                        flit.TgtID    = static_cast<FlitDnRSP::tgtid_t>(x.srcNodeID);
                        flit.DBID     = static_cast<FlitDnRSP::dbid_t>(x.dbid);
                        flit.Opcode   = CCHI::Opcodes::DnRSP::CompDBIDResp;
                        flit.RespErr  = 0;
                        flit.Resp     = 0;
                        flit.CBusy    = 0;
                        flit.WayValid = 0;
                        flit.Way      = 0;
                        flit.TraceTag = static_cast<FlitDnRSP::tracetag_t>(x.traceTag);

                        p.dnrspQueue.push_back(flit);
                    }

                    if (verboseFlit)
                        EARTH_DEBUG("EarthModel: %s emitted (%s, port %zu, txnID %u, DBID %d, for slot %zu) at time %llu.",
                            TextOfDnRSPOpcode(CCHI::Opcodes::DnRSP::CompDBIDResp),
                            TextOfXactKind(static_cast<size_t>(x.kind)), port,
                            static_cast<unsigned>(x.txnID), x.dbid, slotIdx,
                            static_cast<unsigned long long>(time));

                    emittedRSP = true;

                    x.phase = XactPhase::WaitWrData;

                    break;
                }
            }
        }
    }

    bool EarthModel::PushEVT(size_t port, const FlitEVT& flit) noexcept
    {
        if (port >= portCount)
        {
            ReportViolation("EVT pushed to a non-existent port, dropped", port);
            return false;
        }

        Port& p = ports[port];

        // concurrent upstream transactions per port are supported: Taurus nodes
        // drive unique TxnIDs, and responses are told apart by the DBIDs and
        // snoop TxnIDs this model allocates per transaction
        if (p.evtFIFO.size() >= queueDepthEVT)
        {
            ReportBackpressure("EVT denied: channel queue full", port);
            return false;
        }

        p.evtFIFO.push_back({ flit, stampCounter++, false });

        if (verboseFlit)
            EARTH_DEBUG("EarthModel: EVT accepted (opcode %s, addr %#llx, port %zu, txnID %u) at time %llu.",
                TextOfEVTOpcode(static_cast<uint32_t>(flit.Opcode)),
                static_cast<unsigned long long>(static_cast<uint64_t>(flit.Addr)), port,
                static_cast<uint32_t>(flit.TxnID), static_cast<unsigned long long>(time));

        return true;
    }

    bool EarthModel::PushREQ(size_t port, const FlitREQ& flit) noexcept
    {
        if (port >= portCount)
        {
            ReportViolation("REQ pushed to a non-existent port, dropped", port);
            return false;
        }

        Port& p = ports[port];

        // concurrent upstream transactions per port are supported: Taurus nodes
        // drive unique TxnIDs, and responses are told apart by the DBIDs and
        // snoop TxnIDs this model allocates per transaction
        if (p.reqFIFO.size() >= queueDepthREQ)
        {
            ReportBackpressure("REQ denied: channel queue full", port);
            return false;
        }

        p.reqFIFO.push_back({ flit, stampCounter++, false });

        if (verboseFlit)
            EARTH_DEBUG("EarthModel: REQ accepted (opcode %s, addr %#llx, port %zu, txnID %u, ExpCompData %u) at time %llu.",
                TextOfREQOpcode(static_cast<uint32_t>(flit.Opcode)),
                static_cast<unsigned long long>(static_cast<uint64_t>(flit.Addr)), port,
                static_cast<uint32_t>(flit.TxnID), static_cast<uint32_t>(flit.ExpCompData),
                static_cast<unsigned long long>(time));

        return true;
    }

    bool EarthModel::PushUpRSP(size_t port, const FlitUpRSP& flit) noexcept
    {
        if (port >= portCount)
        {
            ReportViolation("UpRSP pushed to a non-existent port, dropped", port);
            return false;
        }

        Port& p = ports[port];

        if (p.uprspFIFO.size() >= queueDepthRSP)
        {
            ReportBackpressure("UpRSP denied: channel queue full", port);
            return false;
        }

        p.uprspFIFO.push_back(flit);

        if (verboseFlit)
            EARTH_DEBUG("EarthModel: UpRSP accepted (opcode %s, resp %s, port %zu, txnID %u) at time %llu.",
                TextOfUpRSPOpcode(static_cast<uint32_t>(flit.Opcode)), TextOfResp(static_cast<uint32_t>(flit.Resp)),
                port, static_cast<uint32_t>(flit.TxnID), static_cast<unsigned long long>(time));

        return true;
    }

    bool EarthModel::PushUpDAT(size_t port, const FlitUpDAT& flit) noexcept
    {
        if (port >= portCount)
        {
            ReportViolation("UpDAT pushed to a non-existent port, dropped", port);
            return false;
        }

        Port& p = ports[port];

        if (p.updatFIFO.size() >= queueDepthDAT)
        {
            ReportBackpressure("UpDAT denied: channel queue full", port);
            return false;
        }

        p.updatFIFO.push_back(flit);

        if (verboseFlit)
            EARTH_DEBUG("EarthModel: UpDAT accepted (opcode %s, resp %s, dataID %u, port %zu, txnID %u) at time %llu.",
                TextOfUpDATOpcode(static_cast<uint32_t>(flit.Opcode)), TextOfResp(static_cast<uint32_t>(flit.Resp)),
                static_cast<uint32_t>(flit.DataID), port, static_cast<uint32_t>(flit.TxnID),
                static_cast<unsigned long long>(time));

        return true;
    }

    bool EarthModel::HasSNP(size_t port) const noexcept
    {
        return port < portCount && !ports[port].snpQueue.empty();
    }

    std::optional<EarthModel::FlitSNP> EarthModel::PeekSNP(size_t port) const noexcept
    {
        if (!HasSNP(port))
            return std::nullopt;

        return { ports[port].snpQueue.front() };
    }

    std::optional<EarthModel::FlitSNP> EarthModel::PopSNP(size_t port) noexcept
    {
        if (!HasSNP(port))
            return std::nullopt;

        FlitSNP flit = ports[port].snpQueue.front();
        ports[port].snpQueue.pop_front();

        return { flit };
    }

    bool EarthModel::HasDnRSP(size_t port) const noexcept
    {
        return port < portCount && !ports[port].dnrspQueue.empty();
    }

    std::optional<EarthModel::FlitDnRSP> EarthModel::PeekDnRSP(size_t port) const noexcept
    {
        if (!HasDnRSP(port))
            return std::nullopt;

        return { ports[port].dnrspQueue.front() };
    }

    std::optional<EarthModel::FlitDnRSP> EarthModel::PopDnRSP(size_t port) noexcept
    {
        if (!HasDnRSP(port))
            return std::nullopt;

        Port& p = ports[port];

        FlitDnRSP flit = p.dnrspQueue.front();
        p.dnrspQueue.pop_front();

        // an Evict transaction retires once its Comp is delivered downstream
        if (flit.Opcode == CCHI::Opcodes::DnRSP::Comp)
        {
            for (size_t slotIdx = 0; slotIdx < tracker.size(); ++slotIdx)
            {
                Xaction& x = tracker[slotIdx];

                if (x.active && x.phase == XactPhase::WaitPop
                 && x.port == port && x.txnID == static_cast<uint8_t>(flit.TxnID))
                {
                    Retire(slotIdx);
                    break;
                }
            }
        }

        return { flit };
    }

    bool EarthModel::HasDnDAT(size_t port) const noexcept
    {
        return port < portCount && !ports[port].dndatQueue.empty();
    }

    std::optional<EarthModel::FlitDnDAT> EarthModel::PeekDnDAT(size_t port) const noexcept
    {
        if (!HasDnDAT(port))
            return std::nullopt;

        return { ports[port].dndatQueue.front() };
    }

    std::optional<EarthModel::FlitDnDAT> EarthModel::PopDnDAT(size_t port) noexcept
    {
        if (!HasDnDAT(port))
            return std::nullopt;

        FlitDnDAT flit = ports[port].dndatQueue.front();
        ports[port].dndatQueue.pop_front();

        return { flit };
    }

    size_t EarthModel::GetPortCount() const noexcept
    {
        return portCount;
    }

    uint32_t EarthModel::GetNodeID() const noexcept
    {
        return nodeID;
    }

    bool EarthModel::IsIdle() const noexcept
    {
        for (const Port& p : ports)
        {
            if (!p.evtFIFO.empty()   || !p.reqFIFO.empty()
             || !p.uprspFIFO.empty() || !p.updatFIFO.empty()
             || !p.snpQueue.empty()  || !p.dnrspQueue.empty() || !p.dndatQueue.empty()
             || !p.snoops.empty()    || !p.dbids.empty())
                return false;
        }

        for (const Xaction& x : tracker)
        {
            if (x.active)
                return false;
        }

        return true;
    }

    uint64_t EarthModel::GetBackpressureDenialCount() const noexcept
    {
        return countDeniedBackpressure;
    }

    uint64_t EarthModel::GetProtocolDenialCount() const noexcept
    {
        return countDeniedProtocol;
    }

    uint64_t EarthModel::GetServicedREQCount() const noexcept
    {
        return countREQ;
    }

    uint64_t EarthModel::GetServicedEVTCount() const noexcept
    {
        return countEVT;
    }

    uint64_t EarthModel::GetServicedSNPCount() const noexcept
    {
        return countSNP;
    }

    std::optional<int> EarthModel::AllocateID(size_t port) noexcept
    {
        Port& p = ports[port];

        for (size_t i = 0; i < TXNID_SPACE; ++i)
        {
            if (!p.idUsed[i])
            {
                p.idUsed[i] = true;

                if (verboseInternal)
                    EARTH_DEBUG("EarthModel: ID %zu allocated on port %zu at time %llu.",
                        i, port, static_cast<unsigned long long>(time));

                return { static_cast<int>(i) };
            }
        }

        if (verboseInternal)
            EARTH_DEBUG("EarthModel: no free ID on port %zu at time %llu.",
                port, static_cast<unsigned long long>(time));

        return std::nullopt;
    }

    void EarthModel::FreeID(size_t port, int id) noexcept
    {
        ports[port].idUsed[static_cast<size_t>(id)] = false;

        if (verboseInternal)
            EARTH_DEBUG("EarthModel: ID %d freed on port %zu at time %llu.",
                id, port, static_cast<unsigned long long>(time));
    }

    std::optional<size_t> EarthModel::AllocateSlot() noexcept
    {
        for (size_t i = 0; i < tracker.size(); ++i)
        {
            if (!tracker[i].active)
                return { i };
        }

        return std::nullopt;
    }

    void EarthModel::BuildSnoopPlan(size_t slot) noexcept
    {
        Xaction& x = tracker[slot];

        x.snoopPlanBuilt = true;
        x.snoops.clear();

        DirectoryEntry dir = DirLookup(x.line);

        switch (x.kind)
        {
        case XactKind::ReadShared:

            // a unique owner is downgraded to shared, returning dirty data
            if (dir.state == LineState::U && dir.owner != x.port)
                x.snoops.push_back({ dir.owner, CCHI::Opcodes::SNP::SnpToShared, -1, false, false, false });

            break;

        case XactKind::ReadUnique:
        case XactKind::MakeUnique:

            // all other holders are invalidated; a unique owner may hold dirty
            // data and is snooped with a data-returning opcode
            if (dir.state == LineState::U && dir.owner != x.port)
            {
                x.snoops.push_back({ dir.owner, CCHI::Opcodes::SNP::SnpToInvalid, -1, false, false, false });
            }
            else if (dir.state == LineState::S)
            {
                for (size_t p = 0; p < portCount; ++p)
                {
                    if (p != x.port && (dir.sharers & (uint32_t(1) << p)))
                        x.snoops.push_back({ p, CCHI::Opcodes::SNP::SnpMakeInvalid, -1, false, false, false });
                }
            }

            break;

        default:
            break;
        }

        if (x.snoops.empty())
        {
            x.phase     = XactPhase::Respond;
            x.readyTime = time + (x.kind == XactKind::ReadShared || x.expCompData ? latencyDAT
                                                                                 : latencyRSP);
        }

        if (verboseInternal)
        {
            // gem5 adaptation: the target list string is built up front (the
            // original built it in a lambda inside the spdlog argument list)
            std::string targets;
            for (const Snoop& snoop : x.snoops)
            {
                targets += " (port " + std::to_string(snoop.port)
                         + ", opcode " + TextOfSNPOpcode(static_cast<uint32_t>(snoop.opcode)) + ")";
            }

            EARTH_DEBUG("EarthModel: snoop plan for slot %zu (kind %s, line %#llx, port %zu): %zu target(s)%s, directory state %s sharers %#x owner %zu.",
                slot, TextOfXactKind(static_cast<size_t>(x.kind)),
                static_cast<unsigned long long>(x.line << 6), x.port, x.snoops.size(),
                targets.c_str(),
                TextOfLineState(static_cast<size_t>(dir.state)), dir.sharers, dir.owner);
        }
    }

    bool EarthModel::CheckSnoopsDone(size_t slot) noexcept
    {
        Xaction& x = tracker[slot];

        if (!x.active || x.phase != XactPhase::Snoop)
            return false;

        for (const Snoop& snoop : x.snoops)
        {
            if (!snoop.done)
                return false;
        }

        x.phase     = XactPhase::Respond;
        x.readyTime = time + (x.kind == XactKind::ReadShared || x.expCompData ? latencyDAT
                                                                             : latencyRSP);

        return true;
    }

    void EarthModel::Retire(size_t slot) noexcept
    {
        Xaction& x = tracker[slot];

        if (verboseXact)
            EARTH_DEBUG("EarthModel: slot %zu retired (kind %s, line %#llx, port %zu) at time %llu.",
                slot, TextOfXactKind(static_cast<size_t>(x.kind)),
                static_cast<unsigned long long>(x.line << 6), x.port,
                static_cast<unsigned long long>(time));

        if (x.dbid >= 0)
        {
            ports[x.port].dbids.erase(static_cast<uint8_t>(x.dbid));
            FreeID(x.port, x.dbid);
            x.dbid = -1;
        }

        if (x.kind == XactKind::Evict || x.kind == XactKind::WriteBackFull)
            lineEVT.erase(x.line);
        else
            lineREQ.erase(x.line);

        x.active = false;
    }

    EarthModel::DirectoryEntry EarthModel::DirLookup(uint64_t line) const noexcept
    {
        auto it = directory.find(line);

        if (it == directory.end())
            return {};

        return it->second;
    }

    void EarthModel::DirRemove(size_t port, uint64_t line) noexcept
    {
        auto it = directory.find(line);

        if (it == directory.end())
        {
            if (verboseState)
                EARTH_DEBUG("EarthModel: directory drop of line %#llx for port %zu ignored, entry absent.",
                    static_cast<unsigned long long>(line << 6), port);
            return;
        }

        DirectoryEntry& dir = it->second;

        if (dir.state == LineState::U && dir.owner == port)
        {
            if (verboseState)
                EARTH_DEBUG("EarthModel: directory line %#llx: U(owner %zu) -> I at time %llu.",
                    static_cast<unsigned long long>(line << 6), dir.owner,
                    static_cast<unsigned long long>(time));

            directory.erase(it);
        }
        else if (dir.state == LineState::S)
        {
            dir.sharers &= ~(uint32_t(1) << port);

            if (verboseState)
                EARTH_DEBUG("EarthModel: directory line %#llx: sharer %zu dropped, sharers now %#x at time %llu.",
                    static_cast<unsigned long long>(line << 6), port, dir.sharers,
                    static_cast<unsigned long long>(time));

            if (!dir.sharers)
                directory.erase(it);
        }
        else if (verboseState)
            EARTH_DEBUG("EarthModel: directory drop of line %#llx for port %zu ignored, state %s owner %zu sharers %#x.",
                static_cast<unsigned long long>(line << 6), port,
                TextOfLineState(static_cast<size_t>(dir.state)), dir.owner, dir.sharers);
    }

    void EarthModel::DirGrant(size_t slot) noexcept
    {
        Xaction& x = tracker[slot];

        DirectoryEntry& dir = directory[x.line];

        if (x.kind == XactKind::ReadShared)
        {
            if (dir.state == LineState::U && dir.owner == x.port)
            {
                // defensive: the requester already owns the line
                if (verboseState)
                    EARTH_DEBUG("EarthModel: directory line %#llx: grant skipped, requester %zu already owns it.",
                        static_cast<unsigned long long>(x.line << 6), x.port);
                return;
            }

            if (dir.state == LineState::U)
            {
                // the previous owner was snooped SnpToShared and stays a
                // sharer, unless it answered I (e.g. it evicted in between)
                bool keepOwner = true;

                for (const Snoop& snoop : x.snoops)
                {
                    if (snoop.port == dir.owner
                     && snoop.opcode == CCHI::Opcodes::SNP::SnpToShared)
                    {
                        keepOwner = snoop.respNotI;
                    }
                }

                uint32_t sharers = uint32_t(1) << x.port;

                if (keepOwner)
                    sharers |= uint32_t(1) << dir.owner;

                if (verboseState)
                    EARTH_DEBUG("EarthModel: directory line %#llx: U(owner %zu) -> S(sharers %#x) at time %llu.",
                        static_cast<unsigned long long>(x.line << 6), dir.owner, sharers,
                        static_cast<unsigned long long>(time));

                dir.state   = LineState::S;
                dir.sharers = sharers;
            }
            else if (dir.state == LineState::S)
            {
                dir.sharers |= uint32_t(1) << x.port;

                if (verboseState)
                    EARTH_DEBUG("EarthModel: directory line %#llx: sharer %zu added, sharers now %#x at time %llu.",
                        static_cast<unsigned long long>(x.line << 6), x.port, dir.sharers,
                        static_cast<unsigned long long>(time));
            }
            else
            {
                dir.state   = LineState::S;
                dir.sharers = uint32_t(1) << x.port;

                if (verboseState)
                    EARTH_DEBUG("EarthModel: directory line %#llx: I -> S(sharers %#x) at time %llu.",
                        static_cast<unsigned long long>(x.line << 6), dir.sharers,
                        static_cast<unsigned long long>(time));
            }
        }
        else if (x.kind == XactKind::ReadUnique || x.kind == XactKind::MakeUnique)
        {
            if (verboseState)
                EARTH_DEBUG("EarthModel: directory line %#llx: (state %s, sharers %#x, owner %zu) -> U(owner %zu) at time %llu.",
                    static_cast<unsigned long long>(x.line << 6),
                    TextOfLineState(static_cast<size_t>(dir.state)), dir.sharers, dir.owner,
                    x.port, static_cast<unsigned long long>(time));

            dir.state   = LineState::U;
            dir.sharers = 0;
            dir.owner   = x.port;
        }
    }

    // gem5 adaptation: whole-line data access routed through the attached
    // MemoryBackend (gem5 memory); replaces the old internal sparse map.
    // Without a backend there is no storage: reads return zeros and writes
    // are dropped, and the misconfiguration is reported once.
    void EarthModel::ReadLine(uint64_t line, std::array<uint64_t, LINE_WORDS>& data) noexcept
    {
        if (memoryBackend)
        {
            memoryBackend->readLine(line, data);
            return;
        }

        if (!backendMissingWarned)
        {
            backendMissingWarned = true;
            EARTH_WARN("EarthModel: no MemoryBackend attached, reads return zeros and writes are dropped.");
        }

        data.fill(0);
    }

    void EarthModel::WriteLine(uint64_t line, const std::array<uint64_t, LINE_WORDS>& data) noexcept
    {
        if (memoryBackend)
        {
            memoryBackend->writeLine(line, data);
            return;
        }

        if (!backendMissingWarned)
        {
            backendMissingWarned = true;
            EARTH_WARN("EarthModel: no MemoryBackend attached, reads return zeros and writes are dropped.");
        }
    }

    void EarthModel::MergeBeat(uint64_t line, size_t dataID, const uint64_t* data, uint32_t BE) noexcept
    {
        // gem5 adaptation: the beat is merged into the backend copy of the
        // line (read-modify-write); the original merged in place into the
        // internal sparse map entry ('memory[line]')
        std::array<uint64_t, LINE_WORDS> lineData;
        ReadLine(line, lineData);

        // one BE bit per byte within the beat payload
        for (size_t w = 0; w < WORDS_PER_BEAT; ++w)
        {
            for (size_t b = 0; b < sizeof(uint64_t); ++b)
            {
                if (BE & (uint32_t(1) << (w * sizeof(uint64_t) + b)))
                {
                    uint64_t  mask = uint64_t(0xFF) << (b * 8);
                    uint64_t& dst  = lineData[dataID * WORDS_PER_BEAT + w];

                    dst = (dst & ~mask) | (data[w] & mask);
                }
            }
        }

        WriteLine(line, lineData);
    }

    void EarthModel::ReportBackpressure(const char* what, size_t port) noexcept
    {
        countDeniedBackpressure++;

        if (verboseBackpressure)
            EARTH_DEBUG("EarthModel: %s at time %llu (port %zu).",
                what, static_cast<unsigned long long>(time), port);
    }

    void EarthModel::ReportViolation(const char* what, size_t port) noexcept
    {
        countDeniedProtocol++;

        EARTH_WARN("EarthModel: %s at time %llu (port %zu).",
            what, static_cast<unsigned long long>(time), port);
    }

    bool EarthModel::CheckAddressWindow(uint64_t addr, const char* what, size_t port) noexcept
    {
        if (addr < memoryStart || addr >= memoryEnd)
        {
            EARTH_WARN("EarthModel: %s address %#llx outside the configured memory window [%#llx, %#llx) at time %llu (port %zu).",
                what, static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(memoryStart),
                static_cast<unsigned long long>(memoryEnd),
                static_cast<unsigned long long>(time), port);

            countDeniedProtocol++;
            return false;
        }

        return true;
    }

    void EarthModel::DumpState() const noexcept
    {
        EARTH_INFO("EarthModel state dump at time %llu:", static_cast<unsigned long long>(time));

        for (size_t slotIdx = 0; slotIdx < tracker.size(); ++slotIdx)
        {
            const Xaction& x = tracker[slotIdx];

            if (!x.active)
                continue;

            // gem5 adaptation: the done count is computed up front (the
            // original computed it in a lambda inside the spdlog argument list)
            size_t snoopsDone = 0;
            for (const Snoop& snoop : x.snoops)
                snoopsDone += snoop.done;

            EARTH_INFO(" - slot %zu: %s %s on line %#llx (port %zu), txnID %u, dbid %d, "
                "ack %s, CompData beats %zu, snoops %zu/%zu done, ready time %llu",
                slotIdx,
                TextOfXactKind(static_cast<size_t>(x.kind)),
                TextOfXactPhase(static_cast<size_t>(x.phase)),
                static_cast<unsigned long long>(x.line << 6), x.port,
                static_cast<unsigned>(x.txnID), x.dbid,
                x.ackReceived ? "true" : "false", x.compDataBeats,
                snoopsDone, x.snoops.size(),
                static_cast<unsigned long long>(x.readyTime));
        }

        for (size_t port = 0; port < portCount; ++port)
        {
            const Port& p = ports[port];

            EARTH_INFO(" - port %zu: FIFOs EVT %zu / REQ %zu / UpRSP %zu / UpDAT %zu, "
                "queues SNP %zu / DnRSP %zu / DnDAT %zu, snoops outstanding %zu, DBIDs outstanding %zu",
                port,
                p.evtFIFO.size(), p.reqFIFO.size(), p.uprspFIFO.size(), p.updatFIFO.size(),
                p.snpQueue.size(), p.dnrspQueue.size(), p.dndatQueue.size(),
                p.snoops.size(), p.dbids.size());
        }

        // gem5 adaptation: no local memory map anymore, so the dump reports
        // backend presence instead of the old 'memory.size()' line count
        EARTH_INFO(" - lines: %zu REQ-active, %zu EVT-active, %zu waiting, directory %zu entries, memory backend %s",
            lineREQ.size(), lineEVT.size(), lineWaiters.size(), directory.size(),
            memoryBackend ? "attached" : "absent");
    }
}

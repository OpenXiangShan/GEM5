#pragma once

// Vendored from CHIron: cchi/cohestra/cohestra_earth/earth_model.hpp
//
// gem5 adaptations (each also marked "gem5 adaptation:" at the site):
//  - the internal sparse memory map is replaced by the MemoryBackend hook,
//    so gem5's memory system (checkpoint restore, golden memory, ...) is the
//    backing store for line data; the directory stays internal to the model
//  - the SimpleIni/ConfigurationEntry machinery (CONFIG_EARTH_* entries and
//    LoadConfiguration) is replaced by plain setters; defaults are unchanged
//  - spdlog is removed; logging goes through the shim at the top of
//    earth_model.cpp (gem5 warn/inform/DPRINTF, or an fprintf fallback)
//  - CHIron includes are retargeted to -I<CHIRON_DIR>/cchi style paths

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// gem5 adaptation: retargeted CHIron includes (relative "../..." includes in
// the original), expecting -I<CHIRON_DIR>/cchi to be provided at build time
#include "cohestra/cohestra_interface.hpp"
#include "spec/cchi_protocol_encoding.hpp"


namespace Cohestra {

    // gem5 adaptation: pluggable backing store for line data.
    //
    // Earth no longer keeps its own sparse memory; every line-sized data
    // access goes through this interface so that gem5's memory system always
    // holds the real data.
    //
    // 'lineAddr' is the 64-byte line number, i.e. the physical byte address
    // shifted right by 6 (Earth's internal line key); implementations recover
    // the byte address as (lineAddr << 6).
    class MemoryBackend {
    public:
        virtual ~MemoryBackend() = default;

        virtual void    readLine(uint64_t lineAddr, std::array<uint64_t, 8>& data) = 0;
        virtual void    writeLine(uint64_t lineAddr, const std::array<uint64_t, 8>& data) = 0;
    };

    // EarthModel: a simple, pure-C++ downstream (home node + memory) CCHI model.
    //
    // One instance serves all Type 1 ports with a shared memory and a shared
    // directory, so cross-port (multi-agent) cache coherency is exercised.
    //
    // Coherency policy:
    //  - directory states per 64B line: I / S (clean, sharer bitmask) / U (possibly
    //    dirty, single owner); S lines are always clean, U lines are always
    //    snooped with data-returning opcodes before ownership transfers.
    //  - ReadShared      : SnpToShared to a unique owner, then CompData (SC).
    //  - ReadUnique      : SnpToInvalid to a unique owner or SnpMakeInvalid to
    //                      other sharers, then CompData (UC) or Comp, honoring
    //                      ExpCompData.
    //  - MakeUnique      : same invalidation flow, then Comp.
    //  - Evict           : directory drop, then Comp.
    //  - WriteBackFull   : CompDBIDResp, then CopyBackWrData beats are merged
    //                      into memory.
    //
    // Channel priority: EVT > SNP > REQ, applied to the per-cycle processing
    // order (TickEVT -> TickSNP -> TickREQ). EVT admissions are additionally
    // allowed to cut ahead of an active REQ transaction on the same line,
    // because Taurus upstream nodes hold snoop responses until their in-flight
    // eviction is completed; completing the eviction first is always coherent
    // and avoids the request/snoop/eviction circular wait.
    //
    // Transactional parallelism is configurable (home transaction tracker size,
    // per-port snoop inflight limit, per-channel queue depths, response
    // latencies). One upstream-initiated transaction (REQ or EVT) is accepted
    // per port at a time: Taurus nodes always drive TxnID 0, so a second
    // concurrent one could not be told apart on the response channels. Excess
    // pushes are denied (backpressured) and reported.
    //
    // Denials are always reported: flow-control (backpressure) denials are
    // counted and logged at debug level, protocol violations are counted and
    // logged at warning/error level; both are readable through counters.
    class EarthModel {
    public:
        using FlitEVT   = CCHI::Flits::EVT<CommonFlitConfigurationType1>;
        using FlitREQ   = CCHI::Flits::REQ<CommonFlitConfigurationType1>;
        using FlitSNP   = CCHI::Flits::SNP<CommonFlitConfigurationType1>;
        using FlitUpRSP = CCHI::Flits::UpRSP<CommonFlitConfigurationType1>;
        using FlitUpDAT = CCHI::Flits::UpDAT<CommonFlitConfigurationType1>;
        using FlitDnRSP = CCHI::Flits::DnRSP<CommonFlitConfigurationType1>;
        using FlitDnDAT = CCHI::Flits::DnDAT<CommonFlitConfigurationType1>;

        static constexpr size_t     LINE_SIZE           = 64;
        static constexpr size_t     LINE_WORDS          = LINE_SIZE / sizeof(uint64_t);
        static constexpr size_t     BEATS_PER_LINE      = 512 / CommonFlitConfigurationType1::dataWidth;
        static constexpr size_t     WORDS_PER_BEAT      = LINE_WORDS / BEATS_PER_LINE;
        static constexpr size_t     TXNID_SPACE         = size_t(1) << CommonFlitConfigurationType1::dbIdWidth;

    protected:
        enum class LineState {
            I = 0,
            S,
            U
        };

        struct DirectoryEntry {
            LineState   state           = LineState::I;
            uint32_t    sharers         = 0;    // port bitmask, valid when S
            size_t      owner           = 0;    // port index, valid when U
        };

        enum class XactKind {
            ReadShared = 0,
            ReadUnique,
            MakeUnique,
            Evict,
            WriteBackFull
        };

        enum class XactPhase {
            Snoop = 0,      // issuing snoops and/or waiting for snoop responses
            Respond,        // waiting for readyTime to emit Comp/CompData/CompDBIDResp
            WaitCompAck,    // waiting for CompAck of the emitted DBID
            WaitWrData,     // waiting for CopyBackWrData beats of the emitted DBID
            WaitPop         // waiting for the Evict Comp to be popped downstream
        };

        struct Snoop {
            size_t      port            = 0;
            uint8_t     opcode          = 0;
            int         txnID           = -1;
            bool        issued          = false;
            bool        done            = false;
            bool        respNotI        = false;
        };

        struct Xaction {
            bool                        active          = false;
            XactKind                    kind            = XactKind::ReadShared;
            size_t                      port            = 0;
            uint64_t                    line            = 0;      // PA >> 6
            uint64_t                    addr            = 0;
            uint8_t                     txnID           = 0;
            uint32_t                    srcNodeID       = 0;
            bool                        expCompData     = false;
            uint8_t                     traceTag        = 0;

            XactPhase                   phase           = XactPhase::Snoop;
            uint64_t                    readyTime       = 0;

            int                         dbid            = -1;
            bool                        ackReceived     = false;
            size_t                      compDataBeats   = 0;

            bool                        snoopPlanBuilt  = false;
            std::vector<Snoop>          snoops;

            uint32_t                    wrBeatMask      = 0;
        };

        enum class DBIDKind {
            CompAckWait = 0,
            WriteBackData
        };

        struct DBIDEntry {
            size_t      slot;
            DBIDKind    kind;
        };

        struct SnoopEntry {
            size_t      slot;
            size_t      snoop;
            uint64_t    line;
            uint32_t    beatMask        = 0;
            bool        gotData         = false;
            bool        gotResp         = false;
            uint8_t     resp            = 0;
        };

        template<class TFlit>
        struct StampedFlit {
            TFlit       flit;
            uint64_t    stamp;
            bool        registered;
        };

        struct Port {
            std::deque<StampedFlit<FlitEVT>>    evtFIFO;
            std::deque<StampedFlit<FlitREQ>>    reqFIFO;
            std::deque<FlitUpRSP>               uprspFIFO;
            std::deque<FlitUpDAT>               updatFIFO;

            std::deque<FlitSNP>                 snpQueue;
            std::deque<FlitDnRSP>               dnrspQueue;
            std::deque<FlitDnDAT>               dndatQueue;

            // one allocator per port shared by snoop TxnIDs and DBIDs, so an
            // UpRSP/UpDAT flit always resolves to exactly one transaction
            std::array<bool, TXNID_SPACE>       idUsed          = {};

            std::unordered_map<uint8_t, SnoopEntry>
                                                snoops;
            std::unordered_map<uint8_t, DBIDEntry>
                                                dbids;
        };

    protected:
        const size_t            portCount;

        uint64_t                time                = 0;

        uint32_t                nodeID;
        uint32_t                parallelism;
        uint32_t                inflightSNP;

        uint32_t                queueDepthEVT;
        uint32_t                queueDepthREQ;
        uint32_t                queueDepthRSP;
        uint32_t                queueDepthDAT;

        uint32_t                latencyRSP;
        uint32_t                latencyDAT;

        uint64_t                memoryStart;
        uint64_t                memoryEnd;

        // verbose switches: 'verbose' is the total switch of all categories
        bool                    verbose;
        bool                    verboseFlit;
        bool                    verboseXact;
        bool                    verboseState;
        bool                    verboseInternal;
        bool                    verboseBackpressure;

        std::vector<uint32_t>   upstreamNodeIDs;

        std::vector<Port>       ports;

        std::vector<Xaction>    tracker;

        // gem5 adaptation: backing store for line data, attached through
        // SetMemoryBackend(); replaces the old internal sparse map
        // std::unordered_map<uint64_t, std::array<uint64_t, LINE_WORDS>> memory
        std::shared_ptr<MemoryBackend>
                                memoryBackend;

        bool                    backendMissingWarned  = false;

        std::unordered_map<uint64_t, DirectoryEntry>
                                directory;

        std::unordered_map<uint64_t, size_t>
                                lineREQ;

        std::unordered_map<uint64_t, size_t>
                                lineEVT;

        std::unordered_map<uint64_t, std::deque<std::pair<uint64_t, size_t>>>
                                lineWaiters;

        uint64_t                stampCounter        = 0;

        uint64_t                countDeniedBackpressure = 0;
        uint64_t                countDeniedProtocol     = 0;

        uint64_t                countREQ            = 0;
        uint64_t                countEVT            = 0;
        uint64_t                countSNP            = 0;

    public:
        EarthModel(size_t portCount) noexcept;

        virtual ~EarthModel() noexcept = default;

    public:
        // gem5 adaptation: plain parameter setters replacing the SimpleIni
        // LoadConfiguration() flow; defaults match the old CONFIG_EARTH_*
        // defaultValues and invalid values are rejected with an error log
        void                                SetNodeID(uint32_t nodeID) noexcept;
        void                                SetParallelism(uint32_t parallelism) noexcept;
        void                                SetInflightSNP(uint32_t inflightSNP) noexcept;

        void                                SetQueueDepthEVT(uint32_t depth) noexcept;
        void                                SetQueueDepthREQ(uint32_t depth) noexcept;
        void                                SetQueueDepthRSP(uint32_t depth) noexcept;
        void                                SetQueueDepthDAT(uint32_t depth) noexcept;

        void                                SetLatencyRSP(uint32_t latency) noexcept;
        void                                SetLatencyDAT(uint32_t latency) noexcept;

        void                                SetMemoryWindow(uint64_t start, uint64_t end) noexcept;

        void                                SetVerbose(bool verbose) noexcept;
        void                                SetVerboseFlit(bool verbose) noexcept;
        void                                SetVerboseXact(bool verbose) noexcept;
        void                                SetVerboseState(bool verbose) noexcept;
        void                                SetVerboseInternal(bool verbose) noexcept;
        void                                SetVerboseBackpressure(bool verbose) noexcept;

        // gem5 adaptation: attach the backing store (gem5 memory) for line data
        void                                SetMemoryBackend(std::shared_ptr<MemoryBackend> backend) noexcept;

        void                                SetUpstreamNodeIDs(const std::vector<uint32_t>& nodeIDs) noexcept;

    public:
        void                                Tick(uint64_t time) noexcept;

    protected:
        void                                TickEVT() noexcept;
        void                                TickSNP() noexcept;
        void                                TickREQ() noexcept;
        void                                TickEmit() noexcept;

    public:
        bool                                PushEVT(size_t port, const FlitEVT& flit) noexcept;
        bool                                PushREQ(size_t port, const FlitREQ& flit) noexcept;
        bool                                PushUpRSP(size_t port, const FlitUpRSP& flit) noexcept;
        bool                                PushUpDAT(size_t port, const FlitUpDAT& flit) noexcept;

        bool                                HasSNP(size_t port) const noexcept;
        std::optional<FlitSNP>              PeekSNP(size_t port) const noexcept;
        std::optional<FlitSNP>              PopSNP(size_t port) noexcept;

        bool                                HasDnRSP(size_t port) const noexcept;
        std::optional<FlitDnRSP>            PeekDnRSP(size_t port) const noexcept;
        std::optional<FlitDnRSP>            PopDnRSP(size_t port) noexcept;

        bool                                HasDnDAT(size_t port) const noexcept;
        std::optional<FlitDnDAT>            PeekDnDAT(size_t port) const noexcept;
        std::optional<FlitDnDAT>            PopDnDAT(size_t port) noexcept;

    public:
        size_t                              GetPortCount() const noexcept;
        uint32_t                            GetNodeID() const noexcept;

        bool                                IsIdle() const noexcept;

        uint64_t                            GetBackpressureDenialCount() const noexcept;
        uint64_t                            GetProtocolDenialCount() const noexcept;

        uint64_t                            GetServicedREQCount() const noexcept;
        uint64_t                            GetServicedEVTCount() const noexcept;
        uint64_t                            GetServicedSNPCount() const noexcept;

        void                                DumpState() const noexcept;

    protected:
        std::optional<int>                  AllocateID(size_t port) noexcept;
        void                                FreeID(size_t port, int id) noexcept;

        std::optional<size_t>               AllocateSlot() noexcept;

        void                                BuildSnoopPlan(size_t slot) noexcept;
        bool                                CheckSnoopsDone(size_t slot) noexcept;

        void                                Retire(size_t slot) noexcept;

        DirectoryEntry                      DirLookup(uint64_t line) const noexcept;
        void                                DirRemove(size_t port, uint64_t line) noexcept;
        void                                DirGrant(size_t slot) noexcept;

        // gem5 adaptation: line data access routed through the MemoryBackend;
        // without a backend, reads return zeros and writes are dropped (a
        // warning is emitted once)
        void                                ReadLine(uint64_t line, std::array<uint64_t, LINE_WORDS>& data) noexcept;
        void                                WriteLine(uint64_t line, const std::array<uint64_t, LINE_WORDS>& data) noexcept;

        void                                MergeBeat(uint64_t line, size_t dataID, const uint64_t* data, uint32_t BE) noexcept;

        void                                ReportBackpressure(const char* what, size_t port) noexcept;
        void                                ReportViolation(const char* what, size_t port) noexcept;

        bool                                CheckAddressWindow(uint64_t addr, const char* what, size_t port) noexcept;
    };
}

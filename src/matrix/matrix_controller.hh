/*
 * XSAI CUTE-aligned matrix controller scaffold.
 */

#ifndef __MATRIX_MATRIX_CONTROLLER_HH__
#define __MATRIX_MATRIX_CONTROLLER_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "sim/faults.hh"
#include "sim/serialize.hh"

namespace gem5
{

class ExecContext;

namespace matrix
{

class MatrixController
{
  public:
    static constexpr uint32_t TokenCount = 32;
    static constexpr uint32_t MatrixRegCount = 4;
    static constexpr uint32_t DecodedQueueDepth = 8;
    static constexpr uint32_t MicroTaskFifoSlots = 4;
    static constexpr uint32_t LocalMmuSourceCount = 64;
    static constexpr uint32_t OutsideDataWidthBits = 512;
    static constexpr uint32_t OutsideDataWidthBytes =
        OutsideDataWidthBits / 8;
    static constexpr uint32_t ReduceWidthBytes = 64;
    static constexpr uint32_t ResultWidthBytes = 4;
    static constexpr uint32_t MatrixMN = 4;

    static constexpr uint32_t MatrixMaxM = 128;
    static constexpr uint32_t MatrixMaxK = 64;
    static constexpr uint32_t MatrixMaxN = 128;
    static constexpr uint32_t MatrixABRegBytes = MatrixMaxM * MatrixMaxK;
    static constexpr uint32_t MatrixAccElems = MatrixMaxM * MatrixMaxN;

    static constexpr uint8_t DefaultAReg = 0;
    static constexpr uint8_t DefaultBReg = 1;
    static constexpr uint8_t DefaultAccReg = 0;

    enum class TaskOp : uint8_t
    {
        Mma,
        LoadStore,
        Release,
        Arith,
    };

    enum class FuType : uint8_t
    {
        None = 0,
        AML,
        BML,
        CMLLoad,
        CMLStore,
        Compute,
        Num
    };

    enum class MemPort : uint8_t
    {
        A = 0,
        B,
        CLoad,
        CStore,
        BScale,
        AScale,
        Num
    };

    static_assert(static_cast<uint8_t>(MemPort::A) == 0 &&
        static_cast<uint8_t>(MemPort::B) == 1 &&
        static_cast<uint8_t>(MemPort::CLoad) == 2 &&
        static_cast<uint8_t>(MemPort::CStore) == 3 &&
        static_cast<uint8_t>(MemPort::BScale) == 4 &&
        static_cast<uint8_t>(MemPort::AScale) == 5,
        "matrix MemPort order must match RTL LocalMMUTaskType encoding");

    enum class TaskEvent : uint8_t
    {
        LoadAllocate = 0,
        LoadIssue,
        LoadFinish,
        ComputeIssue,
        ComputeReadFinish,
        ComputeWriteFinish,
        StoreIssue,
        StoreFinish,
        ReleaseIssue,
        MemoryReadDataResponse,
        MemoryWriteAckResponse,
        Num
    };

    enum class ElemWidth : uint8_t
    {
        E8 = 1,
        E16 = 2,
        E32 = 4,
        E64 = 8,
    };

    static constexpr size_t FuTypeCount = static_cast<size_t>(FuType::Num);
    static constexpr size_t MemPortCount = static_cast<size_t>(MemPort::Num);
    static constexpr size_t TaskEventCount =
        static_cast<size_t>(TaskEvent::Num);

    static_assert((DecodedQueueDepth & (DecodedQueueDepth - 1)) == 0,
        "matrix decoded FIFO depth must be a power of two");
    static_assert((MicroTaskFifoSlots & (MicroTaskFifoSlots - 1)) == 0,
        "matrix micro-task FIFO slots must be a power of two");
    static_assert((LocalMmuSourceCount & (LocalMmuSourceCount - 1)) == 0,
        "matrix LocalMMU source count must be a power of two");
    static_assert((OutsideDataWidthBytes & (OutsideDataWidthBytes - 1)) == 0,
        "matrix outside data width must be a power of two");
    static_assert((MatrixMN & (MatrixMN - 1)) == 0,
        "matrix Matrix_MN must be a power of two");

    struct Stats : public statistics::Group
    {
        statistics::Scalar tasksAccepted;
        statistics::Scalar tasksIssued;
        statistics::Scalar tasksCompleted;
        statistics::Scalar tasksAborted;

        statistics::Scalar aPortTasks;
        statistics::Scalar bPortTasks;
        statistics::Scalar cLoadTasks;
        statistics::Scalar cStoreTasks;
        statistics::Scalar mmaTasks;
        statistics::Scalar zeroTasks;
        statistics::Scalar releaseTasks;

        statistics::Scalar memoryRequests;
        statistics::Scalar memoryReadRequests;
        statistics::Scalar memoryWriteRequests;
        statistics::Scalar memoryBytes;
        statistics::Scalar memoryReadBytes;
        statistics::Scalar memoryWriteBytes;
        statistics::Scalar memoryBusBytes;
        statistics::Scalar memoryReadBusBytes;
        statistics::Scalar memoryWriteBusBytes;
        statistics::Scalar localMmuSourceAllocations;
        statistics::Scalar localMmuSourceReleases;
        statistics::Scalar localMmuArbitrations;
        statistics::Scalar localMmuReadDataResponses;
        statistics::Scalar localMmuWriteAcks;
        statistics::Scalar localMmuMaxOutstanding;
        statistics::Scalar memoryPipelineRequests;
        statistics::Scalar memoryPipelineReadResponses;
        statistics::Scalar memoryPipelineWriteAcks;
        statistics::Scalar memoryPipelineSourceStallTicks;
        statistics::Scalar memoryPipelineRequestQueueTicks;
        statistics::Scalar memoryPipelineResponseQueueTicks;
        statistics::Scalar memoryPipelineLastRequestTick;
        statistics::Scalar memoryPipelineLastResponseTick;
        statistics::Scalar memoryPipelineMaxOutstanding;
        statistics::Scalar timingTasks;
        statistics::Scalar timingQueueTicks;
        statistics::Scalar timingMaxQueueTicks;
        statistics::Scalar timingBusyTicks;
        statistics::Scalar timingLastIssueTick;
        statistics::Scalar timingLastCompletionTick;
        statistics::Scalar acquireStallEvents;
        statistics::Scalar acquireStallTicks;
        statistics::Scalar tokenReleaseEvents;
        statistics::Scalar tokenReleaseDelayEvents;
        statistics::Scalar tokenReleaseDelayTicks;
        std::array<uint64_t, MemPortCount> memPortRequests = {};
        std::array<uint64_t, MemPortCount> memPortReadDataResponses = {};
        std::array<uint64_t, MemPortCount> memPortWriteAcks = {};
        std::array<uint64_t, MemPortCount> localMmuPortSelections = {};
        std::array<uint64_t, TaskEventCount> taskEvents = {};

        Stats(statistics::Group *parent);
        void resetStats() override;
    };

    struct TimingConfig
    {
        uint32_t issueIntervalCycles = 1;
        uint32_t loadBaseCycles = 4;
        uint32_t storeBaseCycles = 4;
        uint32_t zeroCycles = 1;
        uint32_t computeBaseCycles = 2;
        uint32_t computeReadCycles = 1;
        uint32_t releaseCycles = 1;
        uint32_t localMmuIssuePerCycle = 1;
        uint32_t localMmuArbCycles = 1;
        uint32_t l2RequestPipelineCycles = 1;
        uint32_t l2ResponsePipelineCycles = 1;
        uint32_t localMmuReadLatencyCycles = 20;
        uint32_t localMmuWriteAckLatencyCycles = 12;
    };

    struct ControlSnapshot
    {
        uint8_t decodedQueueHead = 0;
        uint8_t decodedQueueSize = 0;
        uint8_t nextLoadFifoIdx = 0;
        uint8_t nextComputeFifoIdx = 0;
        uint8_t nextStoreFifoIdx = 0;

        uint8_t fuBusyMask = 0;
        uint8_t abBusyMask = 0;
        uint8_t cBusyMask = 0;
        uint8_t abPendingReaderMask = 0;
        uint8_t cPendingReaderMask = 0;
        std::array<uint8_t, MatrixRegCount> abPendingReaders = {};
        std::array<uint8_t, MatrixRegCount> cPendingReaders = {};

        bool pendingStore = false;
        uint8_t firstMmuRequestIndex = 0;
        uint8_t nextLocalMmuSource = 0;
        uint8_t localMmuOutstanding = 0;
        uint64_t localMmuBusySourceMask = 0;

        Tick timingNextIssueTick = 0;
        Tick timingPendingStoreReadyTick = 0;
        Tick timingLastIssueTick = 0;
        Tick timingLastCompletionTick = 0;
        Tick timingLocalMmuIssueSlotTick = 0;
        Tick timingLocalMmuResponseSlotTick = 0;
        Tick timingLocalMmuLastRequestTick = 0;
        Tick timingLocalMmuLastResponseTick = 0;
        uint8_t timingLocalMmuOutstanding = 0;
        uint16_t pendingTokenEvents = 0;
    };

    MatrixController(statistics::Group *parent);

    void reset();
    void setTimingConfig(const TimingConfig &config);
    void retireReadyTokensUpTo(Tick now);
    bool tokenTargetReached(uint64_t token_idx, uint64_t target) const;
    Tick tokenReadyTick(uint64_t token_idx, uint64_t target) const;
    void recordAcquireStall(Tick ticks);

    void serialize(CheckpointOut &cp) const;
    void unserialize(CheckpointIn &cp);

    void syncReset(uint64_t token_idx);
    void release(ExecContext *xc, uint64_t token_idx);
    void acquire(ExecContext *xc, uint64_t token_idx, uint64_t target);

    void setTileM(uint64_t value);
    void setTileK(uint64_t value);
    void setTileN(uint64_t value);
    uint32_t getTileM() const { return data.tileM; }
    uint32_t getTileK() const { return data.tileK; }
    uint32_t getTileN() const { return data.tileN; }

    Fault load(ExecContext *xc, MemPort port, Addr base, Addr stride,
        uint32_t rows, uint32_t cols, ElemWidth width, bool transpose,
        uint64_t reg_idx);
    Fault store(ExecContext *xc, MemPort port, Addr base, Addr stride,
        uint32_t rows, uint32_t cols, ElemWidth width, bool transpose,
        uint64_t reg_idx);
    Fault loadA8(ExecContext *xc, Addr base, Addr stride,
        uint64_t reg_idx = DefaultAReg);
    Fault loadB8(ExecContext *xc, Addr base, Addr stride,
        uint64_t reg_idx = DefaultBReg);
    Fault loadC32(ExecContext *xc, Addr base, Addr stride,
        uint64_t acc_idx = DefaultAccReg);
    Fault storeC32(ExecContext *xc, Addr base, Addr stride,
        uint64_t acc_idx = DefaultAccReg);
    void zeroAcc(ExecContext *xc, uint64_t acc_idx = DefaultAccReg);
    void zero(ExecContext *xc, uint64_t reg_idx, bool is_acc);
    void mmaccWB(uint64_t src_a_idx = DefaultAReg,
        uint64_t src_b_idx = DefaultBReg,
        uint64_t dst_acc_idx = DefaultAccReg);
    void mmaccWB(ExecContext *xc, uint64_t src_a_idx = DefaultAReg,
        uint64_t src_b_idx = DefaultBReg,
        uint64_t dst_acc_idx = DefaultAccReg);
    void mmacc(ExecContext *xc, uint64_t src_a_idx, uint64_t src_b_idx,
        uint64_t dst_acc_idx, uint32_t rows, uint32_t cols, uint32_t depth);

    const Stats &getStats() const { return stats; }
    ControlSnapshot controlSnapshot() const;

#ifdef UNIT_TEST
    uint8_t allocateLocalMmuSourceForTest(MemPort port);
    void releaseLocalMmuSourceForTest(uint8_t source);
    void writeABRegForTest(
        uint64_t reg_idx, uint32_t row, uint32_t col, int8_t value);
    int32_t readAccRegForTest(
        uint64_t reg_idx, uint32_t row, uint32_t col) const;
    RegVal readTokenForTest(uint64_t token_idx) const;
    Tick tokenReadyTickForTest(uint64_t token_idx, uint64_t target) const;
    void scheduleMemoryTimingForTest(
        MemPort port, bool is_store, uint64_t requests);
    void scheduleMemoryTimingForTest(MemPort port, bool is_store,
        Addr base, Addr stride, uint32_t rows, uint32_t cols,
        ElemWidth elem_width, bool transpose);
#endif

  private:
    struct DataState
    {
        uint32_t tileM = 0;
        uint32_t tileK = 0;
        uint32_t tileN = 0;
        std::array<std::vector<int8_t>, MatrixRegCount> abRegs;
        std::array<std::vector<int32_t>, MatrixRegCount> accRegs;
        std::vector<RegVal> tokens;
    };

    struct RegStatus
    {
        bool busy = false;
        uint8_t pendingReaders = 0;
        FuType producer = FuType::None;
    };

    struct FuStatus
    {
        bool busy = false;
        uint8_t fifoIdx = 0;
    };

    struct TaskDesc
    {
        TaskOp op = TaskOp::Arith;
        FuType fu = FuType::None;
        MemPort memPort = MemPort::Num;

        bool destValid = false;
        bool destIsAcc = false;
        uint8_t destReg = 0;

        std::array<bool, 3> srcValid = {};
        std::array<bool, 3> srcIsAcc = {};
        std::array<uint8_t, 3> srcReg = {};
        std::array<bool, 3> srcReadPending = {};

        bool isStore = false;
        bool coherent = true;
        bool transpose = false;
        uint64_t tokenIdx = 0;
        uint8_t fifoIdx = 0;
        uint8_t needMask = 0;

        Addr base = 0;
        Addr stride = 0;
        uint32_t rows = 0;
        uint32_t cols = 0;
        uint32_t depth = 0;
        uint32_t bytesPerRow = 0;
        ElemWidth elemWidth = ElemWidth::E8;

        uint64_t memoryBytes = 0;
        uint64_t memoryRequests = 0;
        uint64_t memoryBusBytes = 0;
    };

    struct MemoryRequestShape
    {
        uint64_t rows = 0;
        uint64_t bytesPerRow = 0;
    };

    struct ControlState
    {
        std::array<TaskDesc, DecodedQueueDepth> decodedQueue = {};
        uint8_t queueHead = 0;
        uint8_t queueSize = 0;
        uint8_t nextLoadFifoIdx = 0;
        uint8_t nextComputeFifoIdx = 0;
        uint8_t nextStoreFifoIdx = 0;

        std::array<RegStatus, MatrixRegCount> abRegs = {};
        std::array<RegStatus, MatrixRegCount> cRegs = {};
        std::array<FuStatus, FuTypeCount> fus = {};

        bool pendingStore = false;

        std::array<MemPort, LocalMmuSourceCount> sourceToPort = {};
        std::array<bool, LocalMmuSourceCount> sourceBusy = {};
        uint8_t firstMmuRequestIndex = 0;
        uint8_t localMmuOutstanding = 0;
    };

    struct TimingState
    {
        Tick nextIssueTick = 0;
        Tick pendingStoreReadyTick = 0;
        Tick lastIssueTick = 0;
        Tick lastCompletionTick = 0;
        std::array<Tick, FuTypeCount> fuReadyTick = {};
        std::array<Tick, MatrixRegCount> abWriteReadyTick = {};
        std::array<Tick, MatrixRegCount> cWriteReadyTick = {};
        std::array<Tick, MatrixRegCount> abReadBlockTick = {};
        std::array<Tick, MatrixRegCount> cReadBlockTick = {};
        std::array<std::vector<Tick>, TokenCount> tokenReadyTicks = {};
        Tick localMmuIssueSlotTick = 0;
        uint32_t localMmuIssueSlotsUsed = 0;
        Tick localMmuResponseSlotTick = 0;
        Tick localMmuLastRequestTick = 0;
        Tick localMmuLastResponseTick = 0;
        std::array<Tick, LocalMmuSourceCount> localMmuSourceReadyTick = {};
    };

    DataState data;
    ControlState control;
    TimingConfig timingConfig;
    TimingState timing;
    Stats stats;

    void resetDataState();
    void resetControlState();
    void resetTimingState();
    void normalizeDataState();

    RegVal &token(uint64_t idx);
    const RegVal &token(uint64_t idx) const;
    std::vector<int8_t> &abReg(uint64_t idx);
    const std::vector<int8_t> &abReg(uint64_t idx) const;
    std::vector<int32_t> &accReg(uint64_t idx);
    const std::vector<int32_t> &accReg(uint64_t idx) const;

    TaskDesc makeMemoryTask(MemPort port, FuType fu, bool is_store,
        bool is_acc, Addr base, Addr stride, uint32_t rows, uint32_t cols,
        ElemWidth elem_width, bool transpose, uint8_t reg_idx) const;
    TaskDesc makeMmaTask(uint8_t src_a_idx, uint8_t src_b_idx,
        uint8_t dst_acc_idx, uint32_t rows, uint32_t cols,
        uint32_t depth) const;
    TaskDesc makeZeroTask(uint8_t reg_idx, bool is_acc) const;
    TaskDesc makeReleaseTask(uint64_t token_idx) const;

    Fault executeLoad(ExecContext *xc, const TaskDesc &task);
    Fault executeStore(ExecContext *xc, const TaskDesc &task);
    void executeZero(const TaskDesc &task);
    void executeMma(const TaskDesc &task);

    Tick scheduleTimingTask(ExecContext *xc, const TaskDesc &task);
    Tick scheduleMemoryPipeline(
        ExecContext *xc, const TaskDesc &task, Tick issue_tick);
    Tick taskLatencyTicks(ExecContext *xc, const TaskDesc &task) const;
    Tick taskSourceReadDoneTick(
        ExecContext *xc, const TaskDesc &task, Tick issue_tick,
        Tick complete_tick, size_t src_idx) const;
    Tick cpuCycleTicks(ExecContext *xc) const;
    Tick cyclesToTicks(ExecContext *xc, uint64_t cycles) const;
    Tick peekLocalMmuIssueSlot(ExecContext *xc, Tick earliest_tick) const;
    Tick reserveLocalMmuIssueSlot(ExecContext *xc, Tick earliest_tick);
    Tick reserveLocalMmuResponseSlot(ExecContext *xc, Tick earliest_tick);
    uint8_t chooseTimingLocalMmuSource(Tick earliest_tick) const;
    uint32_t timingLocalMmuOutstanding(Tick tick) const;
    void retireReadyTokens(Tick now);
    void enqueueTokenRelease(uint64_t token_idx, Tick ready_tick);
    void delayPendingTokenEvents(Tick ready_tick);
    Tick tokenTargetReadyTick(uint64_t token_idx, uint64_t target) const;
    uint16_t pendingTokenEvents() const;

    void beginTask(const TaskDesc &task);
    void completeTask();
    void abortTask();
    void assignFifoIdx(TaskDesc &task);

    bool queueFull() const;
    bool queueEmpty() const;
    void pushTask(const TaskDesc &task);
    TaskDesc &headTask();
    const TaskDesc &headTask() const;
    void popTask();

    bool canIssue(const TaskDesc &task) const;
    void reserveTask(const TaskDesc &task);
    void releaseTask(const TaskDesc &task);

    bool regBusy(bool is_acc, uint8_t reg_idx) const;
    bool regHasPendingReaders(bool is_acc, uint8_t reg_idx) const;
    void reserveReg(bool is_acc, uint8_t reg_idx, FuType fu);
    void releaseReg(bool is_acc, uint8_t reg_idx, FuType fu);
    void addRegReader(bool is_acc, uint8_t reg_idx);
    void removeRegReader(bool is_acc, uint8_t reg_idx);

    bool fuBusy(FuType fu) const;
    void setFuBusy(FuType fu, bool busy);

    void recordIssue(const TaskDesc &task);
    void recordCompletion(const TaskDesc &task);
    void recordMemoryRequests(const TaskDesc &task);
    MemPort arbitrateLocalMmuPort(MemPort requested);
    uint8_t allocateLocalMmuSource(MemPort port);
    void completeLocalMmuRequest(uint8_t source, bool is_store);
    void releaseLocalMmuSource(uint8_t source);

    static uint8_t normalizeRegIdx(uint64_t reg_idx);
    static uint32_t clampTileM(uint64_t value);
    static uint32_t clampTileK(uint64_t value);
    static uint32_t clampTileN(uint64_t value);
    static uint64_t divCeil(uint64_t numerator, uint64_t denominator);
    static MemoryRequestShape matrixMemoryRequestShape(
        MemPort port, bool transpose, uint32_t rows, uint32_t cols,
        ElemWidth width);
    static uint64_t matrixMemoryRequests(MemPort port, bool transpose,
        Addr base, Addr stride, uint32_t rows, uint32_t cols,
        ElemWidth width);
    static uint64_t rowRequests(
        Addr base, Addr stride, uint64_t rows, uint64_t bytes_per_row);
    static uint32_t elemWidthBytes(ElemWidth width);
    static size_t fuIndex(FuType fu);
    static size_t memPortIndex(MemPort port);
    static size_t taskEventIndex(TaskEvent event);
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_MATRIX_CONTROLLER_HH__

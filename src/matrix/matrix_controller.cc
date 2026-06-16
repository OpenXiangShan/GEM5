/*
 * XSAI CUTE-aligned matrix controller scaffold.
 */

#include "matrix/matrix_controller.hh"

#include <algorithm>
#include <memory>
#include <string>

#include "base/logging.hh"
#include "cpu/exec_context.hh"
#include "sim/cur_tick.hh"

#ifndef UNIT_TEST
#include "arch/generic/mmu.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "mem/se_translating_port_proxy.hh"
#include "mem/translating_port_proxy.hh"
#include "sim/full_system.hh"
#endif

namespace gem5
{

namespace matrix
{

namespace
{

#ifndef UNIT_TEST
std::string
indexedName(const char *base, size_t idx)
{
    return std::string(base) + std::to_string(idx);
}

bool
checkpointEntryExists(CheckpointIn &cp, const std::string &name)
{
    return cp.entryExists(Serializable::currentSection(), name);
}
#endif

#ifndef UNIT_TEST
Fault
matrixReadBlob(ThreadContext *tc, Addr addr, void *dst, size_t size)
{
    bool ok = false;
    if (FullSystem) {
        TranslatingPortProxy proxy(tc);
        ok = proxy.tryReadBlob(addr, dst, size);
    } else {
        SETranslatingPortProxy proxy(tc);
        ok = proxy.tryReadBlob(addr, dst, size);
    }

    if (!ok) {
        return std::make_shared<GenericPageTableFault>(addr);
    }
    return NoFault;
}

Fault
matrixWriteBlob(ThreadContext *tc, Addr addr, const void *src, size_t size)
{
    bool ok = false;
    if (FullSystem) {
        TranslatingPortProxy proxy(tc);
        ok = proxy.tryWriteBlob(addr, src, size);
    } else {
        SETranslatingPortProxy proxy(tc);
        ok = proxy.tryWriteBlob(addr, src, size);
    }

    if (!ok) {
        return std::make_shared<GenericPageTableFault>(addr);
    }
    return NoFault;
}
#endif

uint8_t
selectHighestFreeSource(
    const std::array<bool, MatrixController::LocalMmuSourceCount> &busy)
{
    for (uint32_t i = MatrixController::LocalMmuSourceCount; i > 0; --i) {
        const uint32_t source = i - 1;
        if (!busy[source]) {
            return static_cast<uint8_t>(source);
        }
    }
    return MatrixController::LocalMmuSourceCount;
}

Tick
peekIssueSlot(Tick earliest_tick, Tick issue_slot_tick,
    uint32_t issue_slots_used, uint32_t issue_per_cycle, Tick cycle_ticks)
{
    const uint32_t slots_per_cycle = std::max<uint32_t>(issue_per_cycle, 1);
    const Tick cycle = std::max<Tick>(cycle_ticks, 1);
    Tick issue_tick = std::max(earliest_tick, issue_slot_tick);
    uint32_t slots_used = issue_slots_used;
    if (issue_tick > issue_slot_tick) {
        slots_used = 0;
    }
    if (slots_used >= slots_per_cycle) {
        issue_tick = std::max(issue_tick, issue_slot_tick + cycle);
    }
    return issue_tick;
}

Tick
reserveIssueSlot(Tick earliest_tick, Tick &issue_slot_tick,
    uint32_t &issue_slots_used, uint32_t issue_per_cycle, Tick cycle_ticks)
{
    const uint32_t slots_per_cycle = std::max<uint32_t>(issue_per_cycle, 1);
    const Tick cycle = std::max<Tick>(cycle_ticks, 1);
    Tick issue_tick = std::max(earliest_tick, issue_slot_tick);
    if (issue_tick > issue_slot_tick) {
        issue_slot_tick = issue_tick;
        issue_slots_used = 0;
    }
    if (issue_slots_used >= slots_per_cycle) {
        issue_slot_tick += cycle;
        issue_slots_used = 0;
        issue_tick = std::max(issue_tick, issue_slot_tick);
    }
    ++issue_slots_used;
    return issue_tick;
}

} // namespace

MatrixController::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent, nullptr),
      ADD_STAT(tasksAccepted, statistics::units::Count::get(),
          "Matrix controller tasks accepted"),
      ADD_STAT(tasksIssued, statistics::units::Count::get(),
          "Matrix controller tasks issued"),
      ADD_STAT(tasksCompleted, statistics::units::Count::get(),
          "Matrix controller tasks completed"),
      ADD_STAT(tasksAborted, statistics::units::Count::get(),
          "Matrix controller tasks aborted"),
      ADD_STAT(aPortTasks, statistics::units::Count::get(),
          "Matrix A-port tasks"),
      ADD_STAT(bPortTasks, statistics::units::Count::get(),
          "Matrix B-port tasks"),
      ADD_STAT(cLoadTasks, statistics::units::Count::get(),
          "Matrix C-load tasks"),
      ADD_STAT(cStoreTasks, statistics::units::Count::get(),
          "Matrix C-store tasks"),
      ADD_STAT(mmaTasks, statistics::units::Count::get(),
          "Matrix MMA tasks"),
      ADD_STAT(zeroTasks, statistics::units::Count::get(),
          "Matrix zero tasks"),
      ADD_STAT(releaseTasks, statistics::units::Count::get(),
          "Matrix release tasks"),
      ADD_STAT(memoryRequests, statistics::units::Count::get(),
          "Analytic LocalMMU requests"),
      ADD_STAT(memoryReadRequests, statistics::units::Count::get(),
          "Analytic LocalMMU read requests"),
      ADD_STAT(memoryWriteRequests, statistics::units::Count::get(),
          "Analytic LocalMMU write requests"),
      ADD_STAT(memoryBytes, statistics::units::Byte::get(),
          "Functional matrix memory bytes"),
      ADD_STAT(memoryReadBytes, statistics::units::Byte::get(),
          "Functional matrix memory read bytes"),
      ADD_STAT(memoryWriteBytes, statistics::units::Byte::get(),
          "Functional matrix memory write bytes"),
      ADD_STAT(memoryBusBytes, statistics::units::Byte::get(),
          "Analytic matrix memory bus bytes"),
      ADD_STAT(memoryReadBusBytes, statistics::units::Byte::get(),
          "Analytic matrix memory read bus bytes"),
      ADD_STAT(memoryWriteBusBytes, statistics::units::Byte::get(),
          "Analytic matrix memory write bus bytes"),
      ADD_STAT(localMmuSourceAllocations, statistics::units::Count::get(),
          "Analytic LocalMMU source allocations"),
      ADD_STAT(localMmuSourceReleases, statistics::units::Count::get(),
          "Analytic LocalMMU source releases"),
      ADD_STAT(localMmuArbitrations, statistics::units::Count::get(),
          "Analytic LocalMMU port arbitrations"),
      ADD_STAT(localMmuReadDataResponses, statistics::units::Count::get(),
          "Analytic LocalMMU read responses"),
      ADD_STAT(localMmuWriteAcks, statistics::units::Count::get(),
          "Analytic LocalMMU write acknowledgements"),
      ADD_STAT(localMmuMaxOutstanding, statistics::units::Count::get(),
          "Maximum analytic LocalMMU outstanding sources"),
      ADD_STAT(memoryPipelineRequests, statistics::units::Count::get(),
          "CUTE memory pipeline requests launched toward L2"),
      ADD_STAT(memoryPipelineReadResponses, statistics::units::Count::get(),
          "CUTE memory pipeline read data responses"),
      ADD_STAT(memoryPipelineWriteAcks, statistics::units::Count::get(),
          "CUTE memory pipeline write acknowledgements"),
      ADD_STAT(memoryPipelineSourceStallTicks, statistics::units::Tick::get(),
          "Ticks CUTE memory requests waited for a free source id"),
      ADD_STAT(memoryPipelineRequestQueueTicks, statistics::units::Tick::get(),
          "Ticks CUTE memory requests waited for LocalMMU/TL-A issue slots"),
      ADD_STAT(memoryPipelineResponseQueueTicks, statistics::units::Tick::get(),
          "Ticks CUTE memory responses waited for the unified response port"),
      ADD_STAT(memoryPipelineLastRequestTick, statistics::units::Tick::get(),
          "Last CUTE memory pipeline request issue tick"),
      ADD_STAT(memoryPipelineLastResponseTick, statistics::units::Tick::get(),
          "Last CUTE memory pipeline response tick"),
      ADD_STAT(memoryPipelineMaxOutstanding, statistics::units::Count::get(),
          "Maximum CUTE memory pipeline outstanding sources"),
      ADD_STAT(timingTasks, statistics::units::Count::get(),
          "Matrix tasks scheduled by the timing model"),
      ADD_STAT(timingQueueTicks, statistics::units::Tick::get(),
          "Matrix task queueing ticks before issue"),
      ADD_STAT(timingMaxQueueTicks, statistics::units::Tick::get(),
          "Maximum matrix task queueing ticks before issue"),
      ADD_STAT(timingBusyTicks, statistics::units::Tick::get(),
          "Analytic matrix engine busy ticks"),
      ADD_STAT(timingLastIssueTick, statistics::units::Tick::get(),
          "Last analytic matrix issue tick"),
      ADD_STAT(timingLastCompletionTick, statistics::units::Tick::get(),
          "Last analytic matrix completion tick"),
      ADD_STAT(acquireStallEvents, statistics::units::Count::get(),
          "macquire events that quiesced the CPU"),
      ADD_STAT(acquireStallTicks, statistics::units::Tick::get(),
          "Total macquire quiesce ticks"),
      ADD_STAT(tokenReleaseEvents, statistics::units::Count::get(),
          "Matrix token release events scheduled"),
      ADD_STAT(tokenReleaseDelayEvents, statistics::units::Count::get(),
          "Matrix token release events delayed by observed L2 responses"),
      ADD_STAT(tokenReleaseDelayTicks, statistics::units::Tick::get(),
          "Total ticks added to pending matrix token release events by "
          "observed L2 responses")
{
}

void
MatrixController::Stats::resetStats()
{
    statistics::Group::resetStats();
    memPortRequests.fill(0);
    memPortReadDataResponses.fill(0);
    memPortWriteAcks.fill(0);
    localMmuPortSelections.fill(0);
    taskEvents.fill(0);
}

MatrixController::MatrixController(statistics::Group *parent)
    : stats(parent)
{
    reset();
}

void
MatrixController::reset()
{
    resetDataState();
    resetControlState();
    resetTimingState();
    stats.resetStats();
}

void
MatrixController::setTimingConfig(const TimingConfig &config)
{
    timingConfig = config;
    timingConfig.issueIntervalCycles =
        std::max<uint32_t>(timingConfig.issueIntervalCycles, 1);
    timingConfig.localMmuIssuePerCycle =
        std::max<uint32_t>(timingConfig.localMmuIssuePerCycle, 1);
    timingConfig.l2ResponsePipelineCycles =
        std::max<uint32_t>(timingConfig.l2ResponsePipelineCycles, 1);
}

void
MatrixController::retireReadyTokensUpTo(Tick now)
{
    retireReadyTokens(now);
}

bool
MatrixController::tokenTargetReached(
    uint64_t token_idx, uint64_t target) const
{
    return token(token_idx) >= target;
}

Tick
MatrixController::tokenReadyTick(uint64_t token_idx, uint64_t target) const
{
    return tokenTargetReadyTick(token_idx, target);
}

void
MatrixController::recordAcquireStall(Tick ticks)
{
    ++stats.acquireStallEvents;
    stats.acquireStallTicks += ticks;
}

void
MatrixController::resetDataState()
{
    data.tileM = 0;
    data.tileK = 0;
    data.tileN = 0;
    for (auto &reg : data.abRegs) {
        reg.assign(MatrixABRegBytes, 0);
    }
    for (auto &reg : data.accRegs) {
        reg.assign(MatrixAccElems, 0);
    }
    data.tokens.assign(TokenCount, 0);
}

void
MatrixController::resetControlState()
{
    control = ControlState{};
    control.sourceToPort.fill(MemPort::Num);
    control.sourceBusy.fill(false);
}

void
MatrixController::resetTimingState()
{
    timing = TimingState{};
}

MatrixController::ControlSnapshot
MatrixController::controlSnapshot() const
{
    ControlSnapshot snapshot;
    snapshot.decodedQueueHead = control.queueHead;
    snapshot.decodedQueueSize = control.queueSize;
    snapshot.nextLoadFifoIdx = control.nextLoadFifoIdx;
    snapshot.nextComputeFifoIdx = control.nextComputeFifoIdx;
    snapshot.nextStoreFifoIdx = control.nextStoreFifoIdx;

    for (size_t i = 0; i < FuTypeCount; ++i) {
        if (control.fus[i].busy) {
            snapshot.fuBusyMask |= 1U << i;
        }
    }

    for (size_t i = 0; i < MatrixRegCount; ++i) {
        if (control.abRegs[i].busy) {
            snapshot.abBusyMask |= 1U << i;
        }
        if (control.cRegs[i].busy) {
            snapshot.cBusyMask |= 1U << i;
        }

        snapshot.abPendingReaders[i] = control.abRegs[i].pendingReaders;
        snapshot.cPendingReaders[i] = control.cRegs[i].pendingReaders;
        if (control.abRegs[i].pendingReaders != 0) {
            snapshot.abPendingReaderMask |= 1U << i;
        }
        if (control.cRegs[i].pendingReaders != 0) {
            snapshot.cPendingReaderMask |= 1U << i;
        }
    }

    snapshot.pendingStore = control.pendingStore;
    snapshot.firstMmuRequestIndex = control.firstMmuRequestIndex;
    snapshot.nextLocalMmuSource = selectHighestFreeSource(
        control.sourceBusy);
    snapshot.localMmuOutstanding = control.localMmuOutstanding;
    for (size_t i = 0; i < LocalMmuSourceCount; ++i) {
        if (control.sourceBusy[i]) {
            snapshot.localMmuBusySourceMask |= 1ULL << i;
        }
    }
    snapshot.timingNextIssueTick = timing.nextIssueTick;
    snapshot.timingPendingStoreReadyTick = timing.pendingStoreReadyTick;
    snapshot.timingLastIssueTick = timing.lastIssueTick;
    snapshot.timingLastCompletionTick = timing.lastCompletionTick;
    snapshot.timingLocalMmuIssueSlotTick = timing.localMmuIssueSlotTick;
    snapshot.timingLocalMmuResponseSlotTick =
        timing.localMmuResponseSlotTick;
    snapshot.timingLocalMmuLastRequestTick =
        timing.localMmuLastRequestTick;
    snapshot.timingLocalMmuLastResponseTick =
        timing.localMmuLastResponseTick;
    snapshot.timingLocalMmuOutstanding =
        timingLocalMmuOutstanding(curTick());
    snapshot.pendingTokenEvents = pendingTokenEvents();

    return snapshot;
}

#ifdef UNIT_TEST
uint8_t
MatrixController::allocateLocalMmuSourceForTest(MemPort port)
{
    return allocateLocalMmuSource(port);
}

void
MatrixController::releaseLocalMmuSourceForTest(uint8_t source)
{
    releaseLocalMmuSource(source);
}

void
MatrixController::writeABRegForTest(
    uint64_t reg_idx, uint32_t row, uint32_t col, int8_t value)
{
    panic_if(row >= MatrixMaxM || col >= MatrixMaxK,
        "matrix AB test write out of range");
    abReg(reg_idx)[row * MatrixMaxK + col] = value;
}

int32_t
MatrixController::readAccRegForTest(
    uint64_t reg_idx, uint32_t row, uint32_t col) const
{
    panic_if(row >= MatrixMaxM || col >= MatrixMaxN,
        "matrix accumulator test read out of range");
    return accReg(reg_idx)[row * MatrixMaxN + col];
}

RegVal
MatrixController::readTokenForTest(uint64_t token_idx) const
{
    return token(token_idx);
}

Tick
MatrixController::tokenReadyTickForTest(
    uint64_t token_idx, uint64_t target) const
{
    return tokenTargetReadyTick(token_idx, target);
}

void
MatrixController::scheduleMemoryTimingForTest(
    MemPort port, bool is_store, uint64_t requests)
{
    TaskDesc task;
    task.op = TaskOp::LoadStore;
    task.memPort = port;
    task.isStore = is_store;
    task.memoryRequests = requests;
    task.memoryBytes = requests * OutsideDataWidthBytes;
    task.memoryBusBytes = task.memoryBytes;
    task.base = 0x1000;
    task.stride = OutsideDataWidthBytes;
    task.rows = static_cast<uint32_t>(requests);
    task.cols = OutsideDataWidthBytes;
    task.bytesPerRow = OutsideDataWidthBytes;
    task.fu = port == MemPort::A ? FuType::AML :
        port == MemPort::B ? FuType::BML :
        port == MemPort::CLoad ? FuType::CMLLoad :
        port == MemPort::CStore ? FuType::CMLStore :
        FuType::None;
    task.destValid = !is_store;
    task.destIsAcc = port == MemPort::CLoad;
    task.destReg = 0;
    task.srcValid[0] = is_store;
    task.srcIsAcc[0] = port == MemPort::CStore;
    task.srcReg[0] = 0;
    task.srcReadPending[0] = is_store;

    beginTask(task);
    scheduleTimingTask(nullptr, task);
    completeTask();
}

void
MatrixController::scheduleMemoryTimingForTest(MemPort port, bool is_store,
    Addr base, Addr stride, uint32_t rows, uint32_t cols,
    ElemWidth elem_width, bool transpose)
{
    const FuType fu = port == MemPort::A ? FuType::AML :
        port == MemPort::B ? FuType::BML :
        port == MemPort::CLoad ? FuType::CMLLoad :
        port == MemPort::CStore ? FuType::CMLStore :
        FuType::None;
    const bool is_acc = port == MemPort::CLoad || port == MemPort::CStore;
    TaskDesc task = makeMemoryTask(port, fu, is_store, is_acc, base, stride,
        rows, cols, elem_width, transpose, 0);

    beginTask(task);
    scheduleTimingTask(nullptr, task);
    completeTask();
}
#endif

void
MatrixController::normalizeDataState()
{
    data.tileM = clampTileM(data.tileM);
    data.tileK = clampTileK(data.tileK);
    data.tileN = clampTileN(data.tileN);
    for (auto &reg : data.abRegs) {
        reg.resize(MatrixABRegBytes, 0);
    }
    for (auto &reg : data.accRegs) {
        reg.resize(MatrixAccElems, 0);
    }
    data.tokens.resize(TokenCount, 0);
}

void
MatrixController::serialize(CheckpointOut &cp) const
{
#ifdef UNIT_TEST
    panic("MatrixController checkpoint serialization is not linked in unit "
        "tests");
#else
    paramOut(cp, "matrixTileM", data.tileM);
    paramOut(cp, "matrixTileK", data.tileK);
    paramOut(cp, "matrixTileN", data.tileN);

    for (size_t i = 0; i < MatrixRegCount; ++i) {
        arrayParamOut(cp, indexedName("matrixABReg", i), data.abRegs[i]);
        arrayParamOut(cp, indexedName("matrixAccReg", i), data.accRegs[i]);
    }

    arrayParamOut(cp, "matrixTileA", data.abRegs[DefaultAReg]);
    arrayParamOut(cp, "matrixTileB", data.abRegs[DefaultBReg]);
    arrayParamOut(cp, "matrixAcc", data.accRegs[DefaultAccReg]);
    arrayParamOut(cp, "matrixTokens", data.tokens);

    paramOut(cp, "matrixTimingNextIssueTick", timing.nextIssueTick);
    paramOut(cp, "matrixTimingPendingStoreReadyTick",
        timing.pendingStoreReadyTick);
    paramOut(cp, "matrixTimingLastIssueTick", timing.lastIssueTick);
    paramOut(cp, "matrixTimingLastCompletionTick",
        timing.lastCompletionTick);
    paramOut(cp, "matrixTimingLocalMmuIssueSlotTick",
        timing.localMmuIssueSlotTick);
    paramOut(cp, "matrixTimingLocalMmuIssueSlotsUsed",
        timing.localMmuIssueSlotsUsed);
    paramOut(cp, "matrixTimingLocalMmuResponseSlotTick",
        timing.localMmuResponseSlotTick);
    paramOut(cp, "matrixTimingLocalMmuLastRequestTick",
        timing.localMmuLastRequestTick);
    paramOut(cp, "matrixTimingLocalMmuLastResponseTick",
        timing.localMmuLastResponseTick);
    arrayParamOut(cp, "matrixTimingLocalMmuSourceReadyTicks",
        timing.localMmuSourceReadyTick);
    for (size_t i = 0; i < TokenCount; ++i) {
        arrayParamOut(cp, indexedName("matrixTokenReadyTicks", i),
            timing.tokenReadyTicks[i]);
    }
#endif
}

void
MatrixController::unserialize(CheckpointIn &cp)
{
#ifdef UNIT_TEST
    panic("MatrixController checkpoint unserialization is not linked in unit "
        "tests");
#else
    paramIn(cp, "matrixTileM", data.tileM);
    paramIn(cp, "matrixTileK", data.tileK);
    paramIn(cp, "matrixTileN", data.tileN);

    for (auto &reg : data.abRegs) {
        reg.assign(MatrixABRegBytes, 0);
    }
    for (auto &reg : data.accRegs) {
        reg.assign(MatrixAccElems, 0);
    }

    bool found_new_regs = false;
    for (size_t i = 0; i < MatrixRegCount; ++i) {
        const std::string ab_name = indexedName("matrixABReg", i);
        if (checkpointEntryExists(cp, ab_name)) {
            arrayParamIn(cp, ab_name, data.abRegs[i]);
            found_new_regs = true;
        }

        const std::string acc_name = indexedName("matrixAccReg", i);
        if (checkpointEntryExists(cp, acc_name)) {
            arrayParamIn(cp, acc_name, data.accRegs[i]);
            found_new_regs = true;
        }
    }

    if (!found_new_regs) {
        arrayParamIn(cp, "matrixTileA", data.abRegs[DefaultAReg]);
        arrayParamIn(cp, "matrixTileB", data.abRegs[DefaultBReg]);
        arrayParamIn(cp, "matrixAcc", data.accRegs[DefaultAccReg]);
    }
    arrayParamIn(cp, "matrixTokens", data.tokens);

    normalizeDataState();
    resetControlState();
    resetTimingState();
    if (checkpointEntryExists(cp, "matrixTimingNextIssueTick")) {
        paramIn(cp, "matrixTimingNextIssueTick", timing.nextIssueTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingPendingStoreReadyTick")) {
        paramIn(cp, "matrixTimingPendingStoreReadyTick",
            timing.pendingStoreReadyTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLastIssueTick")) {
        paramIn(cp, "matrixTimingLastIssueTick", timing.lastIssueTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLastCompletionTick")) {
        paramIn(cp, "matrixTimingLastCompletionTick",
            timing.lastCompletionTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLocalMmuIssueSlotTick")) {
        paramIn(cp, "matrixTimingLocalMmuIssueSlotTick",
            timing.localMmuIssueSlotTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLocalMmuIssueSlotsUsed")) {
        paramIn(cp, "matrixTimingLocalMmuIssueSlotsUsed",
            timing.localMmuIssueSlotsUsed);
    }
    if (checkpointEntryExists(cp, "matrixTimingLocalMmuResponseSlotTick")) {
        paramIn(cp, "matrixTimingLocalMmuResponseSlotTick",
            timing.localMmuResponseSlotTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLocalMmuLastRequestTick")) {
        paramIn(cp, "matrixTimingLocalMmuLastRequestTick",
            timing.localMmuLastRequestTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLocalMmuLastResponseTick")) {
        paramIn(cp, "matrixTimingLocalMmuLastResponseTick",
            timing.localMmuLastResponseTick);
    }
    if (checkpointEntryExists(cp, "matrixTimingLocalMmuSourceReadyTicks")) {
        arrayParamIn(cp, "matrixTimingLocalMmuSourceReadyTicks",
            timing.localMmuSourceReadyTick.data(),
            timing.localMmuSourceReadyTick.size());
    }
    for (size_t i = 0; i < TokenCount; ++i) {
        const std::string token_ready_name =
            indexedName("matrixTokenReadyTicks", i);
        if (checkpointEntryExists(cp, token_ready_name)) {
            arrayParamIn(cp, token_ready_name, timing.tokenReadyTicks[i]);
        }
    }
#endif
}

RegVal &
MatrixController::token(uint64_t idx)
{
    panic_if(idx >= data.tokens.size(),
        "matrix token index %llu out of range",
        static_cast<unsigned long long>(idx));
    return data.tokens[idx];
}

const RegVal &
MatrixController::token(uint64_t idx) const
{
    panic_if(idx >= data.tokens.size(),
        "matrix token index %llu out of range",
        static_cast<unsigned long long>(idx));
    return data.tokens[idx];
}

std::vector<int8_t> &
MatrixController::abReg(uint64_t idx)
{
    return data.abRegs[normalizeRegIdx(idx)];
}

const std::vector<int8_t> &
MatrixController::abReg(uint64_t idx) const
{
    return data.abRegs[normalizeRegIdx(idx)];
}

std::vector<int32_t> &
MatrixController::accReg(uint64_t idx)
{
    return data.accRegs[normalizeRegIdx(idx)];
}

const std::vector<int32_t> &
MatrixController::accReg(uint64_t idx) const
{
    return data.accRegs[normalizeRegIdx(idx)];
}

void
MatrixController::syncReset(uint64_t token_idx)
{
    token(token_idx) = 0;
    timing.tokenReadyTicks[token_idx].clear();
}

void
MatrixController::release(ExecContext *xc, uint64_t token_idx)
{
    TaskDesc task = makeReleaseTask(token_idx);
    beginTask(task);
    const Tick ready_tick = scheduleTimingTask(xc, task);
    enqueueTokenRelease(token_idx, ready_tick);
    completeTask();
}

void
MatrixController::acquire(ExecContext *xc, uint64_t token_idx, uint64_t target)
{
#ifdef UNIT_TEST
    panic("MatrixController::acquire requires a ThreadContext in unit tests");
#else
    ThreadContext *tc = xc->tcBase();
    const Tick now = curTick();
    retireReadyTokens(now);

    const RegVal observed = token(token_idx);
    if (observed >= target) {
        return;
    }

    const Tick ready_tick = tokenTargetReadyTick(token_idx, target);
    panic_if(ready_tick == MaxTick,
        "macquire tok%llu target=%llu observed=%llu has no pending release",
        static_cast<unsigned long long>(token_idx),
        static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(observed));

    if (ready_tick > now) {
        ++stats.acquireStallEvents;
        stats.acquireStallTicks += ready_tick - now;
        tc->quiesceTick(ready_tick);
    } else {
        retireReadyTokens(ready_tick);
    }
#endif
}

void
MatrixController::setTileM(uint64_t value)
{
    data.tileM = clampTileM(value);
}

void
MatrixController::setTileK(uint64_t value)
{
    data.tileK = clampTileK(value);
}

void
MatrixController::setTileN(uint64_t value)
{
    data.tileN = clampTileN(value);
}

Fault
MatrixController::load(ExecContext *xc, MemPort port, Addr base, Addr stride,
    uint32_t rows, uint32_t cols, ElemWidth width, bool transpose,
    uint64_t reg_idx)
{
    panic_if(port != MemPort::A && port != MemPort::B &&
        port != MemPort::CLoad, "unsupported matrix load port");

    const bool is_acc = port == MemPort::CLoad;
    const FuType fu = port == MemPort::A ? FuType::AML :
        port == MemPort::B ? FuType::BML : FuType::CMLLoad;
    const uint8_t reg = normalizeRegIdx(reg_idx);
    TaskDesc task = makeMemoryTask(port, fu, false, is_acc, base, stride,
        rows, cols, width, transpose, reg);

    beginTask(task);
    Fault fault = executeLoad(xc, task);
    if (fault != NoFault) {
        abortTask();
        return fault;
    }

    scheduleTimingTask(xc, task);
    completeTask();
    return NoFault;
}

Fault
MatrixController::store(ExecContext *xc, MemPort port, Addr base, Addr stride,
    uint32_t rows, uint32_t cols, ElemWidth width, bool transpose,
    uint64_t reg_idx)
{
    panic_if(port != MemPort::CStore,
        "matrix store currently models the RTL CML-store path only");

    const bool is_acc = true;
    const FuType fu = FuType::CMLStore;
    const uint8_t reg = normalizeRegIdx(reg_idx);
    TaskDesc task = makeMemoryTask(port, fu, true, is_acc, base, stride,
        rows, cols, width, transpose, reg);

    beginTask(task);
    Fault fault = executeStore(xc, task);
    if (fault != NoFault) {
        abortTask();
        return fault;
    }

    scheduleTimingTask(xc, task);
    completeTask();
    return NoFault;
}

Fault
MatrixController::loadA8(
    ExecContext *xc, Addr base, Addr stride, uint64_t reg_idx)
{
    return load(xc, MemPort::A, base, stride, data.tileM, data.tileK,
        ElemWidth::E8, false, reg_idx);
}

Fault
MatrixController::loadB8(
    ExecContext *xc, Addr base, Addr stride, uint64_t reg_idx)
{
    return load(xc, MemPort::B, base, stride, data.tileN, data.tileK,
        ElemWidth::E8, false, reg_idx);
}

Fault
MatrixController::loadC32(
    ExecContext *xc, Addr base, Addr stride, uint64_t acc_idx)
{
    return load(xc, MemPort::CLoad, base, stride, data.tileM, data.tileN,
        ElemWidth::E32, false, acc_idx);
}

Fault
MatrixController::storeC32(
    ExecContext *xc, Addr base, Addr stride, uint64_t acc_idx)
{
    return store(xc, MemPort::CStore, base, stride, data.tileM, data.tileN,
        ElemWidth::E32, false, acc_idx);
}

void
MatrixController::zeroAcc(ExecContext *xc, uint64_t acc_idx)
{
    zero(xc, acc_idx, true);
}

void
MatrixController::zero(ExecContext *xc, uint64_t reg_idx, bool is_acc)
{
    const uint8_t reg = normalizeRegIdx(reg_idx);
    TaskDesc task = makeZeroTask(reg, is_acc);
    beginTask(task);
    executeZero(task);
    scheduleTimingTask(xc, task);
    completeTask();
}

void
MatrixController::mmaccWB(
    uint64_t src_a_idx, uint64_t src_b_idx, uint64_t dst_acc_idx)
{
    mmaccWB(nullptr, src_a_idx, src_b_idx, dst_acc_idx);
}

void
MatrixController::mmaccWB(ExecContext *xc,
    uint64_t src_a_idx, uint64_t src_b_idx, uint64_t dst_acc_idx)
{
    mmacc(xc, src_a_idx, src_b_idx, dst_acc_idx, data.tileM, data.tileN,
        data.tileK);
}

void
MatrixController::mmacc(ExecContext *xc,
    uint64_t src_a_idx, uint64_t src_b_idx, uint64_t dst_acc_idx,
    uint32_t rows, uint32_t cols, uint32_t depth)
{
    const uint8_t a_reg_idx = normalizeRegIdx(src_a_idx);
    const uint8_t b_reg_idx = normalizeRegIdx(src_b_idx);
    const uint8_t acc_reg_idx = normalizeRegIdx(dst_acc_idx);
    TaskDesc task = makeMmaTask(a_reg_idx, b_reg_idx, acc_reg_idx,
        rows, cols, depth);
    beginTask(task);
    executeMma(task);
    scheduleTimingTask(xc, task);
    completeTask();
}

MatrixController::TaskDesc
MatrixController::makeMemoryTask(MemPort port, FuType fu, bool is_store,
    bool is_acc, Addr base, Addr stride, uint32_t rows, uint32_t cols,
    ElemWidth elem_width, bool transpose, uint8_t reg_idx) const
{
    TaskDesc task;
    const uint32_t bytes_per_row = cols * elemWidthBytes(elem_width);
    task.op = TaskOp::LoadStore;
    task.fu = fu;
    task.memPort = port;
    task.isStore = is_store;
    task.transpose = transpose;
    task.destValid = !is_store;
    task.destIsAcc = is_acc;
    task.destReg = reg_idx;
    task.srcValid[0] = is_store;
    task.srcIsAcc[0] = is_acc;
    task.srcReg[0] = reg_idx;
    task.srcReadPending[0] = is_store;
    task.base = base;
    task.stride = stride;
    task.rows = rows;
    task.cols = cols;
    task.bytesPerRow = bytes_per_row;
    task.elemWidth = elem_width;
    task.memoryBytes = static_cast<uint64_t>(rows) * bytes_per_row;
    task.memoryRequests = matrixMemoryRequests(port, transpose, base, stride,
        rows, cols, elem_width);
    task.memoryBusBytes = task.memoryRequests * OutsideDataWidthBytes;
    switch (port) {
      case MemPort::A:
        task.needMask = 0x1;
        break;
      case MemPort::B:
        task.needMask = 0x2;
        break;
      case MemPort::CLoad:
      case MemPort::CStore:
        task.needMask = 0x4;
        break;
      default:
        task.needMask = 0;
        break;
    }
    return task;
}

MatrixController::TaskDesc
MatrixController::makeMmaTask(
    uint8_t src_a_idx, uint8_t src_b_idx, uint8_t dst_acc_idx,
    uint32_t rows, uint32_t cols, uint32_t depth) const
{
    TaskDesc task;
    task.op = TaskOp::Mma;
    task.fu = FuType::Compute;
    task.destValid = true;
    task.destIsAcc = true;
    task.destReg = dst_acc_idx;
    task.srcValid[0] = true;
    task.srcIsAcc[0] = false;
    task.srcReg[0] = src_a_idx;
    task.srcReadPending[0] = true;
    task.srcValid[1] = true;
    task.srcIsAcc[1] = false;
    task.srcReg[1] = src_b_idx;
    task.srcReadPending[1] = true;
    task.srcValid[2] = true;
    task.srcIsAcc[2] = true;
    task.srcReg[2] = dst_acc_idx;
    task.rows = rows;
    task.cols = cols;
    task.depth = depth;
    task.needMask = 0x7;
    return task;
}

MatrixController::TaskDesc
MatrixController::makeZeroTask(uint8_t reg_idx, bool is_acc) const
{
    TaskDesc task;
    task.op = TaskOp::Arith;
    task.fu = is_acc ? FuType::CMLLoad : FuType::AML;
    task.destValid = true;
    task.destIsAcc = is_acc;
    task.destReg = reg_idx;
    task.rows = is_acc ? MatrixMaxM : MatrixMaxM;
    task.cols = is_acc ? MatrixMaxN : MatrixMaxK;
    task.needMask = is_acc ? 0x4 : 0x1;
    return task;
}

MatrixController::TaskDesc
MatrixController::makeReleaseTask(uint64_t token_idx) const
{
    TaskDesc task;
    task.op = TaskOp::Release;
    task.fu = FuType::None;
    task.tokenIdx = token_idx;
    return task;
}

Fault
MatrixController::executeLoad(ExecContext *xc, const TaskDesc &task)
{
#ifdef UNIT_TEST
    panic("MatrixController memory loads are not linked in unit tests");
#else
    ThreadContext *tc = xc->tcBase();

    if (task.destIsAcc) {
        panic_if(task.elemWidth != ElemWidth::E32,
            "C matrix functional load currently supports e32 only");
        panic_if(task.rows > MatrixMaxM || task.cols > MatrixMaxN,
            "C matrix load shape exceeds accumulator register capacity");

        auto &dst_reg = accReg(task.destReg);
        for (uint32_t row = 0; row < task.rows; ++row) {
            auto *dst =
                reinterpret_cast<uint8_t *>(&dst_reg[row * MatrixMaxN]);
            Fault fault = matrixReadBlob(tc, task.base + row * task.stride,
                dst, task.bytesPerRow);
            if (fault != NoFault) {
                return fault;
            }
        }
        return NoFault;
    }

    panic_if(task.elemWidth != ElemWidth::E8,
        "AB matrix functional load currently supports e8 only");
    panic_if(task.rows > MatrixMaxM || task.cols > MatrixMaxK,
        "AB matrix load shape exceeds tile register capacity");

    auto &dst_reg = abReg(task.destReg);
    for (uint32_t row = 0; row < task.rows; ++row) {
        auto *dst = reinterpret_cast<uint8_t *>(&dst_reg[row * MatrixMaxK]);
        Fault fault = matrixReadBlob(tc, task.base + row * task.stride, dst,
            task.bytesPerRow);
        if (fault != NoFault) {
            return fault;
        }
    }
    return NoFault;
#endif
}

Fault
MatrixController::executeStore(ExecContext *xc, const TaskDesc &task)
{
#ifdef UNIT_TEST
    panic("MatrixController memory stores are not linked in unit tests");
#else
    ThreadContext *tc = xc->tcBase();

    if (task.srcIsAcc[0]) {
        panic_if(task.elemWidth != ElemWidth::E32,
            "C matrix functional store currently supports e32 only");
        panic_if(task.rows > MatrixMaxM || task.cols > MatrixMaxN,
            "C matrix store shape exceeds accumulator register capacity");

        const auto &src_reg = accReg(task.srcReg[0]);
        for (uint32_t row = 0; row < task.rows; ++row) {
            const auto *src =
                reinterpret_cast<const uint8_t *>(&src_reg[row * MatrixMaxN]);
            Fault fault = matrixWriteBlob(tc, task.base + row * task.stride,
                src, task.bytesPerRow);
            if (fault != NoFault) {
                return fault;
            }
        }
        return NoFault;
    }

    panic_if(task.elemWidth != ElemWidth::E8,
        "AB matrix functional store currently supports e8 only");
    panic_if(task.rows > MatrixMaxM || task.cols > MatrixMaxK,
        "AB matrix store shape exceeds tile register capacity");

    const auto &src_reg = abReg(task.srcReg[0]);
    for (uint32_t row = 0; row < task.rows; ++row) {
        const auto *src =
            reinterpret_cast<const uint8_t *>(&src_reg[row * MatrixMaxK]);
        Fault fault = matrixWriteBlob(tc, task.base + row * task.stride, src,
            task.bytesPerRow);
        if (fault != NoFault) {
            return fault;
        }
    }
    return NoFault;
#endif
}

void
MatrixController::executeZero(const TaskDesc &task)
{
    if (task.destIsAcc) {
        std::fill(accReg(task.destReg).begin(), accReg(task.destReg).end(),
            0);
    } else {
        std::fill(abReg(task.destReg).begin(), abReg(task.destReg).end(), 0);
    }
}

void
MatrixController::executeMma(const TaskDesc &task)
{
    panic_if(task.rows > MatrixMaxM || task.cols > MatrixMaxN ||
        task.depth > MatrixMaxK, "matrix mma shape exceeds register capacity");

    const auto &a_reg = abReg(task.srcReg[0]);
    const auto &b_reg = abReg(task.srcReg[1]);
    auto &dst_reg = accReg(task.destReg);
    for (uint32_t m = 0; m < task.rows; ++m) {
        for (uint32_t n = 0; n < task.cols; ++n) {
            int32_t acc = dst_reg[m * MatrixMaxN + n];
            for (uint32_t k = 0; k < task.depth; ++k) {
                const int8_t a = a_reg[m * MatrixMaxK + k];
                const int8_t b = b_reg[n * MatrixMaxK + k];
                acc += static_cast<int32_t>(a) * static_cast<int32_t>(b);
            }
            dst_reg[m * MatrixMaxN + n] = acc;
        }
    }
}

Tick
MatrixController::scheduleTimingTask(ExecContext *xc, const TaskDesc &task)
{
    const Tick now = curTick();

    Tick issue_tick = std::max(now, timing.nextIssueTick);
    if (task.op == TaskOp::Release) {
        issue_tick = std::max(issue_tick, timing.pendingStoreReadyTick);
        issue_tick = std::max(issue_tick, timing.lastCompletionTick);
    }

    if (task.fu != FuType::None) {
        issue_tick = std::max(issue_tick, timing.fuReadyTick[fuIndex(task.fu)]);
    }

    if (task.destValid) {
        const uint8_t dest = normalizeRegIdx(task.destReg);
        const Tick write_ready = task.destIsAcc ?
            timing.cWriteReadyTick[dest] : timing.abWriteReadyTick[dest];
        const Tick read_block = task.destIsAcc ?
            timing.cReadBlockTick[dest] : timing.abReadBlockTick[dest];
        issue_tick = std::max(issue_tick, std::max(write_ready, read_block));
    }

    for (size_t i = 0; i < task.srcValid.size(); ++i) {
        if (!task.srcValid[i]) {
            continue;
        }

        const uint8_t src = normalizeRegIdx(task.srcReg[i]);
        const Tick write_ready = task.srcIsAcc[i] ?
            timing.cWriteReadyTick[src] : timing.abWriteReadyTick[src];
        issue_tick = std::max(issue_tick, write_ready);

        if (task.isStore) {
            const Tick read_block = task.srcIsAcc[i] ?
                timing.cReadBlockTick[src] : timing.abReadBlockTick[src];
            issue_tick = std::max(issue_tick, read_block);
        }
    }

    const Tick complete_tick = task.op == TaskOp::LoadStore ?
        scheduleMemoryPipeline(xc, task, issue_tick) :
        issue_tick + taskLatencyTicks(xc, task);
    timing.nextIssueTick = issue_tick +
        cyclesToTicks(xc, timingConfig.issueIntervalCycles);
    timing.lastIssueTick = issue_tick;
    timing.lastCompletionTick =
        std::max(timing.lastCompletionTick, complete_tick);

    if (task.fu != FuType::None) {
        timing.fuReadyTick[fuIndex(task.fu)] = complete_tick;
    }

    if (task.destValid) {
        const uint8_t dest = normalizeRegIdx(task.destReg);
        if (task.destIsAcc) {
            timing.cWriteReadyTick[dest] = complete_tick;
        } else {
            timing.abWriteReadyTick[dest] = complete_tick;
        }
    }

    for (size_t i = 0; i < task.srcValid.size(); ++i) {
        if (!task.srcValid[i] || !task.srcReadPending[i]) {
            continue;
        }

        const uint8_t src = normalizeRegIdx(task.srcReg[i]);
        const Tick read_done =
            taskSourceReadDoneTick(xc, task, issue_tick, complete_tick, i);
        if (task.srcIsAcc[i]) {
            timing.cReadBlockTick[src] =
                std::max(timing.cReadBlockTick[src], read_done);
        } else {
            timing.abReadBlockTick[src] =
                std::max(timing.abReadBlockTick[src], read_done);
        }
    }

    if (task.isStore) {
        timing.pendingStoreReadyTick =
            std::max(timing.pendingStoreReadyTick, complete_tick);
    }

    const Tick queue_ticks = issue_tick > now ? issue_tick - now : 0;
    ++stats.timingTasks;
    stats.timingQueueTicks += queue_ticks;
    stats.timingMaxQueueTicks =
        std::max<uint64_t>(
            static_cast<uint64_t>(stats.timingMaxQueueTicks.value()),
            queue_ticks);
    stats.timingBusyTicks += complete_tick - issue_tick;
    stats.timingLastIssueTick = issue_tick;
    stats.timingLastCompletionTick = timing.lastCompletionTick;

    return complete_tick;
}

Tick
MatrixController::scheduleMemoryPipeline(
    ExecContext *xc, const TaskDesc &task, Tick issue_tick)
{
    panic_if(task.op != TaskOp::LoadStore,
        "matrix memory pipeline scheduled for non-memory task");

    const uint64_t base_cycles =
        task.isStore ? timingConfig.storeBaseCycles :
        timingConfig.loadBaseCycles;
    const Tick request_ready_tick = issue_tick +
        cyclesToTicks(xc, base_cycles + timingConfig.localMmuArbCycles);
    const Tick l2_request_pipe =
        cyclesToTicks(xc, timingConfig.l2RequestPipelineCycles);
    const Tick response_latency = cyclesToTicks(xc,
        task.isStore ? timingConfig.localMmuWriteAckLatencyCycles :
        timingConfig.localMmuReadLatencyCycles);

    Tick complete_tick = request_ready_tick;
    stats.memoryPipelineRequests += task.memoryRequests;

    for (uint64_t i = 0; i < task.memoryRequests; ++i) {
        Tick issue_candidate = request_ready_tick;
        Tick request_tick = request_ready_tick;
        Tick source_ready_tick = request_ready_tick;
        uint8_t source = 0;
        while (true) {
            const Tick candidate_request_tick =
                peekLocalMmuIssueSlot(xc, issue_candidate);
            if (candidate_request_tick > issue_candidate) {
                stats.memoryPipelineRequestQueueTicks +=
                    candidate_request_tick - issue_candidate;
            }

            source = chooseTimingLocalMmuSource(candidate_request_tick);
            source_ready_tick = timing.localMmuSourceReadyTick[source];
            if (source_ready_tick <= candidate_request_tick) {
                request_tick = reserveLocalMmuIssueSlot(xc, issue_candidate);
                panic_if(request_tick != candidate_request_tick,
                    "matrix LocalMMU issue slot changed after source "
                    "selection");
                break;
            }

            stats.memoryPipelineSourceStallTicks +=
                source_ready_tick - candidate_request_tick;
            issue_candidate = source_ready_tick;
        }

        const Tick response_candidate =
            request_tick + l2_request_pipe + response_latency;
        const Tick response_tick =
            reserveLocalMmuResponseSlot(xc, response_candidate);
        if (response_tick > response_candidate) {
            stats.memoryPipelineResponseQueueTicks +=
                response_tick - response_candidate;
        }

        timing.localMmuLastRequestTick =
            std::max(timing.localMmuLastRequestTick, request_tick);
        timing.localMmuLastResponseTick =
            std::max(timing.localMmuLastResponseTick, response_tick);

        const Tick source_reuse_tick =
            response_tick + cyclesToTicks(xc,
                timingConfig.l2ResponsePipelineCycles);
        timing.localMmuSourceReadyTick[source] = source_reuse_tick;
        const uint32_t outstanding = timingLocalMmuOutstanding(request_tick);
        stats.memoryPipelineMaxOutstanding = std::max<uint64_t>(
            static_cast<uint64_t>(
                stats.memoryPipelineMaxOutstanding.value()),
            outstanding);

        if (task.isStore) {
            ++stats.memoryPipelineWriteAcks;
        } else {
            ++stats.memoryPipelineReadResponses;
        }
        stats.memoryPipelineLastRequestTick =
            timing.localMmuLastRequestTick;
        stats.memoryPipelineLastResponseTick =
            timing.localMmuLastResponseTick;

        complete_tick = std::max(complete_tick, response_tick);
    }

    return std::max(complete_tick, issue_tick + cyclesToTicks(xc, 1));
}

Tick
MatrixController::taskLatencyTicks(ExecContext *xc, const TaskDesc &task) const
{
    uint64_t cycles = 1;
    switch (task.op) {
      case TaskOp::LoadStore:
        panic("matrix load/store timing must use scheduleMemoryPipeline");
        break;
      case TaskOp::Mma:
        cycles = timingConfig.computeBaseCycles;
        break;
      case TaskOp::Arith:
        cycles = timingConfig.zeroCycles;
        break;
      case TaskOp::Release:
        cycles = timingConfig.releaseCycles;
        break;
    }

    return cyclesToTicks(xc, std::max<uint64_t>(cycles, 1));
}

Tick
MatrixController::taskSourceReadDoneTick(
    ExecContext *xc, const TaskDesc &task, Tick issue_tick,
    Tick complete_tick, size_t src_idx) const
{
    if (task.op == TaskOp::Mma && src_idx < 2) {
        return std::min(complete_tick,
            issue_tick + cyclesToTicks(xc, timingConfig.computeReadCycles));
    }

    if (task.op == TaskOp::Mma && src_idx == 2) {
        return std::min(complete_tick,
            issue_tick + cyclesToTicks(xc, timingConfig.computeReadCycles));
    }

    return complete_tick;
}

Tick
MatrixController::cpuCycleTicks(ExecContext *xc) const
{
#ifdef UNIT_TEST
    return 1;
#else
    if (xc != nullptr && xc->tcBase() != nullptr &&
        xc->tcBase()->getCpuPtr() != nullptr) {
        return xc->tcBase()->getCpuPtr()->clockPeriod();
    }
    return 1;
#endif
}

Tick
MatrixController::cyclesToTicks(ExecContext *xc, uint64_t cycles) const
{
    return cpuCycleTicks(xc) * cycles;
}

Tick
MatrixController::peekLocalMmuIssueSlot(
    ExecContext *xc, Tick earliest_tick) const
{
    return peekIssueSlot(earliest_tick,
        timing.localMmuIssueSlotTick, timing.localMmuIssueSlotsUsed,
        timingConfig.localMmuIssuePerCycle, cyclesToTicks(xc, 1));
}

Tick
MatrixController::reserveLocalMmuIssueSlot(
    ExecContext *xc, Tick earliest_tick)
{
    return reserveIssueSlot(earliest_tick,
        timing.localMmuIssueSlotTick, timing.localMmuIssueSlotsUsed,
        timingConfig.localMmuIssuePerCycle, cyclesToTicks(xc, 1));
}

Tick
MatrixController::reserveLocalMmuResponseSlot(
    ExecContext *xc, Tick earliest_tick)
{
    const Tick response_tick =
        std::max(earliest_tick, timing.localMmuResponseSlotTick);
    timing.localMmuResponseSlotTick = response_tick +
        cyclesToTicks(xc, timingConfig.l2ResponsePipelineCycles);
    return response_tick;
}

uint8_t
MatrixController::chooseTimingLocalMmuSource(Tick earliest_tick) const
{
    uint8_t source = 0;
    Tick selected_ready_tick = MaxTick;
    bool found_ready_source = false;

    for (uint32_t i = 0; i < LocalMmuSourceCount; ++i) {
        const Tick ready_tick = timing.localMmuSourceReadyTick[i];
        if (ready_tick <= earliest_tick) {
            source = static_cast<uint8_t>(i);
            selected_ready_tick = ready_tick;
            found_ready_source = true;
            continue;
        }

        if (!found_ready_source &&
            (ready_tick < selected_ready_tick ||
             (ready_tick == selected_ready_tick && i > source))) {
            source = static_cast<uint8_t>(i);
            selected_ready_tick = ready_tick;
        }
    }

    return source;
}

uint32_t
MatrixController::timingLocalMmuOutstanding(Tick tick) const
{
    uint32_t count = 0;
    for (Tick ready_tick : timing.localMmuSourceReadyTick) {
        if (ready_tick > tick) {
            ++count;
        }
    }
    return count;
}

void
MatrixController::retireReadyTokens(Tick now)
{
    for (size_t i = 0; i < TokenCount; ++i) {
        auto &ready_ticks = timing.tokenReadyTicks[i];
        const auto ready_end =
            std::upper_bound(ready_ticks.begin(), ready_ticks.end(), now);
        const size_t ready_count = ready_end - ready_ticks.begin();
        if (ready_count == 0) {
            continue;
        }

        token(i) += ready_count;
        ready_ticks.erase(ready_ticks.begin(), ready_end);
    }
}

void
MatrixController::enqueueTokenRelease(uint64_t token_idx, Tick ready_tick)
{
    panic_if(token_idx >= TokenCount,
        "matrix token index %llu out of range",
        static_cast<unsigned long long>(token_idx));

    auto &ready_ticks = timing.tokenReadyTicks[token_idx];
    ready_ticks.insert(std::upper_bound(
        ready_ticks.begin(), ready_ticks.end(), ready_tick), ready_tick);
    ++stats.tokenReleaseEvents;
}

void
MatrixController::delayPendingTokenEvents(Tick ready_tick)
{
    if (ready_tick == 0) {
        return;
    }

    for (auto &ready_ticks : timing.tokenReadyTicks) {
        for (Tick &tick : ready_ticks) {
            if (tick >= ready_tick) {
                continue;
            }

            stats.tokenReleaseDelayTicks += ready_tick - tick;
            tick = ready_tick;
            ++stats.tokenReleaseDelayEvents;
        }
        std::sort(ready_ticks.begin(), ready_ticks.end());
    }
}

Tick
MatrixController::tokenTargetReadyTick(uint64_t token_idx, uint64_t target) const
{
    panic_if(token_idx >= TokenCount,
        "matrix token index %llu out of range",
        static_cast<unsigned long long>(token_idx));

    const RegVal observed = token(token_idx);
    if (observed >= target) {
        return curTick();
    }

    const uint64_t pending_needed = target - observed;
    const auto &ready_ticks = timing.tokenReadyTicks[token_idx];
    if (pending_needed == 0 || pending_needed > ready_ticks.size()) {
        return MaxTick;
    }
    return ready_ticks[pending_needed - 1];
}

uint16_t
MatrixController::pendingTokenEvents() const
{
    size_t pending = 0;
    for (const auto &ready_ticks : timing.tokenReadyTicks) {
        pending += ready_ticks.size();
    }
    return pending > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(pending);
}

void
MatrixController::beginTask(const TaskDesc &task)
{
    panic_if(queueFull(), "matrix decoded FIFO full");

    TaskDesc queued = task;
    assignFifoIdx(queued);
    pushTask(queued);
    const TaskDesc &head = headTask();
    panic_if(!canIssue(head), "matrix controller task cannot issue");

    reserveTask(head);
    recordIssue(head);
}

void
MatrixController::completeTask()
{
    panic_if(queueEmpty(), "matrix task completion with empty decoded FIFO");

    const TaskDesc &task = headTask();
    recordCompletion(task);
    releaseTask(task);
    ++stats.tasksCompleted;
    popTask();
}

void
MatrixController::abortTask()
{
    panic_if(queueEmpty(), "matrix task abort with empty decoded FIFO");

    const TaskDesc &task = headTask();
    releaseTask(task);
    ++stats.tasksAborted;
    popTask();
}

void
MatrixController::assignFifoIdx(TaskDesc &task)
{
    switch (task.op) {
      case TaskOp::Mma:
        task.fifoIdx = control.nextComputeFifoIdx;
        control.nextComputeFifoIdx =
            (control.nextComputeFifoIdx + 1) & (MicroTaskFifoSlots - 1);
        break;
      case TaskOp::LoadStore:
        if (task.isStore) {
            task.fifoIdx = control.nextStoreFifoIdx;
            control.nextStoreFifoIdx =
                (control.nextStoreFifoIdx + 1) & (MicroTaskFifoSlots - 1);
        } else {
            task.fifoIdx = control.nextLoadFifoIdx;
            control.nextLoadFifoIdx =
                (control.nextLoadFifoIdx + 1) & (MicroTaskFifoSlots - 1);
        }
        break;
      case TaskOp::Arith:
        task.fifoIdx = control.nextLoadFifoIdx;
        control.nextLoadFifoIdx =
            (control.nextLoadFifoIdx + 1) & (MicroTaskFifoSlots - 1);
        break;
      case TaskOp::Release:
        task.fifoIdx = 0;
        break;
    }
}

bool
MatrixController::queueFull() const
{
    return control.queueSize == DecodedQueueDepth;
}

bool
MatrixController::queueEmpty() const
{
    return control.queueSize == 0;
}

void
MatrixController::pushTask(const TaskDesc &task)
{
    const uint8_t tail = (control.queueHead + control.queueSize) &
        (DecodedQueueDepth - 1);
    control.decodedQueue[tail] = task;
    ++control.queueSize;
}

MatrixController::TaskDesc &
MatrixController::headTask()
{
    return control.decodedQueue[control.queueHead];
}

const MatrixController::TaskDesc &
MatrixController::headTask() const
{
    return control.decodedQueue[control.queueHead];
}

void
MatrixController::popTask()
{
    panic_if(queueEmpty(), "matrix decoded FIFO pop on empty queue");

    control.queueHead = (control.queueHead + 1) & (DecodedQueueDepth - 1);
    --control.queueSize;
}

bool
MatrixController::canIssue(const TaskDesc &task) const
{
    if (task.op == TaskOp::Release) {
        return !control.pendingStore;
    }

    if (fuBusy(task.fu)) {
        return false;
    }

    if (task.destValid &&
        (regBusy(task.destIsAcc, task.destReg) ||
         regHasPendingReaders(task.destIsAcc, task.destReg))) {
        return false;
    }

    for (size_t i = 0; i < task.srcValid.size(); ++i) {
        if (task.srcValid[i] && regBusy(task.srcIsAcc[i], task.srcReg[i])) {
            return false;
        }
    }

    if (task.isStore && task.srcValid[0] &&
        regHasPendingReaders(task.srcIsAcc[0], task.srcReg[0])) {
        return false;
    }

    return true;
}

void
MatrixController::reserveTask(const TaskDesc &task)
{
    if (task.fu != FuType::None) {
        setFuBusy(task.fu, true);
        control.fus[fuIndex(task.fu)].fifoIdx = task.fifoIdx;
    }

    if (task.destValid) {
        reserveReg(task.destIsAcc, task.destReg, task.fu);
    }

    for (size_t i = 0; i < task.srcValid.size(); ++i) {
        if (task.srcValid[i] && task.srcReadPending[i]) {
            addRegReader(task.srcIsAcc[i], task.srcReg[i]);
        }
    }

    if (task.isStore) {
        control.pendingStore = true;
    }
}

void
MatrixController::releaseTask(const TaskDesc &task)
{
    for (size_t i = 0; i < task.srcValid.size(); ++i) {
        if (task.srcValid[i] && task.srcReadPending[i]) {
            removeRegReader(task.srcIsAcc[i], task.srcReg[i]);
        }
    }

    if (task.destValid) {
        releaseReg(task.destIsAcc, task.destReg, task.fu);
    }

    if (task.isStore) {
        control.pendingStore = false;
    }

    if (task.fu != FuType::None) {
        setFuBusy(task.fu, false);
    }
}

bool
MatrixController::regBusy(bool is_acc, uint8_t reg_idx) const
{
    const uint8_t idx = normalizeRegIdx(reg_idx);
    return is_acc ? control.cRegs[idx].busy : control.abRegs[idx].busy;
}

bool
MatrixController::regHasPendingReaders(bool is_acc, uint8_t reg_idx) const
{
    const uint8_t idx = normalizeRegIdx(reg_idx);
    return is_acc ? control.cRegs[idx].pendingReaders != 0 :
        control.abRegs[idx].pendingReaders != 0;
}

void
MatrixController::reserveReg(bool is_acc, uint8_t reg_idx, FuType fu)
{
    RegStatus &status = is_acc ? control.cRegs[normalizeRegIdx(reg_idx)] :
        control.abRegs[normalizeRegIdx(reg_idx)];
    panic_if(status.busy, "matrix register reserved while busy");
    status.busy = true;
    status.producer = fu;
}

void
MatrixController::releaseReg(bool is_acc, uint8_t reg_idx, FuType fu)
{
    RegStatus &status = is_acc ? control.cRegs[normalizeRegIdx(reg_idx)] :
        control.abRegs[normalizeRegIdx(reg_idx)];
    panic_if(!status.busy, "matrix register released while idle");
    panic_if(status.producer != fu, "matrix register producer mismatch");
    status.busy = false;
    status.producer = FuType::None;
}

void
MatrixController::addRegReader(bool is_acc, uint8_t reg_idx)
{
    RegStatus &status = is_acc ? control.cRegs[normalizeRegIdx(reg_idx)] :
        control.abRegs[normalizeRegIdx(reg_idx)];
    panic_if(status.pendingReaders == UINT8_MAX,
        "matrix register pending reader count overflow");
    ++status.pendingReaders;
}

void
MatrixController::removeRegReader(bool is_acc, uint8_t reg_idx)
{
    RegStatus &status = is_acc ? control.cRegs[normalizeRegIdx(reg_idx)] :
        control.abRegs[normalizeRegIdx(reg_idx)];
    panic_if(status.pendingReaders == 0,
        "matrix register pending reader count underflow");
    --status.pendingReaders;
}

bool
MatrixController::fuBusy(FuType fu) const
{
    if (fu == FuType::None) {
        return false;
    }
    return control.fus[fuIndex(fu)].busy;
}

void
MatrixController::setFuBusy(FuType fu, bool busy)
{
    if (fu == FuType::None) {
        return;
    }

    FuStatus &status = control.fus[fuIndex(fu)];
    panic_if(status.busy == busy, "matrix FU busy state transition invalid");
    status.busy = busy;
}

void
MatrixController::recordIssue(const TaskDesc &task)
{
    ++stats.tasksAccepted;
    ++stats.tasksIssued;

    switch (task.op) {
      case TaskOp::Mma:
        ++stats.mmaTasks;
        ++stats.taskEvents[taskEventIndex(TaskEvent::ComputeIssue)];
        break;
      case TaskOp::Arith:
        ++stats.zeroTasks;
        ++stats.taskEvents[taskEventIndex(TaskEvent::LoadAllocate)];
        ++stats.taskEvents[taskEventIndex(TaskEvent::LoadIssue)];
        break;
      case TaskOp::Release:
        ++stats.releaseTasks;
        ++stats.taskEvents[taskEventIndex(TaskEvent::ReleaseIssue)];
        break;
      case TaskOp::LoadStore:
        switch (task.memPort) {
          case MemPort::A:
            ++stats.aPortTasks;
            break;
          case MemPort::B:
            ++stats.bPortTasks;
            break;
          case MemPort::CLoad:
            ++stats.cLoadTasks;
            break;
          case MemPort::CStore:
            ++stats.cStoreTasks;
            break;
          default:
            break;
        }
        if (task.isStore) {
            ++stats.taskEvents[taskEventIndex(TaskEvent::StoreIssue)];
        } else {
            ++stats.taskEvents[taskEventIndex(TaskEvent::LoadAllocate)];
            ++stats.taskEvents[taskEventIndex(TaskEvent::LoadIssue)];
        }
        recordMemoryRequests(task);
        break;
    }
}

void
MatrixController::recordCompletion(const TaskDesc &task)
{
    switch (task.op) {
      case TaskOp::Mma:
        stats.taskEvents[taskEventIndex(TaskEvent::ComputeReadFinish)] += 2;
        ++stats.taskEvents[taskEventIndex(TaskEvent::ComputeWriteFinish)];
        break;
      case TaskOp::Arith:
        ++stats.taskEvents[taskEventIndex(TaskEvent::LoadFinish)];
        break;
      case TaskOp::LoadStore:
        if (task.isStore) {
            ++stats.taskEvents[taskEventIndex(TaskEvent::StoreFinish)];
        } else {
            ++stats.taskEvents[taskEventIndex(TaskEvent::LoadFinish)];
        }
        break;
      case TaskOp::Release:
        break;
    }
}

void
MatrixController::recordMemoryRequests(const TaskDesc &task)
{
    if (task.memoryRequests == 0 || task.memPort == MemPort::Num) {
        return;
    }

    stats.memoryRequests += task.memoryRequests;
    stats.memoryBytes += task.memoryBytes;
    stats.memoryBusBytes += task.memoryBusBytes;

    if (task.isStore) {
        stats.memoryWriteRequests += task.memoryRequests;
        stats.memoryWriteBytes += task.memoryBytes;
        stats.memoryWriteBusBytes += task.memoryBusBytes;
    } else {
        stats.memoryReadRequests += task.memoryRequests;
        stats.memoryReadBytes += task.memoryBytes;
        stats.memoryReadBusBytes += task.memoryBusBytes;
    }

    std::array<uint8_t, LocalMmuSourceCount> inflight_sources = {};
    uint32_t inflight_count = 0;

    for (uint64_t i = 0; i < task.memoryRequests; ++i) {
        const MemPort selected_port = arbitrateLocalMmuPort(task.memPort);
        ++stats.memPortRequests[memPortIndex(selected_port)];
        inflight_sources[inflight_count++] =
            allocateLocalMmuSource(selected_port);

        if (inflight_count == LocalMmuSourceCount) {
            for (uint32_t j = 0; j < inflight_count; ++j) {
                completeLocalMmuRequest(inflight_sources[j], task.isStore);
            }
            inflight_count = 0;
        }
    }

    for (uint32_t i = 0; i < inflight_count; ++i) {
        completeLocalMmuRequest(inflight_sources[i], task.isStore);
    }
}

MatrixController::MemPort
MatrixController::arbitrateLocalMmuPort(MemPort requested)
{
    const uint8_t requested_idx = static_cast<uint8_t>(memPortIndex(requested));

    for (uint8_t offset = 0; offset < MemPortCount; ++offset) {
        const uint8_t candidate = static_cast<uint8_t>(
            (control.firstMmuRequestIndex + offset) % MemPortCount);
        if (candidate == requested_idx) {
            control.firstMmuRequestIndex =
                static_cast<uint8_t>((candidate + 1) % MemPortCount);
            ++stats.localMmuArbitrations;
            ++stats.localMmuPortSelections[candidate];
            return requested;
        }
    }

    panic("matrix LocalMMU arbitration missed requested port");
}

void
MatrixController::completeLocalMmuRequest(uint8_t source, bool is_store)
{
    panic_if(source >= LocalMmuSourceCount,
        "matrix LocalMMU response source out of range");
    panic_if(!control.sourceBusy[source],
        "matrix LocalMMU response for idle source");

    const MemPort port = control.sourceToPort[source];
    panic_if(port == MemPort::Num,
        "matrix LocalMMU response has no routed port");

    const size_t port_idx = memPortIndex(port);
    if (is_store) {
        ++stats.localMmuWriteAcks;
        ++stats.memPortWriteAcks[port_idx];
        ++stats.taskEvents[taskEventIndex(TaskEvent::MemoryWriteAckResponse)];
    } else {
        ++stats.localMmuReadDataResponses;
        ++stats.memPortReadDataResponses[port_idx];
        ++stats.taskEvents[taskEventIndex(TaskEvent::MemoryReadDataResponse)];
    }

    releaseLocalMmuSource(source);
}

uint8_t
MatrixController::allocateLocalMmuSource(MemPort port)
{
    const uint8_t source = selectHighestFreeSource(control.sourceBusy);
    panic_if(source >= LocalMmuSourceCount,
        "matrix LocalMMU has no free source id");

    control.sourceBusy[source] = true;
    control.sourceToPort[source] = port;
    ++control.localMmuOutstanding;
    ++stats.localMmuSourceAllocations;
    stats.localMmuMaxOutstanding = std::max<uint64_t>(
        static_cast<uint64_t>(stats.localMmuMaxOutstanding.value()),
        control.localMmuOutstanding);
    return source;
}

void
MatrixController::releaseLocalMmuSource(uint8_t source)
{
    panic_if(source >= LocalMmuSourceCount,
        "matrix LocalMMU release source out of range");
    panic_if(!control.sourceBusy[source],
        "matrix LocalMMU release for idle source");
    panic_if(control.localMmuOutstanding == 0,
        "matrix LocalMMU outstanding count underflow");

    control.sourceBusy[source] = false;
    control.sourceToPort[source] = MemPort::Num;
    --control.localMmuOutstanding;
    ++stats.localMmuSourceReleases;
}

uint8_t
MatrixController::normalizeRegIdx(uint64_t reg_idx)
{
    return static_cast<uint8_t>(reg_idx & (MatrixRegCount - 1));
}

uint32_t
MatrixController::clampTileM(uint64_t value)
{
    return value > MatrixMaxM ? MatrixMaxM : static_cast<uint32_t>(value);
}

uint32_t
MatrixController::clampTileK(uint64_t value)
{
    return value > MatrixMaxK ? MatrixMaxK : static_cast<uint32_t>(value);
}

uint32_t
MatrixController::clampTileN(uint64_t value)
{
    return value > MatrixMaxN ? MatrixMaxN : static_cast<uint32_t>(value);
}

uint64_t
MatrixController::divCeil(uint64_t numerator, uint64_t denominator)
{
    return numerator == 0 ? 0 : (numerator + denominator - 1) / denominator;
}

MatrixController::MemoryRequestShape
MatrixController::matrixMemoryRequestShape(
    MemPort port, bool transpose, uint32_t rows, uint32_t cols,
    ElemWidth width)
{
    const uint32_t elem_bytes = elemWidthBytes(width);
    if (rows == 0 || cols == 0) {
        return {};
    }

    if (port == MemPort::A || port == MemPort::B || port == MemPort::CLoad) {
        return {rows, static_cast<uint64_t>(cols) * elem_bytes};
    }

    if (port == MemPort::CStore) {
        const uint64_t major_dim = transpose ? cols : rows;
        const uint64_t reduce_dim = transpose ? rows : cols;
        const uint64_t rounded_major =
            divCeil(major_dim, MatrixMN) * MatrixMN;
        return {rounded_major, reduce_dim * elem_bytes};
    }

    return {rows, static_cast<uint64_t>(cols) * elem_bytes};
}

uint64_t
MatrixController::matrixMemoryRequests(MemPort port, bool transpose,
    Addr base, Addr stride, uint32_t rows, uint32_t cols, ElemWidth width)
{
    const MemoryRequestShape shape =
        matrixMemoryRequestShape(port, transpose, rows, cols, width);
    return rowRequests(base, stride, shape.rows, shape.bytesPerRow);
}

uint64_t
MatrixController::rowRequests(
    Addr base, Addr stride, uint64_t rows, uint64_t bytes_per_row)
{
    if (rows == 0 || bytes_per_row == 0) {
        return 0;
    }

    uint64_t requests = 0;
    for (uint64_t row = 0; row < rows; ++row) {
        const Addr row_base = base + static_cast<Addr>(row) * stride;
        const uint64_t offset = row_base & (OutsideDataWidthBytes - 1);
        requests += divCeil(offset + bytes_per_row, OutsideDataWidthBytes);
    }
    return requests;
}

uint32_t
MatrixController::elemWidthBytes(ElemWidth width)
{
    switch (width) {
      case ElemWidth::E8:
        return 1;
      case ElemWidth::E16:
        return 2;
      case ElemWidth::E32:
        return 4;
      case ElemWidth::E64:
        return 8;
    }

    panic("unsupported matrix element width");
}

size_t
MatrixController::fuIndex(FuType fu)
{
    return static_cast<size_t>(fu);
}

size_t
MatrixController::memPortIndex(MemPort port)
{
    return static_cast<size_t>(port);
}

size_t
MatrixController::taskEventIndex(TaskEvent event)
{
    return static_cast<size_t>(event);
}

} // namespace matrix
} // namespace gem5

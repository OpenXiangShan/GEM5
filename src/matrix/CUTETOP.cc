/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "matrix/CUTETOP.hh"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <optional>
#include <utility>

#include "cpu/thread_context.hh"
#include "matrix/MemoryLoader.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"

namespace gem5
{

namespace matrix
{

namespace
{

constexpr unsigned MatrixTileMn = 8;
constexpr unsigned Int8KGroup = 32;

MatrixBankKind
lsuMatrixBank(const AmuLsuDesc &desc)
{
    if (desc.isAcc) {
        return MatrixBankKind::C;
    }
    if (desc.isB) {
        return MatrixBankKind::B;
    }
    return MatrixBankKind::A;
}

MatrixL2FillTable::Request
fillTableRequestForResponse(const LocalMmuModel::Response &response,
                            unsigned fill_chunks)
{
    MatrixL2FillTable::Request request;
    request.client = response.client;
    request.destBank = response.metadata.destBank;
    request.fillChunks = fill_chunks;
    request.targetBank = response.beatIndex %
                         MatrixRegResource::NumBanks;
    return request;
}

CuteCompletion
makeCompletion(uint64_t seq, CuteRequestKind kind, CuteCompletionStatus status)
{
    CuteCompletion completion;
    completion.seq = seq;
    completion.kind = kind;
    completion.status = status;
    return completion;
}

bool
isInt8TileWritebackMma(const AmuMmaDesc &desc)
{
    const bool int8_tags =
        desc.lhsElemType == MatrixElemType::Int8 &&
        desc.rhsElemType == MatrixElemType::Int8 &&
        desc.dstElemType == MatrixElemType::Int32;
    const bool int8_encodings =
        (desc.types1 == 0 || desc.types1 == 0x4) &&
        (desc.types2 == 0 || desc.types2 == 0x4) &&
        (desc.typed == 0 || desc.typed == 0x2);
    return !desc.isFp && int8_tags && int8_encodings &&
           desc.mtilem % MatrixTileMn == 0 &&
           desc.mtilen % MatrixTileMn == 0 &&
           desc.mtilek % Int8KGroup == 0;
}

CuteCompletion
execRelease(uint64_t seq, const AmuReleaseDesc &desc)
{
    CuteCompletion completion =
        makeCompletion(seq, CuteRequestKind::Release,
                       CuteCompletionStatus::Success);
    completion.hasTokenRelease = true;
    completion.tokenIdx = desc.tokenIndex;
    return completion;
}

} // anonymous namespace

// Active backend register, memory, and writeback helpers.
DetailedCuteBackend::CutePhaseStats::CutePhaseStats(statistics::Group *parent)
    : statistics::Group(parent, "matrix_cute"),
      ADD_STAT(matrixLocalMmuOutstandingMax,
               statistics::units::Count::get(),
               "Maximum LocalMMU external source occupancy observed by "
               "CUTE"),
      ADD_STAT(matrixLocalMmuOutstandingFullCycles,
               statistics::units::Cycle::get(),
               "CUTE cycles where LocalMMU external source occupancy is "
               "full"),
      ADD_STAT(matrixL2FillTableReservedMax,
               statistics::units::Count::get(),
               "Maximum Matrix L2 fill table entries reserved"),
      ADD_STAT(matrixL2FillBankFifoOccupancyMax,
               statistics::units::Count::get(),
               "Maximum Matrix L2 per-bank fill FIFO occupancy"),
      ADD_STAT(matrixL2FillAcceptedResponses,
               statistics::units::Count::get(),
               "Matrix L2 fill responses accepted into the fill table"),
      ADD_STAT(matrixL2FillAcceptedChunks,
               statistics::units::Count::get(),
               "Matrix L2 fill chunks accepted into the fill table"),
      ADD_STAT(matrixL2FillDrainCycles,
               statistics::units::Cycle::get(),
               "CUTE cycles with at least one Matrix L2 fill chunk drained"),
      ADD_STAT(matrixL2FillDrainChunks,
               statistics::units::Count::get(),
               "Matrix L2 fill chunks drained into matrix registers"),
      ADD_STAT(matrixL2FillDrainChunksPerCycleMax,
               statistics::units::Count::get(),
               "Maximum Matrix L2 fill chunks drained in one CUTE cycle"),
      ADD_STAT(matrixCuteALoadFillTableFullBlocked,
               statistics::units::Count::get(),
               "CUTE ALoad responses blocked by full matrix fill table"),
      ADD_STAT(matrixCuteBLoadFillTableFullBlocked,
               statistics::units::Count::get(),
               "CUTE BLoad responses blocked by full matrix fill table"),
      ADD_STAT(matrixCuteALoadFillBankFifoFullBlocked,
               statistics::units::Count::get(),
               "CUTE ALoad responses blocked by full fill bank FIFO"),
      ADD_STAT(matrixCuteBLoadFillBankFifoFullBlocked,
               statistics::units::Count::get(),
               "CUTE BLoad responses blocked by full fill bank FIFO"),
      ADD_STAT(matrixCuteALoadMatrixRegWriteBlocked,
               statistics::units::Count::get(),
               "CUTE ALoad matrix register write attempts blocked"),
      ADD_STAT(matrixCuteBLoadMatrixRegWriteBlocked,
               statistics::units::Count::get(),
               "CUTE BLoad matrix register write attempts blocked")
{
}

void
DetailedCuteBackend::recordLocalMmuOutstanding()
{
    const auto occupancy = localMmu.outstandingCount();
    if (occupancy > cutePhaseStats.matrixLocalMmuOutstandingMax.value()) {
        cutePhaseStats.matrixLocalMmuOutstandingMax = occupancy;
    }
    if (localMmu.outstandingFull()) {
        ++cutePhaseStats.matrixLocalMmuOutstandingFullCycles;
    }
}

void
DetailedCuteBackend::recordMatrixL2FillOccupancy()
{
    const auto reserved = matrixL2FillTable.reservedCount();
    if (reserved > cutePhaseStats.matrixL2FillTableReservedMax.value()) {
        cutePhaseStats.matrixL2FillTableReservedMax = reserved;
    }

    for (unsigned bank = 0; bank < MatrixRegResource::NumBanks; ++bank) {
        const auto occupancy = matrixL2FillTable.bankFifoOccupancy(bank);
        if (occupancy > cutePhaseStats.matrixL2FillBankFifoOccupancyMax.value()) {
            cutePhaseStats.matrixL2FillBankFifoOccupancyMax = occupancy;
        }
    }
}

bool
DetailedCuteBackend::useMemoryBudget()
{
    if (memoryBudget == 0) {
        return false;
    }
    --memoryBudget;
    return true;
}

MatrixBankKind
DetailedCuteBackend::destBank(const DecodedFifoEntry &entry) const
{
    if (entry.isMma || entry.isZeroAcc) {
        return MatrixBankKind::C;
    }
    if (entry.isZeroTr) {
        return MatrixBankKind::A;
    }
    if (entry.isLoad) {
        if (entry.request.lsu.isAcc) {
            return MatrixBankKind::C;
        }
        return entry.request.lsu.isB ? MatrixBankKind::B : MatrixBankKind::A;
    }
    return MatrixBankKind::C;
}

CuteCompletion
DetailedCuteBackend::executeArith(uint64_t seq, const AmuArithDesc &desc,
                                  MatrixRegFile &state)
{
    switch (desc.op) {
      case MatrixArithOpcode::Zero:
        // An empty active rectangle is a no-op. Do not replace an existing
        // architectural register with an empty tensor and a different shape.
        if (desc.rows && desc.cols) {
            state.zero(desc.bank, desc.reg, desc.rows, desc.cols, desc.elemType);
        }
        return makeCompletion(seq, CuteRequestKind::Arith,
                              CuteCompletionStatus::Success);
    }

    return makeCompletion(seq, CuteRequestKind::Arith,
                          CuteCompletionStatus::Unsupported);
}

CuteCompletion
DetailedCuteBackend::executeTaskSlot(const TaskSlot &task)
{
    const auto &entry = task.entry;
    if (entry.isZeroAcc || entry.isZeroTr) {
        return executeArith(entry.request.seq, entry.request.arith, regFile);
    }

    assert(entry.isRelease);
    return execRelease(entry.request.seq, entry.request.release);
}

bool
DetailedCuteBackend::advanceZeroAccDelay(TaskSlot &task)
{
    assert(task.entry.isZeroAcc);
    if (task.zeroCyclesRemaining > 1) {
        --task.zeroCyclesRemaining;
        return false;
    }

    task.zeroCyclesRemaining = 0;
    return true;
}

CuteCompletion
DetailedCuteBackend::executeLoadWrite(TaskSlot &task)
{
    assert(task.entry.isLoad);

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return task.bufferedCompletion;
    }

    if (!task.hasBufferedTensor) {
        return makeCompletion(task.entry.request.seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
    }

    regFile.write(destBank(task.entry), task.entry.writeRegs[0],
                  task.bufferedTensor);
    return makeCompletion(task.entry.request.seq, CuteRequestKind::Lsu,
                          CuteCompletionStatus::Success);
}

CuteCompletion
DetailedCuteBackend::executeStoreWrite(const TaskSlot &task)
{
    assert(task.entry.isStore);

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return task.bufferedCompletion;
    }

    if (!useTimingMemory()) {
        return makeCompletion(task.entry.request.seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
    }

    return makeCompletion(task.entry.request.seq, CuteRequestKind::Lsu,
                          CuteCompletionStatus::Success);
}

CuteCompletion
DetailedCuteBackend::executeComputeWrite(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return task.bufferedCompletion;
    }

    if (!task.hasBufferedTensor) {
        return makeCompletion(task.entry.request.seq, CuteRequestKind::Mma,
                              CuteCompletionStatus::Unsupported);
    }

    regFile.write(MatrixBankKind::C, task.entry.writeRegs[0],
                  task.bufferedTensor);
    return makeCompletion(task.entry.request.seq, CuteRequestKind::Mma,
                          CuteCompletionStatus::Success);
}

void
DetailedCuteBackend::initializeTimingLoadBuffer(TaskSlot &task)
{
    if (!task.entry.isLoad || !task.lsuLoadBytes.empty()) {
        return;
    }

    const auto &desc = task.entry.request.lsu;
    panic_if(desc.tc && !desc.pageMappings,
             "Committed Matrix load lacks frozen page translations");
    task.lsuLoadPlan = buildTimingLoadPlan(
        desc, frozenMatrixTranslator(desc.pageMappings));
    task.lsuLoadBytes.assign(task.lsuLoadPlan.tensorBytes, 0);
}

void
DetailedCuteBackend::initializeTimingStoreBuffer(TaskSlot &task)
{
    if (!task.entry.isStore || task.lsuStorePlanInitialized) {
        return;
    }

    const auto &desc = task.entry.request.lsu;
    panic_if(desc.tc && !desc.pageMappings,
             "Committed Matrix store lacks frozen page translations");
    if (!task.hasBufferedTensor) {
        task.lsuStorePlan = {};
        task.lsuStorePlanInitialized = true;
        return;
    }

    task.lsuStorePlan = buildTimingStorePlan(
        desc, task.bufferedTensor,
        frozenMatrixTranslator(desc.pageMappings));
    task.lsuStorePlanInitialized = true;
}

bool
DetailedCuteBackend::recordTimingLoadResponse(
    TaskSlot &task, const LocalMmuModel::Response &response)
{
    if (!task.entry.isLoad || !response.hasData) {
        return false;
    }

    initializeTimingLoadBuffer(task);
    if (!scatterTimingLoadResponse(
            task.lsuLoadPlan, response.beatIndex, response.data.data(),
            response.dataSize, task.lsuLoadBytes)) {
        return false;
    }

    return true;
}

bool
DetailedCuteBackend::buildTensorFromTimingLoadData(TaskSlot &task)
{
    assert(task.entry.isLoad);
    if (task.lsuLoadBytes.size() != task.lsuLoadPlan.tensorBytes) {
        return false;
    }

    const auto &desc = task.entry.request.lsu;
    const size_t bytes_per_elem = elemBytes(desc.elemType);
    const size_t element_count =
        static_cast<size_t>(desc.row) * desc.column;
    if (bytes_per_elem == 0 ||
        task.lsuLoadBytes.size() != element_count * bytes_per_elem) {
        return false;
    }

    MatrixTensor tensor;
    tensor.rows = desc.row;
    tensor.cols = desc.column;
    tensor.elemType = desc.elemType;
    tensor.elements.reserve(element_count);

    const auto read_raw = [&](size_t byte_offset) {
        uint64_t raw = 0;
        for (size_t byte = 0; byte < bytes_per_elem; ++byte) {
            raw |= static_cast<uint64_t>(
                       task.lsuLoadBytes[byte_offset + byte])
                   << (byte * 8);
        }
        return raw;
    };

    for (size_t elem = 0; elem < element_count; ++elem) {
        const uint64_t raw = read_raw(elem * bytes_per_elem);
        int64_t value = 0;
        switch (desc.elemType) {
          case MatrixElemType::Int8:
            value = static_cast<int8_t>(raw);
            break;
          case MatrixElemType::Int16:
            value = static_cast<int16_t>(raw);
            break;
          case MatrixElemType::Int32:
            value = static_cast<int32_t>(raw);
            break;
          case MatrixElemType::Int64:
            value = static_cast<int64_t>(raw);
            break;
          case MatrixElemType::Fp16:
          case MatrixElemType::Bf16:
          case MatrixElemType::Tf32:
            value = static_cast<int64_t>(raw);
            break;
        }
        tensor.elements.push_back(value);
    }

    task.bufferedTensor = std::move(tensor);
    task.hasBufferedTensor = true;
    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Lsu,
        CuteCompletionStatus::Success);
    task.lsuLoadFinalized = true;
    return true;
}

LocalMmuModel::Client
DetailedCuteBackend::localMmuClient(const TaskSlot &task) const
{
    switch (microTaskKindForEntry(task.entry)) {
      case MicroTaskKind::AML:
        return LocalMmuModel::Client::AML;
      case MicroTaskKind::BML:
        return LocalMmuModel::Client::BML;
      case MicroTaskKind::CML:
        return LocalMmuModel::Client::CML;
      case MicroTaskKind::Compute:
      case MicroTaskKind::Release:
      case MicroTaskKind::Count:
        break;
    }

    return LocalMmuModel::Client::CML;
}

unsigned
DetailedCuteBackend::matrixL2FillChunksForResponse(uint32_t byte_size) const
{
    const unsigned entry_bytes = MatrixRegResource::EntryBytes;
    return std::max(1U, (byte_size + entry_bytes - 1) / entry_bytes);
}

bool
DetailedCuteBackend::useTimingMemory() const
{
    return timingMemory != nullptr && timingMemory->connected();
}

void
DetailedCuteBackend::issueLocalMmuTimingRequest()
{
    if (!useTimingMemory()) {
        return;
    }

    recordLocalMmuOutstanding();

    LocalMmuModel::IssuedRequest issued;
    if (!localMmu.issueExternal(issued)) {
        return;
    }

    MatrixTimingMemoryAdapter::Request request;
    request.isStore = issued.request.isStore;
    request.sourceId = issued.sourceId;
    request.matrixKey = matrixL2KeyBits(MatrixL2Key::None);

    auto attach_address = [&](std::optional<TaskSlot> &slot) {
        if (!slot.has_value()) {
            return false;
        }
        auto &task = slot.value();
        if (task.entry.request.seq != issued.request.seq ||
            localMmuClient(task) != issued.request.client) {
            return false;
        }

        const auto &desc = task.entry.request.lsu;
        const bool is_matrix_c_read = task.entry.isLoad && desc.isAcc;
        request.matrixKey = matrixL2KeyBits(
            is_matrix_c_read ? MatrixL2Key::MatrixModify :
            MatrixL2Key::Matrix);
        if (task.entry.isLoad) {
            initializeTimingLoadBuffer(task);
            if (issued.request.beatIndex >=
                task.lsuLoadPlan.beats.size()) {
                return false;
            }
            const auto &beat =
                task.lsuLoadPlan.beats[issued.request.beatIndex];
            request.paddr = beat.paddr;
            request.packetSize = beat.byteSize;
        } else {
            initializeTimingStoreBuffer(task);
            if (issued.request.beatIndex >=
                task.lsuStorePlan.beats.size()) {
                return false;
            }
            const auto &beat =
                task.lsuStorePlan.beats[issued.request.beatIndex];
            request.paddr = beat.paddr;
            request.packetSize = beat.packetSize;
            request.data = beat.lineData;
            request.byteMask = beat.byteMask;
        }
        if (desc.tc) {
            request.contextId = desc.tc->contextId();
        }
        return true;
    };

    if (!attach_address(amlTask) &&
        !attach_address(bmlTask) &&
        !attach_address(cmlTask)) {
        localMmu.completeExternalResponse(issued.sourceId);
        localMmu.releaseExternalSource(issued.sourceId);
        return;
    }

    if (!timingMemory->sendTimingRequest(request)) {
        return;
    }

}

bool
DetailedCuteBackend::enqueueLocalMmuBeats(TaskSlot &task)
{
    if (task.lsuBeatsEnqueued) {
        return true;
    }

    const auto &desc = task.entry.request.lsu;
    if (!useTimingMemory()) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
        task.lsuBeatsEnqueued = true;
        task.lsuTotalBeats = 0;
        return false;
    }

    if (task.entry.isLoad) {
        initializeTimingLoadBuffer(task);
    } else if (task.entry.isStore) {
        initializeTimingStoreBuffer(task);
    }

    const unsigned timing_beats = task.entry.isLoad ?
        static_cast<unsigned>(task.lsuLoadPlan.beats.size()) :
        static_cast<unsigned>(task.lsuStorePlan.beats.size());
    if (timing_beats == 0) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
        task.lsuBeatsEnqueued = true;
        task.lsuTotalBeats = 0;
        return false;
    }

    const MatrixBankKind matrix_bank = lsuMatrixBank(desc);
    for (unsigned beat = 0; beat < timing_beats; ++beat) {
        LocalMmuModel::Request request;
        request.seq = task.entry.request.seq;
        request.client = localMmuClient(task);
        request.isStore = task.entry.isStore;
        request.beatIndex = beat;
        request.byteSize = task.entry.isLoad ?
            task.lsuLoadPlan.beats[beat].byteSize :
            task.lsuStorePlan.beats[beat].packetSize;
        request.metadata.destBank = matrix_bank;
        if (!localMmu.enqueue(request)) {
            task.bufferedCompletion = makeCompletion(
                task.entry.request.seq, CuteRequestKind::Lsu,
                CuteCompletionStatus::Unsupported);
            task.lsuBeatsEnqueued = true;
            return false;
        }
    }

    task.lsuTotalBeats = timing_beats;
    task.lsuBeatsEnqueued = true;
    return true;
}

namespace
{

bool
isAmlClient(LocalMmuModel::Client client)
{
    return client == LocalMmuModel::Client::AML;
}

bool
isBmlClient(LocalMmuModel::Client client)
{
    return client == LocalMmuModel::Client::BML;
}

} // anonymous namespace

void
DetailedCuteBackend::serviceLsuMatrixRegWriteChunks()
{
    if (!useTimingMemory()) {
        return;
    }

    unsigned drained_chunks = 0;
    const auto retire_candidate =
        [&](const MatrixL2FillTable::DrainCandidate &candidate) {
            const bool retired = matrixL2FillTable.retireDrain(candidate);
            assert(retired);
            ++cutePhaseStats.matrixL2FillDrainChunks;
            ++drained_chunks;
            recordMatrixL2FillOccupancy();
        };

    for (unsigned bank = 0; bank < MatrixRegResource::NumBanks; ++bank) {
        const auto candidate = matrixL2FillTable.drainCandidate(bank);
        if (!candidate.has_value()) {
            continue;
        }

        auto write_request = MatrixRegResource::makeWrite(
            candidate->destBank, MatrixRegResource::Client::MemoryLoader);
        write_request.bankMask = 1U << candidate->targetBank;

        if (candidate->destBank == MatrixBankKind::C) {
            retire_candidate(*candidate);
        } else {
            const auto grants = matrixRegResource.arbitrate({write_request});
            assert(grants.size() == 1);
            if (!grants[0].granted) {
                if (isAmlClient(candidate->client)) {
                    ++cutePhaseStats.matrixCuteALoadMatrixRegWriteBlocked;
                } else if (isBmlClient(candidate->client)) {
                    ++cutePhaseStats.matrixCuteBLoadMatrixRegWriteBlocked;
                }
                continue;
            }
            retire_candidate(*candidate);
        }

    }

    if (drained_chunks != 0) {
        ++cutePhaseStats.matrixL2FillDrainCycles;
        if (drained_chunks >
            cutePhaseStats.matrixL2FillDrainChunksPerCycleMax.value()) {
            cutePhaseStats.matrixL2FillDrainChunksPerCycleMax =
                drained_chunks;
        }
    }
}

void
DetailedCuteBackend::advanceLoadFill(TaskSlot &task)
{
    assert(task.entry.isLoad);

    if (!task.lsuBeatsEnqueued && !enqueueLocalMmuBeats(task)) {
        return;
    }

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return;
    }

    if (matrixL2FillTable.hasPendingFillChunks(localMmuClient(task))) {
        return;
    }

    if (task.lsuResponsesReceived < task.lsuTotalBeats) {
        return;
    }

    if (!task.lsuLoadFinalized) {
        task.hasBufferedTensor = false;
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Success);

        if (buildTensorFromTimingLoadData(task)) {
            return;
        }

        task.bufferedCompletion.status =
            CuteCompletionStatus::Unsupported;
        task.lsuLoadFinalized = true;
    }
}

void
DetailedCuteBackend::advanceStoreRead(TaskSlot &task)
{
    assert(task.entry.isStore);

    if (!useMemoryBudget()) {
        return;
    }

    task.hasBufferedTensor = false;
    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Lsu,
        CuteCompletionStatus::Success);

    if (!regFile.hasRegister(MatrixBankKind::C, task.entry.readRegs[0])) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else {
        task.bufferedTensor =
            regFile.read(MatrixBankKind::C, task.entry.readRegs[0]);
        task.hasBufferedTensor = true;
    }

    enqueueLocalMmuBeats(task);

    enqueueTaskEvent(task, TaskEventKind::ReadFinish);
}

// Active runtime path: task slots, task events, and final completions.
void
DetailedCuteBackend::applyWriteFinish(const DecodedFifoEntry &entry)
{
    if (entry.isLoad) {
        scoreboard.onLoadFinish(entry);
    } else if (entry.isStore) {
        scoreboard.onStoreWriteFinish(entry);
        if (pendingStoreCount > 0) {
            --pendingStoreCount;
        }
    } else if (entry.isMma) {
        scoreboard.onComputeWriteFinishC(entry);
    } else if (entry.isZeroAcc || entry.isZeroTr) {
        scoreboard.onArithFinish(entry);
    }

}

void
DetailedCuteBackend::finalizeCompletion(const CuteCompletion &completion)
{
    completions.push_back(completion);
}

void
DetailedCuteBackend::enqueueTaskEvent(const DecodedFifoEntry &entry,
                                      TaskEventKind event_kind,
                                      CuteCompletion completion)
{
    TaskEvent event;
    event.entry = entry;
    event.kind = event_kind;
    event.completion = completion;
    event.readyStep = backendStep;
    if (entry.isMma &&
        (event_kind == TaskEventKind::WriteFinish ||
         event_kind == TaskEventKind::TerminalCompletion)) {
        event.readyStep = backendStep + 1;
    }
    taskEvents.push_back(event);
}

void
DetailedCuteBackend::enqueueTaskEvent(const TaskSlot &task, TaskEventKind kind,
                                      CuteCompletion completion)
{
    enqueueTaskEvent(task.entry, kind, completion);
}

void
DetailedCuteBackend::enqueueTaskEvent(const ComputeTaskState &task,
                                      TaskEventKind kind,
                                      CuteCompletion completion)
{
    enqueueTaskEvent(task.entry, kind, completion);
}

void
DetailedCuteBackend::retireTaskSlot(MicroTaskKind kind, uint64_t seq)
{
    std::optional<TaskSlot> *slot = nullptr;
    switch (kind) {
      case MicroTaskKind::AML:
        slot = &amlTask;
        break;
      case MicroTaskKind::BML:
        slot = &bmlTask;
        break;
      case MicroTaskKind::CML:
        slot = &cmlTask;
        break;
      case MicroTaskKind::Release:
        slot = &releaseTask;
        break;
      case MicroTaskKind::Compute:
      case MicroTaskKind::Count:
        assert(false && "retireTaskSlot called with unexpected kind");
        return;
    }

    assert(slot != nullptr);
    assert(slot->has_value());
    assert(slot->value().entry.request.seq == seq);
    slot->reset();
}

void
DetailedCuteBackend::retireComputeTask(uint64_t seq)
{
    assert(!computeTasks.empty());
    assert(computeTasks.front().entry.request.seq == seq);
    assert(computeTasks.front().terminalIssued);
    assert(computeTasks.front().unitWorkDone);
    computeTasks.pop_front();
}

void
DetailedCuteBackend::processTaskEvents()
{
    while (!taskEvents.empty()) {
        const auto event = taskEvents.front();
        if (event.readyStep > backendStep) {
            break;
        }
        taskEvents.pop_front();

        switch (event.kind) {
          case TaskEventKind::ReadFinish:
            assert(event.entry.isStore);
            scoreboard.onStoreReadFinish(event.entry);
            break;
          case TaskEventKind::ComputeReadAFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishA(event.entry);
            break;
          case TaskEventKind::ComputeReadBFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishB(event.entry);
            break;
          case TaskEventKind::ComputeReadCFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishC(event.entry);
            break;
          case TaskEventKind::WriteFinish:
            applyWriteFinish(event.entry);
            break;
          case TaskEventKind::TerminalCompletion:
            {
                finalizeCompletion(event.completion);
            }
            if (event.entry.isMma) {
                retireComputeTask(event.entry.request.seq);
            } else {
                retireTaskSlot(microTaskKindForEntry(event.entry),
                               event.entry.request.seq);
            }
            continue;
        }

    }
}

void
DetailedCuteBackend::finishTaskSlot(std::optional<TaskSlot> &slot)
{
    assert(slot.has_value());
    auto &task = slot.value();
    const auto &entry = task.entry;
    const auto completion = entry.isLoad ?
        executeLoadWrite(task) :
        (entry.isStore ? executeStoreWrite(task) :
         executeTaskSlot(task));

    if (entry.isLoad || entry.isStore ||
        entry.isZeroAcc || entry.isZeroTr) {
        enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    }
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
    task.stage = TaskStage::TerminalPending;
}

void
DetailedCuteBackend::beginComputeUnit(ComputeTaskState &task,
                                      ComputeUnitKind kind)
{
    task.activeUnit = kind;
    task.unitWorkDone = false;
    task.terminalIssued = false;
    task.unitIssueStep = backendStep;

    switch (kind) {
      case ComputeUnitKind::ADC:
        break;
      case ComputeUnitKind::BDC:
        break;
      case ComputeUnitKind::MTE:
        {
            const auto timing = computeMteTiming(task.entry.request.mma);
            task.executeCyclesRemaining = computeExecuteLatency(task.entry);
            task.cdcWritebackBeatsRemaining =
                std::max(1U, timing.cdcWriteCycles);
        }
        break;
      case ComputeUnitKind::CDC:
        break;
      case ComputeUnitKind::None:
      case ComputeUnitKind::Count:
        break;
    }
}

void
DetailedCuteBackend::enqueueComputeCompletion(ComputeTaskState &task,
                                              CuteCompletion completion)
{
    enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
}

void
DetailedCuteBackend::finishComputeTerminal(ComputeTaskState &task,
                                           CuteCompletion completion,
                                           bool grant_all_cdc_beats)
{
    if (grant_all_cdc_beats) {
        task.cdcWritebackBeatsRemaining = 0;
    }

    enqueueComputeCompletion(task, completion);
    task.unitWorkDone = true;
    task.terminalIssued = true;
    task.activeUnit = ComputeUnitKind::None;
}

bool
DetailedCuteBackend::issueComputeReadFrontend(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    if (std::any_of(task.readIssued.begin(), task.readIssued.end(),
                   [](bool issued) { return issued; })) {
        return false;
    }

    const auto a_read = MatrixRegResource::makeRead(
        MatrixBankKind::A, MatrixRegResource::Client::DataController);
    const auto b_read = MatrixRegResource::makeRead(
        MatrixBankKind::B, MatrixRegResource::Client::DataController);

    std::vector<MatrixRegResource::Request> requests = {
        a_read, b_read};

    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants[0].granted || !grants[1].granted) {
        return false;
    }

    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Mma,
        CuteCompletionStatus::Success);
    task.readIssued.fill(true);
    task.unitIssueStep = backendStep;

    return true;
}

void
DetailedCuteBackend::advanceComputeRead(ComputeTaskState &task,
                                        size_t read_idx)
{
    assert(task.entry.isMma);
    assert(read_idx < ComputeReadCount);

    static constexpr std::array<MatrixBankKind, ComputeReadCount> banks = {
        MatrixBankKind::A, MatrixBankKind::B, MatrixBankKind::C};
    static constexpr std::array<TaskEventKind, ComputeReadCount> events = {
        TaskEventKind::ComputeReadAFinish,
        TaskEventKind::ComputeReadBFinish,
        TaskEventKind::ComputeReadCFinish};

    task.readTensorValid[read_idx] = false;
    const uint8_t reg = task.entry.readRegs[read_idx];
    if (regFile.hasRegister(banks[read_idx], reg)) {
        task.readTensors[read_idx] = regFile.read(banks[read_idx], reg);
        task.readTensorValid[read_idx] = true;
    } else if (read_idx != ComputeReadCIdx) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    }

    task.readComplete[read_idx] = true;
    enqueueTaskEvent(task, events[read_idx]);
}

void
DetailedCuteBackend::advanceComputeExecute(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    const auto finish_streaming_writeback = [&]() {
        const auto completion =
            task.bufferedCompletion.status == CuteCompletionStatus::Success ?
                executeComputeWrite(task) : task.bufferedCompletion;
        finishComputeTerminal(task, completion, true);
    };

    task.hasBufferedTensor = false;
    if (!computeDatatypeSupported(task.entry.request.mma) ||
        task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !task.readTensorValid[ComputeReadAIdx] ||
        !task.readTensorValid[ComputeReadBIdx]) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        task.unitWorkDone = true;
        return;
    }

    if (isInt8TileWritebackMma(task.entry.request.mma)) {
        const auto &desc = task.entry.request.mma;
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Success);
        const auto &a_tensor = task.readTensors[ComputeReadAIdx];
        const auto &b_tensor = task.readTensors[ComputeReadBIdx];
        const auto &c_tensor = task.readTensors[ComputeReadCIdx];
        if (a_tensor.elemType != MatrixElemType::Int8 ||
            b_tensor.elemType != MatrixElemType::Int8 ||
            a_tensor.rows != desc.mtilem ||
            a_tensor.cols != desc.mtilek ||
            b_tensor.rows != desc.mtilek ||
            b_tensor.cols != desc.mtilen ||
            (task.readTensorValid[ComputeReadCIdx] &&
             (c_tensor.elemType != MatrixElemType::Int32 ||
              c_tensor.rows != desc.mtilem ||
              c_tensor.cols != desc.mtilen))) {
            task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
        } else {
            MatrixTensor tensor;
            tensor.rows = desc.mtilem;
            tensor.cols = desc.mtilen;
            tensor.elemType = MatrixElemType::Int32;
            tensor.elements.assign(
                static_cast<size_t>(desc.mtilem) * desc.mtilen, 0);
            for (uint32_t m = 0; m < desc.mtilem; ++m) {
                for (uint32_t n = 0; n < desc.mtilen; ++n) {
                    int64_t acc = task.readTensorValid[ComputeReadCIdx] ?
                        c_tensor.elements[
                            static_cast<size_t>(m) * desc.mtilen + n] : 0;
                    for (uint32_t k = 0; k < desc.mtilek; ++k) {
                        const auto lhs = static_cast<int8_t>(
                            a_tensor.elements[
                                static_cast<size_t>(m) * desc.mtilek + k]);
                        const auto rhs = static_cast<int8_t>(
                            b_tensor.elements[
                                static_cast<size_t>(k) * desc.mtilen + n]);
                        acc += static_cast<int32_t>(lhs) *
                               static_cast<int32_t>(rhs);
                    }
                    tensor.elements[
                        static_cast<size_t>(m) * desc.mtilen + n] = acc;
                }
            }
            task.bufferedTensor = std::move(tensor);
            task.hasBufferedTensor = true;
        }
        task.unitWorkDone = true;
        finish_streaming_writeback();
        return;
    }

    task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    task.unitWorkDone = true;
    finish_streaming_writeback();
}

void
DetailedCuteBackend::advanceComputeWriteback(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.activeUnit == ComputeUnitKind::CDC);
    assert(task.cdcWritebackBeatsRemaining != 0);

    --task.cdcWritebackBeatsRemaining;
    if (task.cdcWritebackBeatsRemaining != 0) {
        return;
    }

    const auto completion =
        task.bufferedCompletion.status == CuteCompletionStatus::Success ?
            executeComputeWrite(task) : task.bufferedCompletion;
    finishComputeTerminal(task, completion, false);
}

void
DetailedCuteBackend::advanceTaskSlot(std::optional<TaskSlot> &slot)
{
    if (!slot.has_value()) {
        return;
    }

    auto &task = slot.value();
    switch (task.stage) {
      case TaskStage::Accepted:
        if (task.entry.isStore) {
            task.stage = TaskStage::RegRead;
        } else if (task.entry.isRelease) {
            task.stage = TaskStage::WaitPendingStoreClear;
        } else if (task.entry.isZeroAcc || task.entry.isZeroTr) {
            task.stage = TaskStage::RegWrite;
        } else {
            enqueueLocalMmuBeats(task);
            task.stage = TaskStage::FillPending;
        }
        break;
      case TaskStage::FillPending:
        if (task.entry.isLoad) {
            advanceLoadFill(task);
            if (task.bufferedCompletion.status ==
                    CuteCompletionStatus::Success &&
                !task.lsuLoadFinalized) {
                break;
            }
            if (task.bufferedCompletion.status !=
                CuteCompletionStatus::Success) {
                task.stage = TaskStage::RegWrite;
                break;
            }
        }
        if (task.entry.isLoad && !task.lsuLoadFinalized) {
            break;
        }
        task.stage = TaskStage::RegWrite;
        break;
      case TaskStage::RegWrite:
      case TaskStage::WaitPendingStoreClear:
        if (task.entry.isZeroAcc && !advanceZeroAccDelay(task)) {
            break;
        }
        finishTaskSlot(slot);
        break;
      case TaskStage::StorePending:
        if (task.lsuResponsesReceived < task.lsuTotalBeats) {
            break;
        }
        finishTaskSlot(slot);
        break;
      case TaskStage::TerminalPending:
        break;
      case TaskStage::RegRead:
        advanceStoreRead(task);
        task.stage = TaskStage::StorePending;
        break;
    }
}

void
DetailedCuteBackend::releaseLocalMmuSource(
    const LocalMmuModel::Response &response)
{
    if (useTimingMemory()) {
        localMmu.releaseExternalSource(response.sourceId);
    }
}

DetailedCuteBackend::LocalMmuResponseResult
DetailedCuteBackend::serviceFillTableResponse(
    TaskSlot &task, const LocalMmuModel::Response &response,
    unsigned fill_chunks)
{
    const auto fill_request =
        fillTableRequestForResponse(response, fill_chunks);
    if (!matrixL2FillTable.canAccept(fill_request)) {
        if (isAmlClient(response.client)) {
            ++cutePhaseStats.matrixCuteALoadFillTableFullBlocked;
        } else if (isBmlClient(response.client)) {
            ++cutePhaseStats.matrixCuteBLoadFillTableFullBlocked;
        }
        return LocalMmuResponseResult::Blocked;
    }
    if (!matrixL2FillTable.canAcceptResponse(fill_request)) {
        if (isAmlClient(response.client)) {
            ++cutePhaseStats.matrixCuteALoadFillBankFifoFullBlocked;
        } else if (isBmlClient(response.client)) {
            ++cutePhaseStats.matrixCuteBLoadFillBankFifoFullBlocked;
        }
        return LocalMmuResponseResult::Blocked;
    }

    const auto fill_handle =
        matrixL2FillTable.acceptResponseToBank(fill_request);
    if (!fill_handle.has_value()) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
        releaseLocalMmuSource(response);
        ++task.lsuResponsesReceived;
        return LocalMmuResponseResult::Serviced;
    }

    releaseLocalMmuSource(response);
    ++cutePhaseStats.matrixL2FillAcceptedResponses;
    cutePhaseStats.matrixL2FillAcceptedChunks += fill_request.fillChunks;
    recordMatrixL2FillOccupancy();
    return LocalMmuResponseResult::NoMatch;
}

DetailedCuteBackend::LocalMmuResponseResult
DetailedCuteBackend::finishLocalMmuStoreAck(
    TaskSlot &task, const LocalMmuModel::Response &response)
{
    ++task.lsuResponsesReceived;
    releaseLocalMmuSource(response);
    return LocalMmuResponseResult::Serviced;
}

DetailedCuteBackend::LocalMmuResponseResult
DetailedCuteBackend::serviceLocalMmuReadResponse(
    TaskSlot &task, PendingLocalMmuResponse &pending)
{
    const auto &response = pending.response;
    if (!pending.timingLoadRecorded) {
        if (!recordTimingLoadResponse(task, response)) {
            task.bufferedCompletion = makeCompletion(
                task.entry.request.seq, CuteRequestKind::Lsu,
                CuteCompletionStatus::Unsupported);
            releaseLocalMmuSource(response);
            ++task.lsuResponsesReceived;
            return LocalMmuResponseResult::Serviced;
        }
        pending.timingLoadRecorded = true;
    }

    const unsigned fill_chunks =
        matrixL2FillChunksForResponse(response.byteSize);
    if (useTimingMemory()) {
        const auto fill_result =
            serviceFillTableResponse(task, response, fill_chunks);
        if (fill_result != LocalMmuResponseResult::NoMatch) {
            return fill_result;
        }
    }

    ++task.lsuResponsesReceived;
    return LocalMmuResponseResult::Serviced;
}

DetailedCuteBackend::LocalMmuResponseResult
DetailedCuteBackend::serviceLocalMmuResponse(
    std::optional<TaskSlot> &slot, PendingLocalMmuResponse &pending)
{
    if (!slot.has_value()) {
        return LocalMmuResponseResult::NoMatch;
    }

    const auto &response = pending.response;
    auto &task = slot.value();
    if (task.entry.request.seq != response.seq ||
        localMmuClient(task) != response.client) {
        return LocalMmuResponseResult::NoMatch;
    }

    if (response.isStore) {
        return finishLocalMmuStoreAck(task, response);
    }

    return serviceLocalMmuReadResponse(task, pending);
}

void
DetailedCuteBackend::serviceLocalMmuResponses()
{
    const auto ready_responses = localMmu.takeReadyResponses();
    for (const auto &response : ready_responses) {
        PendingLocalMmuResponse pending;
        pending.response = response;
        pendingLocalMmuResponses.push_back(pending);
    }

    while (!pendingLocalMmuResponses.empty()) {
        auto &pending = pendingLocalMmuResponses.front();
        const auto &response = pending.response;
        auto result = serviceLocalMmuResponse(amlTask, pending);
        if (result == LocalMmuResponseResult::NoMatch) {
            result = serviceLocalMmuResponse(bmlTask, pending);
        }
        if (result == LocalMmuResponseResult::NoMatch) {
            result = serviceLocalMmuResponse(cmlTask, pending);
        }

        if (result == LocalMmuResponseResult::Blocked) {
            break;
        }
        if (result == LocalMmuResponseResult::Serviced) {
            pendingLocalMmuResponses.pop_front();
            continue;
        }

        releaseLocalMmuSource(response);
        pendingLocalMmuResponses.pop_front();
    }
}

bool
DetailedCuteBackend::completeTimingMemoryResponse(
    uint32_t source_id, const uint8_t *data, uint32_t size)
{
    if (!useTimingMemory()) {
        return false;
    }

    const bool completed =
        localMmu.completeExternalResponse(source_id, data, size);
    if (!completed) {
        return false;
    }

    serviceLocalMmuResponses();
    return true;
}

void
DetailedCuteBackend::advanceComputeTask()
{
    if (computeTasks.empty()) {
        return;
    }

    serviceActiveComputeUnits();
    dispatchReadyComputeUnits();
}

void
DetailedCuteBackend::serviceActiveComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.readIssued[ComputeReadAIdx] &&
            !task.readComplete[ComputeReadAIdx] &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::A,
                MatrixRegResource::Client::DataController)) {
            advanceComputeRead(task, ComputeReadAIdx);
        }
        if (task.readIssued[ComputeReadBIdx] &&
            !task.readComplete[ComputeReadBIdx] &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::B,
                MatrixRegResource::Client::DataController)) {
            advanceComputeRead(task, ComputeReadBIdx);
        }
        if (task.readIssued[ComputeReadCIdx] &&
            !task.readComplete[ComputeReadCIdx] &&
            backendStep > task.unitIssueStep) {
            advanceComputeRead(task, ComputeReadCIdx);
        }
        if (task.activeUnit == ComputeUnitKind::MTE) {
            if (task.unitWorkDone) {
                continue;
            }
            if (backendStep <= task.unitIssueStep) {
                continue;
            }
            if (task.executeCyclesRemaining > 1) {
                --task.executeCyclesRemaining;
            } else if (task.executeCyclesRemaining == 1) {
                task.executeCyclesRemaining = 0;
                advanceComputeExecute(task);
            }
        }
        if (task.activeUnit == ComputeUnitKind::CDC &&
            !task.unitWorkDone) {
            advanceComputeWriteback(task);
        }
    }
}

void
DetailedCuteBackend::dispatchReadyComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (task.activeUnit == ComputeUnitKind::None &&
            !task.readIssued[ComputeReadAIdx] &&
            computeUnitAvailable(ComputeUnitKind::ADC) &&
            computeUnitAvailable(ComputeUnitKind::BDC) &&
            computeUnitAvailable(ComputeUnitKind::CDC)) {
            issueComputeReadFrontend(task);
        }
    }

    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (task.activeUnit == ComputeUnitKind::None &&
            task.readComplete[ComputeReadAIdx] &&
            task.readComplete[ComputeReadBIdx] &&
            task.readComplete[ComputeReadCIdx] &&
            computeUnitAvailable(ComputeUnitKind::MTE)) {
            beginComputeUnit(task, ComputeUnitKind::MTE);
        }
    }

    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (task.activeUnit == ComputeUnitKind::MTE &&
            task.unitWorkDone &&
            computeUnitAvailable(ComputeUnitKind::CDC)) {
            beginComputeUnit(task, ComputeUnitKind::CDC);
        }
    }
}

void
DetailedCuteBackend::advanceTaskSlots()
{
    advanceTaskSlot(amlTask);
    advanceTaskSlot(bmlTask);
    advanceTaskSlot(cmlTask);
    advanceComputeTask();
    advanceTaskSlot(releaseTask);
}

void
DetailedCuteBackend::processFifoHead()
{
    if (fifo.empty()) {
        return;
    }

    const auto &head = fifo.head();
    DetailedCuteScoreboard::BlockReason sb_reason;
    const bool can_issue = headReady(head, sb_reason);

    if (can_issue) {
        issueHead(head);
        fifo.dequeue();
        return;
    }

    if (sb_reason != DetailedCuteScoreboard::BlockReason::None) {
        return;
    }

    if (head.isRelease && !releaseReady()) {
        return;
    }
}

void
DetailedCuteBackend::step()
{
    ++backendStep;
    matrixRegResource.advanceCycle();
    if (useTimingMemory()) {
        issueLocalMmuTimingRequest();
    }
    serviceLocalMmuResponses();
    serviceLsuMatrixRegWriteChunks();
    memoryBudget = 1;
    processFifoHead();
    advanceTaskSlots();
    processTaskEvents();
}

CuteCompletion
DetailedCuteBackend::popCompletion()
{
    assert(!completions.empty());
    const auto completion = completions.front();
    completions.pop_front();
    return completion;
}

} // namespace matrix
} // namespace gem5

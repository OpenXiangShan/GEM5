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

#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/MatrixCuteTrace.hh"
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

uint64_t
byteMaskForSize(uint32_t byte_size)
{
    if (byte_size == 0) {
        return 0;
    }
    if (byte_size >= 64) {
        return ~uint64_t(0);
    }
    return (uint64_t(1) << byte_size) - 1;
}

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
    request.sourceId = response.sourceId;
    request.seq = response.metadata.seq;
    request.client = response.metadata.client;
    request.beatIndex = response.metadata.beatIndex;
    request.destBank = response.metadata.destBank;
    request.destReg = response.metadata.destReg;
    request.byteSize = response.metadata.byteSize;
    request.fillChunks = fill_chunks;
    request.targetBank = response.metadata.beatIndex %
                         MatrixRegResource::NumBanks;
    request.targetEntry = response.metadata.destReg +
                          response.metadata.beatIndex * fill_chunks;
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
        state.zero(desc.bank, desc.reg, desc.rows, desc.cols, desc.elemType);
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

    if (!task.hasBufferedTensor) {
        return makeCompletion(task.entry.request.seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
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

size_t
DetailedCuteBackend::lsuPayloadBytes(const AmuLsuDesc &desc) const
{
    return static_cast<size_t>(desc.row) * desc.column *
           elemBytes(desc.elemType);
}

void
DetailedCuteBackend::initializeTimingLoadBuffer(TaskSlot &task)
{
    if (!task.entry.isLoad || !task.lsuLoadBytes.empty()) {
        return;
    }

    const auto &desc = task.entry.request.lsu;
    if (desc.tc) {
        const auto translate = [tc = desc.tc](Addr vaddr, uint32_t size,
                                             Addr &paddr) {
            if (!FullSystem) {
                auto *process = tc->getProcessPtr();
                if (!process || !process->pTable) {
                    return false;
                }
                return process->pTable->translate(vaddr, paddr);
            }

            auto req = std::make_shared<Request>(
                vaddr, size, Request::Flags{}, Request::funcRequestorId,
                0, tc->contextId());
            const auto fault = tc->getMMUPtr()->translateFunctional(
                req, tc, BaseMMU::Read);
            if (fault != NoFault || !req->hasPaddr()) {
                return false;
            }
            paddr = req->getPaddr();
            return true;
        };
        task.lsuLoadPlan = buildTimingLoadPlan(desc, translate);
    } else {
        task.lsuLoadPlan = buildTimingLoadPlan(desc);
    }
    task.lsuLoadBytes.assign(task.lsuLoadPlan.tensorBytes, 0);
    task.lsuLoadByteValid.assign(task.lsuLoadPlan.tensorBytes, false);
    task.lsuLoadBytesReceived = 0;
    task.lsuTimingDataComplete = false;
}

void
DetailedCuteBackend::initializeTimingStoreBuffer(TaskSlot &task)
{
    if (!task.entry.isStore || task.lsuStorePlanInitialized) {
        return;
    }

    const auto &desc = task.entry.request.lsu;
    if (!task.hasBufferedTensor) {
        task.lsuStorePlan = {};
        task.lsuStorePlanInitialized = true;
        return;
    }

    if (desc.tc) {
        const auto translate = [tc = desc.tc](Addr vaddr, uint32_t size,
                                             Addr &paddr) {
            if (!FullSystem) {
                auto *process = tc->getProcessPtr();
                if (!process || !process->pTable) {
                    return false;
                }
                return process->pTable->translate(vaddr, paddr);
            }

            auto req = std::make_shared<Request>(
                vaddr, size, Request::Flags{}, Request::funcRequestorId,
                0, tc->contextId());
            const auto fault = tc->getMMUPtr()->translateFunctional(
                req, tc, BaseMMU::Write);
            if (fault != NoFault || !req->hasPaddr()) {
                return false;
            }
            paddr = req->getPaddr();
            return true;
        };
        task.lsuStorePlan = buildTimingStorePlan(
            desc, task.bufferedTensor, translate);
    } else {
        task.lsuStorePlan = buildTimingStorePlan(desc, task.bufferedTensor);
    }
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
            response.dataSize, task.lsuLoadBytes, task.lsuLoadByteValid,
            task.lsuLoadBytesReceived)) {
        return false;
    }

    task.lsuTimingDataComplete =
        task.lsuLoadBytesReceived == task.lsuLoadBytes.size();
    return true;
}

bool
DetailedCuteBackend::buildTensorFromTimingLoadData(TaskSlot &task)
{
    assert(task.entry.isLoad);
    if (!task.lsuTimingDataComplete) {
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
    switch (task.microTaskKind) {
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
DetailedCuteBackend::bmlBypassFillTableEnabled() const
{
    if (timingConfig.matrixBmlBypassFillTable.has_value()) {
        return *timingConfig.matrixBmlBypassFillTable;
    }

    return timingConfig.matrixReduceWidthBytes >=
           timingConfig.matrixOutsideDataWidthBytes;
}

bool
DetailedCuteBackend::useBmlBypassForResponse(
    const TaskSlot &task, const LocalMmuModel::Response &response) const
{
    return bmlBypassFillTableEnabled() &&
           task.microTaskKind == MicroTaskKind::BML &&
           !response.isStore &&
           response.metadata.destBank == MatrixBankKind::B;
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

    LocalMmuModel::IssuedRequest issued;
    if (!localMmu.issueExternal(backendStep, issued)) {
        return;
    }

    MatrixTimingMemoryAdapter::Request request;
    request.localRequest = issued.request;
    request.metadata = issued.metadata;
    request.isStore = issued.request.isStore;
    request.sourceId = issued.sourceId;

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
            request.dataSize = beat.packetSize;
            request.byteMask = beat.byteMask;
            request.byteEnable = beat.byteEnable;
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

    ++counters.localMmuBeatsIssued;
    DPRINTF(MatrixCuteTrace,
            "local_mmu_timing_issue [sn:%llu] client=%u store=%u "
            "beat=%u bytes=%u source=%u paddr=%#llx step=%llu.\n",
            issued.request.seq,
            static_cast<unsigned>(issued.request.client),
            issued.request.isStore ? 1 : 0,
            issued.request.beatIndex,
            issued.request.byteSize,
            issued.sourceId,
            request.paddr,
            static_cast<unsigned long long>(backendStep));
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

    const size_t payload_bytes = lsuPayloadBytes(desc);
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
    const uint32_t matrix_reg = task.entry.isLoad ?
        task.entry.writeRegs[0] : task.entry.readRegs[0];
    for (unsigned beat = 0; beat < timing_beats; ++beat) {
        const size_t offset = static_cast<size_t>(beat) * 64;
        LocalMmuModel::Request request;
        request.seq = task.entry.request.seq;
        request.client = localMmuClient(task);
        request.isStore = task.entry.isStore;
        request.beatIndex = beat;
        if (task.entry.isLoad) {
            request.byteSize = task.lsuLoadPlan.beats[beat].byteSize;
        } else if (task.entry.isStore) {
            request.byteSize = task.lsuStorePlan.beats[beat].packetSize;
        } else {
            request.byteSize = static_cast<uint32_t>(
                std::min<size_t>(64, payload_bytes - offset));
        }
        request.metadata.valid = true;
        request.metadata.isRMW = task.entry.isLoad && desc.isAcc;
        request.metadata.ameIndex = 0;
        request.metadata.destBank = matrix_bank;
        request.metadata.destReg = matrix_reg;
        request.metadata.byteMask = task.entry.isStore ?
            task.lsuStorePlan.beats[beat].byteMask :
            byteMaskForSize(request.byteSize);
        if (!localMmu.enqueue(request)) {
            task.bufferedCompletion = makeCompletion(
                task.entry.request.seq, CuteRequestKind::Lsu,
                CuteCompletionStatus::Unsupported);
            task.lsuBeatsEnqueued = true;
            return false;
        }
        if (task.entry.isStore) {
            ++counters.localMmuStoreBeatsEnqueued;
        } else {
            ++counters.localMmuLoadBeatsEnqueued;
        }
        DPRINTF(MatrixCuteTrace,
                "local_mmu_enqueue [sn:%llu] unit=%u store=%u "
                "beat=%u/%u bytes=%u pending=%llu outstanding=%llu "
                "step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(task.microTaskKind),
                task.entry.isStore ? 1 : 0,
                beat,
                timing_beats,
                request.byteSize,
                static_cast<unsigned long long>(localMmu.pendingCount()),
                static_cast<unsigned long long>(localMmu.outstandingCount()),
                static_cast<unsigned long long>(backendStep));
    }

    task.lsuTotalBeats = timing_beats;
    task.lsuBeatsEnqueued = true;
    return true;
}

bool
DetailedCuteBackend::noteLsuMatrixRegWriteDrain(
    const MatrixL2FillTable::DrainCandidate &candidate)
{
    const auto update_slot = [&](std::optional<TaskSlot> &slot) {
        if (!slot.has_value()) {
            return false;
        }
        auto &task = slot.value();
        if (!task.entry.isLoad ||
            task.entry.request.seq != candidate.seq ||
            localMmuClient(task) != candidate.client) {
            return false;
        }
        assert(task.lsuPendingMatrixRegWriteChunks != 0);
        --task.lsuPendingMatrixRegWriteChunks;
        return true;
    };

    return update_slot(amlTask) ||
           update_slot(bmlTask) ||
           update_slot(cmlTask);
}

void
DetailedCuteBackend::serviceLsuMatrixRegWriteChunks()
{
    if (!useTimingMemory()) {
        return;
    }

    const auto retire_candidate =
        [&](const MatrixL2FillTable::DrainCandidate &candidate) {
            const bool task_updated = noteLsuMatrixRegWriteDrain(candidate);
            assert(task_updated);
            const bool retired = matrixL2FillTable.retireDrain(candidate);
            assert(retired);
            ++counters.matrixL2FillRetires;
            ++counters.matrixRegLoaderWriteChunksGranted;
        };

    for (unsigned bank = 0; bank < MatrixRegResource::NumBanks; ++bank) {
        const auto candidate = matrixL2FillTable.drainCandidate(bank);
        if (!candidate.has_value()) {
            continue;
        }

        auto write_request = MatrixRegResource::makeWrite(
            candidate->destBank, MatrixRegResource::Client::MemoryLoader,
            candidate->targetEntry);
        write_request.bankMask = 1U << candidate->targetBank;

        if (candidate->destBank == MatrixBankKind::C) {
            retire_candidate(*candidate);
        } else {
            const auto grants = matrixRegResource.arbitrate({write_request});
            assert(grants.size() == 1);
            if (!grants[0].granted) {
                ++counters.matrixRegLoaderWriteChunksStalled;
                DPRINTF(MatrixCuteTrace,
                        "matrix_reg_loader_write_stall [sn:%llu] client=%u "
                        "bank=%u physBank=%u entry=%u step=%llu.\n",
                        candidate->seq,
                        static_cast<unsigned>(candidate->client),
                        static_cast<unsigned>(candidate->destBank),
                        candidate->targetBank,
                        candidate->targetEntry,
                        static_cast<unsigned long long>(backendStep));
                continue;
            }
            retire_candidate(*candidate);
        }

        DPRINTF(MatrixCuteTrace,
                "matrix_reg_loader_write_grant [sn:%llu] client=%u "
                "bank=%u physBank=%u entry=%u remainingBefore=%u "
                "reserved=%llu step=%llu.\n",
                candidate->seq,
                static_cast<unsigned>(candidate->client),
                static_cast<unsigned>(candidate->destBank),
                candidate->targetBank,
                candidate->targetEntry,
                candidate->remainingBeforeRetire,
                static_cast<unsigned long long>(
                    matrixL2FillTable.reservedCount()),
                static_cast<unsigned long long>(backendStep));
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

    if (task.lsuPendingMatrixRegWriteChunks != 0) {
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

        if (useTimingMemory()) {
            task.bufferedCompletion.status =
                CuteCompletionStatus::Unsupported;
            task.lsuPendingMatrixRegWriteChunks = 0;
            task.lsuLoadFinalized = true;
            return;
        }

        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
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
DetailedCuteBackend::applyWriteFinish(const DecodedFifoEntry &entry,
                                      const CuteCompletion &completion,
                                      MicroTaskKind kind)
{
    if (entry.isLoad) {
        scoreboard.onLoadFinish(entry);
    } else if (entry.isStore) {
        scoreboard.onStoreWriteFinish(entry);
        if (pendingStoreCount > 0) {
            --pendingStoreCount;
        }
        DPRINTF(MatrixCuteTrace,
                "store_write_finish [sn:%llu] unit=%u step=%llu pendingStore=%u status=%u.\n",
                entry.request.seq,
                static_cast<unsigned>(kind),
                static_cast<unsigned long long>(backendStep),
                pendingStoreCount,
                static_cast<unsigned>(completion.status));
    } else if (entry.isMma) {
        scoreboard.onComputeWriteFinishC(entry);
    } else if (entry.isZeroAcc || entry.isZeroTr) {
        scoreboard.onArithFinish(entry);
    }

}

uint64_t
DetailedCuteBackend::finalizeCompletion(const CuteCompletion &completion,
                                        MicroTaskKind kind,
                                        uint64_t issueStep,
                                        uint64_t activeCount)
{
    ++counters.backendCompletion;
    ++counters.microtaskFinish;
    counters.lastMicrotaskLatency = backendStep - issueStep;
    counters.microtaskLatencySum += counters.lastMicrotaskLatency;
    ++counters.microtaskFinishesByKind[static_cast<size_t>(kind)];
    completions.push_back(completion);
    DPRINTF(MatrixCuteTrace,
            "backend completion [sn:%llu] kind=%u pendingStore=%u active=%llu.\n",
            completion.seq,
            static_cast<unsigned>(completion.kind),
            pendingStoreCount,
            static_cast<unsigned long long>(activeCount));
    return counters.lastMicrotaskLatency;
}

void
DetailedCuteBackend::enqueueTaskEvent(const TaskSlot &task, TaskEventKind kind,
                                      CuteCompletion completion)
{
    TaskEvent event;
    event.entry = task.entry;
    event.microTaskKind = task.microTaskKind;
    event.kind = kind;
    event.completion = completion;
    event.issueStep = task.issueStep;
    event.readyStep = backendStep;
    if (task.entry.isMma &&
        (kind == TaskEventKind::WriteFinish ||
         kind == TaskEventKind::TerminalCompletion)) {
        event.readyStep = backendStep + 1;
    }
    taskEvents.push_back(event);
}

void
DetailedCuteBackend::enqueueTaskEvent(const ComputeTaskState &task,
                                      TaskEventKind kind,
                                      CuteCompletion completion)
{
    TaskEvent event;
    event.entry = task.entry;
    event.microTaskKind = MicroTaskKind::Compute;
    event.kind = kind;
    event.completion = completion;
    event.issueStep = task.issueStep;
    event.readyStep = backendStep;
    if (task.entry.isMma &&
        (kind == TaskEventKind::WriteFinish ||
         kind == TaskEventKind::TerminalCompletion)) {
        event.readyStep = backendStep + 1;
    }
    taskEvents.push_back(event);
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
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::ComputeReadAFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishA(event.entry);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::ComputeReadBFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishB(event.entry);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::ComputeReadCFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishC(event.entry);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::WriteFinish:
            applyWriteFinish(event.entry, event.completion,
                             event.microTaskKind);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::TerminalCompletion:
            finalizeCompletion(event.completion, event.microTaskKind,
                               event.issueStep,
                               activeTaskCount());
            if (event.entry.isMma) {
                retireComputeTask(event.entry.request.seq);
            } else {
                retireTaskSlot(event.microTaskKind, event.entry.request.seq);
            }
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            DPRINTF(MatrixCuteTrace,
                    "microtask_finish [sn:%llu] unit=%u stage=done step=%llu delta=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(
                        counters.lastMicrotaskLatency));
            break;
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
    task.unitOccupancyTraced = false;

    switch (kind) {
      case ComputeUnitKind::ADC:
        break;
      case ComputeUnitKind::BDC:
        break;
      case ComputeUnitKind::MTE:
        {
            const auto timing = computeMteTiming(task.entry.request.mma);
            if (timing.supported && timing.cdcWriteCycles != 0) {
                task.streamingMteActive = true;
                task.streamingResultPrepared = false;
                task.mteInputBeatsTotal = std::max(
                    1U, timing.mteAcceptedInputBeats);
                task.mteInputBeatsAccepted = 0;
                task.mteResultBeatsProduced = 0;
                task.mteResultBeatsWritten = 0;
                task.mtePipelineTailCycles = timing.fReduceTailCycles;
                task.cdcWritebackBeatsTotal =
                    std::max(1U, timing.cdcWriteCycles);
                task.cdcWritebackBeatsRemaining =
                    task.cdcWritebackBeatsTotal;
                task.cdcWritebackBeatsDone = 0;
                task.mteResultReadySteps.clear();
                task.mteResultFifo.clear();
                task.executeCyclesRemaining = 0;
            } else {
                task.streamingMteActive = false;
                task.executeCyclesRemaining =
                    computeExecuteLatency(task.entry);
            }
        }
        break;
      case ComputeUnitKind::CDC:
        task.cdcWritebackBeatsTotal = computeMteTiming(
            task.entry.request.mma).cdcWriteCycles;
        task.cdcWritebackBeatsRemaining = task.cdcWritebackBeatsTotal;
        if (task.cdcWritebackBeatsTotal == 0) {
            task.cdcWritebackBeatsTotal = 1;
            task.cdcWritebackBeatsRemaining = 1;
        }
        task.cdcWritebackBeatsDone = 0;
        task.cdcTileReadIssued = false;
        task.cdcTileWriteReady = false;
        task.hasCdcTileWriteTensor = false;
        issueCdcTileRead(task);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_issue [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(kind),
                static_cast<unsigned long long>(backendStep));
        break;
      case ComputeUnitKind::None:
      case ComputeUnitKind::Count:
        break;
    }
    if (kind != ComputeUnitKind::None &&
        kind != ComputeUnitKind::Count &&
        kind != ComputeUnitKind::CDC) {
        recordComputeUnitIssue(kind);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_issue [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(kind),
                static_cast<unsigned long long>(backendStep));
    }
}

void
DetailedCuteBackend::recordComputeUnitIssue(ComputeUnitKind kind)
{
    ++counters.computeUnitIssuesByKind[static_cast<size_t>(kind)];
}

void
DetailedCuteBackend::recordComputeUnitOccupy(ComputeUnitKind kind)
{
    ++counters.computeUnitOccupiesByKind[static_cast<size_t>(kind)];
}

void
DetailedCuteBackend::recordComputeUnitFinish(ComputeUnitKind kind)
{
    ++counters.computeUnitFinishesByKind[static_cast<size_t>(kind)];
}

void
DetailedCuteBackend::advanceComputeReadC(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorC = false;
    if (regFile.hasRegister(MatrixBankKind::C, task.entry.readRegs[2])) {
        task.bufferedTensorC =
            regFile.read(MatrixBankKind::C, task.entry.readRegs[2]);
        task.hasBufferedTensorC = true;
    }

    recordComputeUnitFinish(ComputeUnitKind::CDC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::CDC),
            static_cast<unsigned long long>(backendStep));
    task.cdcReadComplete = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadCFinish);
}

bool
DetailedCuteBackend::issueComputeReadFrontend(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    if (task.adcReadIssued || task.bdcReadIssued || task.cdcReadIssued) {
        return false;
    }

    const auto a_read = MatrixRegResource::makeRead(
        MatrixBankKind::A, MatrixRegResource::Client::DataController,
        task.entry.readRegs[0]);
    const auto b_read = MatrixRegResource::makeRead(
        MatrixBankKind::B, MatrixRegResource::Client::DataController,
        task.entry.readRegs[1]);

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
    task.adcReadIssued = true;
    task.bdcReadIssued = true;
    task.cdcReadIssued = true;
    task.unitIssueStep = backendStep;
    task.unitOccupancyTraced = false;

    for (auto kind : {ComputeUnitKind::ADC,
                      ComputeUnitKind::BDC,
                      ComputeUnitKind::CDC}) {
        recordComputeUnitIssue(kind);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_issue [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(kind),
                static_cast<unsigned long long>(backendStep));
    }

    return true;
}

void
DetailedCuteBackend::advanceComputeReadA(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorA = false;
    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Mma,
        CuteCompletionStatus::Success);

    if (!regFile.hasRegister(MatrixBankKind::A, task.entry.readRegs[0])) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else {
        task.bufferedTensorA =
            regFile.read(MatrixBankKind::A, task.entry.readRegs[0]);
        task.hasBufferedTensorA = true;
    }

    recordComputeUnitFinish(ComputeUnitKind::ADC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::ADC),
            static_cast<unsigned long long>(backendStep));
    task.adcReadComplete = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadAFinish);
}

void
DetailedCuteBackend::advanceComputeReadB(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorB = false;
    if (task.bufferedCompletion.status != CuteCompletionStatus::Unsupported &&
        !regFile.hasRegister(MatrixBankKind::B, task.entry.readRegs[1])) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else if (regFile.hasRegister(MatrixBankKind::B, task.entry.readRegs[1])) {
        task.bufferedTensorB =
            regFile.read(MatrixBankKind::B, task.entry.readRegs[1]);
        task.hasBufferedTensorB = true;
    }

    recordComputeUnitFinish(ComputeUnitKind::BDC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::BDC),
            static_cast<unsigned long long>(backendStep));
    task.bdcReadComplete = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadBFinish);
}

void
DetailedCuteBackend::advanceComputeExecute(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensor = false;
    if (!computeDatatypeSupported(task.entry.request.mma) ||
        task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !task.hasBufferedTensorA || !task.hasBufferedTensorB) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        recordComputeUnitFinish(ComputeUnitKind::MTE);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(ComputeUnitKind::MTE),
                static_cast<unsigned long long>(backendStep));
        task.unitWorkDone = true;
        return;
    }

    if (isInt8TileWritebackMma(task.entry.request.mma)) {
        const auto &desc = task.entry.request.mma;
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Success);
        if (task.bufferedTensorA.elemType != MatrixElemType::Int8 ||
            task.bufferedTensorB.elemType != MatrixElemType::Int8 ||
            task.bufferedTensorA.rows != desc.mtilem ||
            task.bufferedTensorA.cols != desc.mtilek ||
            task.bufferedTensorB.rows != desc.mtilek ||
            task.bufferedTensorB.cols != desc.mtilen ||
            (task.hasBufferedTensorC &&
             (task.bufferedTensorC.elemType != MatrixElemType::Int32 ||
              task.bufferedTensorC.rows != desc.mtilem ||
              task.bufferedTensorC.cols != desc.mtilen))) {
            task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
        }
        recordComputeUnitFinish(ComputeUnitKind::MTE);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(ComputeUnitKind::MTE),
                static_cast<unsigned long long>(backendStep));
        task.unitWorkDone = true;
        return;
    }

    MatrixRegFile scratch(regFile.abRegCount(), regFile.cRegCount());
    scratch.write(
        MatrixBankKind::A, task.entry.readRegs[0], task.bufferedTensorA);
    scratch.write(
        MatrixBankKind::B, task.entry.readRegs[1], task.bufferedTensorB);
    if (task.hasBufferedTensorC) {
        scratch.write(
            MatrixBankKind::C, task.entry.readRegs[2],
            task.bufferedTensorC);
    }

    task.bufferedCompletion = executeMma(
        task.entry.request.seq, task.entry.request.mma, scratch);
    if (task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !scratch.hasRegister(MatrixBankKind::C, task.entry.writeRegs[0])) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        recordComputeUnitFinish(ComputeUnitKind::MTE);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(ComputeUnitKind::MTE),
                static_cast<unsigned long long>(backendStep));
        task.unitWorkDone = true;
        return;
    }

    task.bufferedTensor =
        scratch.read(MatrixBankKind::C, task.entry.writeRegs[0]);
    task.hasBufferedTensor = true;
    recordComputeUnitFinish(ComputeUnitKind::MTE);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::MTE),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
}

bool
DetailedCuteBackend::prepareStreamingComputeResult(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    if (task.streamingResultPrepared) {
        return task.bufferedCompletion.status == CuteCompletionStatus::Success;
    }

    task.hasBufferedTensor = false;
    if (!computeDatatypeSupported(task.entry.request.mma) ||
        task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !task.hasBufferedTensorA || !task.hasBufferedTensorB) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        task.streamingResultPrepared = true;
        return false;
    }

    MatrixRegFile scratch(regFile.abRegCount(), regFile.cRegCount());
    scratch.write(
        MatrixBankKind::A, task.entry.readRegs[0], task.bufferedTensorA);
    scratch.write(
        MatrixBankKind::B, task.entry.readRegs[1], task.bufferedTensorB);
    if (task.hasBufferedTensorC) {
        scratch.write(
            MatrixBankKind::C, task.entry.readRegs[2],
            task.bufferedTensorC);
    }

    task.bufferedCompletion = executeMma(
        task.entry.request.seq, task.entry.request.mma, scratch);
    if (task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !scratch.hasRegister(MatrixBankKind::C, task.entry.writeRegs[0])) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        task.streamingResultPrepared = true;
        return false;
    }

    task.bufferedTensor =
        scratch.read(MatrixBankKind::C, task.entry.writeRegs[0]);
    task.hasBufferedTensor = true;
    task.streamingResultPrepared = true;
    return true;
}

bool
DetailedCuteBackend::writeStreamingCdcResult(
    ComputeTaskState &task, unsigned beat)
{
    assert(task.entry.isMma);

    (void)beat;
    ++counters.cdcWriteBeatsGranted;
    return true;
}

void
DetailedCuteBackend::advanceStreamingMte(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.activeUnit == ComputeUnitKind::MTE);

    if (backendStep <= task.unitIssueStep) {
        return;
    }

    prepareStreamingComputeResult(task);

    const unsigned output_capacity = timingConfig.mteResultFifoDepth;
    const unsigned pipeline_capacity =
        std::max(1U, task.mtePipelineTailCycles + output_capacity);

    while (!task.mteResultReadySteps.empty() &&
           task.mteResultReadySteps.front() <= backendStep) {
        if (task.mteResultFifo.size() >= output_capacity) {
            ++counters.mteResultFifoStalls;
            break;
        }
        task.mteResultReadySteps.pop_front();
        task.mteResultFifo.push_back(task.mteResultBeatsProduced);
        ++task.mteResultBeatsProduced;
        ++counters.mteResultBeatsProduced;
    }

    if (!task.mteResultFifo.empty()) {
        const unsigned beat = task.mteResultFifo.front();
        if (writeStreamingCdcResult(task, beat)) {
            task.mteResultFifo.pop_front();
            ++task.mteResultBeatsWritten;
            ++task.cdcWritebackBeatsDone;
            if (task.cdcWritebackBeatsRemaining != 0) {
                --task.cdcWritebackBeatsRemaining;
            }
        }
    }

    if (task.mteInputBeatsAccepted < task.mteInputBeatsTotal &&
        task.mteResultReadySteps.size() + task.mteResultFifo.size() <
            pipeline_capacity) {
        const uint64_t ready_step =
            backendStep + std::max(1U, task.mtePipelineTailCycles);
        task.mteResultReadySteps.push_back(ready_step);
        ++task.mteInputBeatsAccepted;
        ++counters.mteInputBeatsAccepted;
    }

    if (task.mteInputBeatsAccepted != task.mteInputBeatsTotal ||
        task.mteResultBeatsProduced != task.mteInputBeatsTotal ||
        task.mteResultBeatsWritten != task.cdcWritebackBeatsTotal ||
        !task.mteResultFifo.empty()) {
        return;
    }

    const auto completion =
        task.bufferedCompletion.status == CuteCompletionStatus::Success ?
            executeComputeWrite(task) : task.bufferedCompletion;
    enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
    recordComputeUnitFinish(ComputeUnitKind::MTE);
    recordComputeUnitFinish(ComputeUnitKind::CDC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::MTE),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
    task.terminalIssued = true;
    task.activeUnit = ComputeUnitKind::None;
}

bool
DetailedCuteBackend::issueCdcTileRead(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.activeUnit == ComputeUnitKind::CDC);
    if (task.cdcTileReadIssued || task.cdcTileWriteReady) {
        return false;
    }

    const auto &desc = task.entry.request.mma;
    if (!isInt8TileWritebackMma(desc)) {
        return false;
    }

    task.cdcTileReadIssued = true;
    task.cdcTileReadBeatIndex = task.cdcWritebackBeatsDone;
    return true;
}

bool
DetailedCuteBackend::prepareCdcTileWrite(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.cdcTileReadIssued);
    assert(!task.cdcTileWriteReady);

    const auto &desc = task.entry.request.mma;
    const unsigned m_tiles = desc.mtilem / MatrixTileMn;
    const unsigned n_tiles = desc.mtilen / MatrixTileMn;
    const unsigned tiles_per_k_group = m_tiles * n_tiles;
    const unsigned addr = task.cdcTileReadBeatIndex % tiles_per_k_group;
    const unsigned k_group = task.cdcTileReadBeatIndex / tiles_per_k_group;
    const unsigned m_tile = addr / n_tiles;
    const unsigned n_tile = addr % n_tiles;
    const unsigned k_begin = k_group * Int8KGroup;
    const unsigned k_end = std::min(k_begin + Int8KGroup, desc.mtilek);

    MatrixTensor updated;
    if (regFile.hasRegister(MatrixBankKind::C, task.entry.writeRegs[0])) {
        updated = regFile.read(MatrixBankKind::C, task.entry.writeRegs[0]);
    } else {
        updated.rows = desc.mtilem;
        updated.cols = desc.mtilen;
        updated.elemType = desc.dstElemType;
        updated.elements.assign(
            static_cast<size_t>(updated.rows) * updated.cols, 0);
    }

    if (updated.elemType != MatrixElemType::Int32 ||
        updated.rows != desc.mtilem ||
        updated.cols != desc.mtilen ||
        !task.hasBufferedTensorA || !task.hasBufferedTensorB ||
        task.bufferedTensorA.elemType != MatrixElemType::Int8 ||
        task.bufferedTensorB.elemType != MatrixElemType::Int8 ||
        task.bufferedTensorA.rows != desc.mtilem ||
        task.bufferedTensorA.cols != desc.mtilek ||
        task.bufferedTensorB.rows != desc.mtilek ||
        task.bufferedTensorB.cols != desc.mtilen) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        task.cdcTileReadIssued = false;
        task.cdcTileWriteReady = true;
        task.hasCdcTileWriteTensor = false;
        return true;
    }

    for (unsigned mi = 0; mi < MatrixTileMn; ++mi) {
        const unsigned m = m_tile * MatrixTileMn + mi;
        for (unsigned ni = 0; ni < MatrixTileMn; ++ni) {
            const unsigned n = n_tile * MatrixTileMn + ni;
            int64_t acc =
                updated.elements[static_cast<size_t>(m) * updated.cols + n];
            for (unsigned k = k_begin; k < k_end; ++k) {
                const int64_t lhs =
                    task.bufferedTensorA.elements[
                        static_cast<size_t>(m) *
                        task.bufferedTensorA.cols + k];
                const int64_t rhs =
                    task.bufferedTensorB.elements[
                        static_cast<size_t>(k) *
                        task.bufferedTensorB.cols + n];
                acc += lhs * rhs;
            }
            updated.elements[static_cast<size_t>(m) * updated.cols + n] = acc;
        }
    }

    task.cdcTileWriteTensor = std::move(updated);
    task.hasCdcTileWriteTensor = true;
    task.cdcTileReadIssued = false;
    task.cdcTileWriteReady = true;
    return true;
}

void
DetailedCuteBackend::advanceComputeWriteback(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.activeUnit == ComputeUnitKind::CDC);
    assert(task.cdcWritebackBeatsRemaining != 0);

    const bool int8_tile_writeback =
        isInt8TileWritebackMma(task.entry.request.mma) &&
        task.bufferedCompletion.status == CuteCompletionStatus::Success;

    if (int8_tile_writeback && !task.cdcTileWriteReady) {
        if (task.cdcTileReadIssued) {
            prepareCdcTileWrite(task);
        } else {
            issueCdcTileRead(task);
        }
        if (!task.cdcTileWriteReady) {
            return;
        }
    }

    const bool can_pipeline_next_read =
        int8_tile_writeback &&
        task.cdcWritebackBeatsRemaining > 1 &&
        !task.cdcTileReadIssued;
    if (can_pipeline_next_read) {
        task.cdcTileReadIssued = true;
        task.cdcTileReadBeatIndex = task.cdcWritebackBeatsDone + 1;
    }

    if (task.cdcTileWriteReady) {
        if (task.bufferedCompletion.status == CuteCompletionStatus::Success) {
            if (!task.hasCdcTileWriteTensor) {
                task.bufferedCompletion.status =
                    CuteCompletionStatus::Unsupported;
            } else {
                regFile.write(MatrixBankKind::C, task.entry.writeRegs[0],
                              task.cdcTileWriteTensor);
            }
        }
        task.cdcTileWriteReady = false;
        task.hasCdcTileWriteTensor = false;
    }

    --task.cdcWritebackBeatsRemaining;
    ++task.cdcWritebackBeatsDone;
    if (task.cdcWritebackBeatsRemaining != 0) {
        return;
    }

    const auto completion = isInt8TileWritebackMma(task.entry.request.mma) ?
        task.bufferedCompletion : executeComputeWrite(task);
    enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::CDC),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
    task.terminalIssued = true;
    task.activeUnit = ComputeUnitKind::None;
}

bool
DetailedCuteBackend::computeTaskFinishedMte(const ComputeTaskState &task) const
{
    return task.activeUnit == ComputeUnitKind::MTE && task.unitWorkDone;
}

void
DetailedCuteBackend::advanceTaskSlot(std::optional<TaskSlot> &slot)
{
    if (!slot.has_value()) {
        return;
    }

    auto &task = slot.value();
    if (!task.occupancyTraced && backendStep > task.issueStep) {
        task.occupancyTraced = true;
        ++counters.microtaskOccupy;
        DPRINTF(MatrixCuteTrace,
                "microtask_occupy [sn:%llu] unit=%u stage=%u step=%llu age=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(task.microTaskKind),
                static_cast<unsigned>(task.stage),
                static_cast<unsigned long long>(backendStep),
                static_cast<unsigned long long>(backendStep - task.issueStep));
    }

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
      case TaskStage::MemReq:
        enqueueLocalMmuBeats(task);
        task.stage = TaskStage::FillPending;
        break;
      case TaskStage::FillPending:
        if (task.entry.isLoad) {
            advanceLoadFill(task);
            if (task.bufferedCompletion.status ==
                    CuteCompletionStatus::Success &&
                (!task.hasBufferedTensor ||
                 task.lsuPendingMatrixRegWriteChunks != 0)) {
                break;
            }
            if (task.bufferedCompletion.status !=
                CuteCompletionStatus::Success) {
                task.stage = TaskStage::RegWrite;
                break;
            }
        }
        if (task.entry.isLoad && !task.hasBufferedTensor) {
            break;
        }
        task.stage = TaskStage::RegWrite;
        break;
      case TaskStage::RegWrite:
      case TaskStage::WaitPendingStoreClear:
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
        if (!task.hasBufferedTensor &&
            task.bufferedCompletion.status == CuteCompletionStatus::Success) {
            break;
        }
        task.stage = TaskStage::StorePending;
        break;
    }
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

    enum class ServiceResult
    {
        NoMatch,
        Blocked,
        Serviced
    };

    while (!pendingLocalMmuResponses.empty()) {
        auto &pending = pendingLocalMmuResponses.front();
        const auto &response = pending.response;
        auto service_slot = [&](std::optional<TaskSlot> &slot) {
            if (!slot.has_value()) {
                return ServiceResult::NoMatch;
            }
            auto &task = slot.value();
            if (task.entry.request.seq != response.seq ||
                localMmuClient(task) != response.client) {
                return ServiceResult::NoMatch;
            }

            if (!response.isStore) {
                const unsigned fill_chunks =
                    matrixL2FillChunksForResponse(
                        response.metadata.byteSize);

                if (useTimingMemory()) {
                    if (useBmlBypassForResponse(task, response)) {
                        if (!pending.timingLoadRecorded) {
                            if (!recordTimingLoadResponse(task, response)) {
                                task.bufferedCompletion = makeCompletion(
                                    task.entry.request.seq,
                                    CuteRequestKind::Lsu,
                                    CuteCompletionStatus::Unsupported);
                                task.lsuPendingMatrixRegWriteChunks = 0;
                                localMmu.releaseExternalSource(
                                    response.sourceId);
                                ++task.lsuResponsesReceived;
                                ++counters.localMmuReadResponses;
                                return ServiceResult::Serviced;
                            }
                            pending.timingLoadRecorded = true;
                            task.lsuPendingMatrixRegWriteChunks +=
                                fill_chunks;
                            counters.matrixRegLoaderWriteChunksQueued +=
                                fill_chunks;
                            counters.bmlBypassWriteChunksQueued +=
                                fill_chunks;
                        }

                        const unsigned target_bank =
                            response.metadata.beatIndex %
                            MatrixRegResource::NumBanks;
                        const uint32_t target_entry =
                            response.metadata.destReg +
                            response.metadata.beatIndex * fill_chunks +
                            pending.bypassWriteChunksDone;
                        auto write_request = MatrixRegResource::makeWrite(
                            MatrixBankKind::B,
                            MatrixRegResource::Client::MemoryLoader,
                            target_entry);
                        write_request.bankMask = 1U << target_bank;
                        const auto grants =
                            matrixRegResource.arbitrate({write_request});
                        assert(grants.size() == 1);
                        if (!grants[0].granted) {
                            ++counters.matrixRegLoaderWriteChunksStalled;
                            ++counters.bmlBypassWriteChunksStalled;
                            DPRINTF(MatrixCuteTrace,
                                    "bml_bypass_write_stall [sn:%llu] "
                                    "beat=%u bank=%u entry=%u reason=%u "
                                    "step=%llu.\n",
                                    response.seq,
                                    response.beatIndex,
                                    target_bank,
                                    target_entry,
                                    static_cast<unsigned>(grants[0].reason),
                                    static_cast<unsigned long long>(
                                        backendStep));
                            return ServiceResult::Blocked;
                        }

                        assert(task.lsuPendingMatrixRegWriteChunks != 0);
                        --task.lsuPendingMatrixRegWriteChunks;
                        ++pending.bypassWriteChunksDone;
                        ++counters.matrixRegLoaderWriteChunksGranted;
                        ++counters.bmlBypassWriteChunksGranted;
                        if (pending.bypassWriteChunksDone < fill_chunks) {
                            return ServiceResult::Blocked;
                        }

                        localMmu.releaseExternalSource(response.sourceId);
                        ++task.lsuResponsesReceived;
                        ++counters.localMmuReadResponses;
                        ++counters.bmlBypassResponses;
                        DPRINTF(MatrixCuteTrace,
                                "bml_bypass_response [sn:%llu] source=%u "
                                "bytes=%u chunks=%u step=%llu.\n",
                                task.entry.request.seq,
                                response.sourceId,
                                response.dataSize,
                                fill_chunks,
                                static_cast<unsigned long long>(
                                    backendStep));
                        return ServiceResult::Serviced;
                    }

                    const auto fill_request =
                        fillTableRequestForResponse(response, fill_chunks);
                    if (!matrixL2FillTable.canAccept(fill_request)) {
                        ++counters.matrixL2FillFullStalls;
                        DPRINTF(MatrixCuteTrace,
                                "matrix_l2_fill_full [sn:%llu] client=%u "
                                "beat=%u source=%u reserved=%llu "
                                "step=%llu.\n",
                                response.seq,
                                static_cast<unsigned>(response.client),
                                response.beatIndex,
                                response.sourceId,
                                static_cast<unsigned long long>(
                                    matrixL2FillTable.reservedCount()),
                                static_cast<unsigned long long>(backendStep));
                        return ServiceResult::Blocked;
                    }
                    if (!matrixL2FillTable.canAcceptResponse(fill_request)) {
                        ++counters.matrixL2FillBankFifoFullStalls;
                        DPRINTF(MatrixCuteTrace,
                                "matrix_l2_fill_bank_fifo_full [sn:%llu] "
                                "client=%u beat=%u source=%u targetBank=%u "
                                "occupancy=%llu step=%llu.\n",
                                response.seq,
                                static_cast<unsigned>(response.client),
                                response.beatIndex,
                                response.sourceId,
                                fill_request.targetBank,
                                static_cast<unsigned long long>(
                                    matrixL2FillTable.bankFifoOccupancy(
                                        fill_request.targetBank)),
                                static_cast<unsigned long long>(backendStep));
                        return ServiceResult::Blocked;
                    }

                    if (!recordTimingLoadResponse(task, response)) {
                        task.bufferedCompletion = makeCompletion(
                            task.entry.request.seq, CuteRequestKind::Lsu,
                            CuteCompletionStatus::Unsupported);
                        task.lsuPendingMatrixRegWriteChunks = 0;
                        localMmu.releaseExternalSource(response.sourceId);
                        ++task.lsuResponsesReceived;
                        return ServiceResult::Serviced;
                    }

                    const auto fill_handle =
                        matrixL2FillTable.acceptResponseToBank(
                            fill_request, response.data.data(),
                            response.dataSize);
                    if (!fill_handle.has_value()) {
                        task.bufferedCompletion = makeCompletion(
                            task.entry.request.seq, CuteRequestKind::Lsu,
                            CuteCompletionStatus::Unsupported);
                        task.lsuPendingMatrixRegWriteChunks = 0;
                        localMmu.releaseExternalSource(response.sourceId);
                        ++task.lsuResponsesReceived;
                        return ServiceResult::Serviced;
                    }

                    localMmu.releaseExternalSource(response.sourceId);
                    ++counters.matrixL2FillReservations;
                    ++counters.matrixL2FillResponses;
                    if (task.microTaskKind == MicroTaskKind::BML) {
                        ++counters.bmlFillTableResponses;
                    }
                    DPRINTF(MatrixCuteTrace,
                            "matrix_l2_fill_response [sn:%llu] "
                            "source=%u slot=%u gen=%u bytes=%u chunks=%u "
                            "reserved=%llu step=%llu.\n",
                            task.entry.request.seq,
                            response.sourceId,
                            fill_handle->slot,
                            fill_handle->generation,
                            response.dataSize,
                            matrixL2FillTable.pendingFillChunks(
                                *fill_handle),
                            static_cast<unsigned long long>(
                                matrixL2FillTable.reservedCount()),
                            static_cast<unsigned long long>(backendStep));
                }

                ++task.lsuResponsesReceived;
                ++counters.localMmuReadResponses;
                const auto fill_chunk_count =
                    useTimingMemory() ? fill_chunks : 0;
                task.lsuPendingMatrixRegWriteChunks += fill_chunk_count;
                counters.matrixRegLoaderWriteChunksQueued +=
                    fill_chunk_count;
            } else {
                ++task.lsuResponsesReceived;
                ++counters.localMmuStoreAcks;
                if (useTimingMemory()) {
                    localMmu.releaseExternalSource(response.sourceId);
                }
            }
            DPRINTF(MatrixCuteTrace,
                    "local_mmu_response [sn:%llu] unit=%u store=%u "
                    "beat=%u bytes=%u source=%u responses=%u/%u "
                    "pendingFill=%u step=%llu.\n",
                    task.entry.request.seq,
                    static_cast<unsigned>(task.microTaskKind),
                    response.isStore ? 1 : 0,
                    response.beatIndex,
                    response.byteSize,
                    response.sourceId,
                    task.lsuResponsesReceived,
                    task.lsuTotalBeats,
                    task.lsuPendingMatrixRegWriteChunks,
                    static_cast<unsigned long long>(backendStep));
            return ServiceResult::Serviced;
        };

        ServiceResult result = service_slot(amlTask);
        if (result == ServiceResult::NoMatch) {
            result = service_slot(bmlTask);
        }
        if (result == ServiceResult::NoMatch) {
            result = service_slot(cmlTask);
        }

        if (result == ServiceResult::Blocked) {
            break;
        }
        if (result == ServiceResult::Serviced) {
            pendingLocalMmuResponses.pop_front();
            continue;
        }

        if (useTimingMemory()) {
            localMmu.releaseExternalSource(response.sourceId);
        }
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

    traceActiveComputeTasks();
    serviceActiveComputeUnits();

    dispatchReadyComputeUnits();
}

void
DetailedCuteBackend::traceActiveComputeTasks()
{
    for (auto &task : computeTasks) {
        if (!task.occupancyTraced && backendStep > task.issueStep) {
            task.occupancyTraced = true;
            ++counters.microtaskOccupy;
            DPRINTF(MatrixCuteTrace,
                    "microtask_occupy [sn:%llu] unit=%u stage=%u step=%llu age=%llu.\n",
                    task.entry.request.seq,
                    static_cast<unsigned>(MicroTaskKind::Compute),
                    static_cast<unsigned>(task.activeUnit),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(backendStep - task.issueStep));
        }

        if (task.activeUnit != ComputeUnitKind::None &&
            !task.unitOccupancyTraced && backendStep > task.unitIssueStep) {
            task.unitOccupancyTraced = true;
            recordComputeUnitOccupy(task.activeUnit);
            DPRINTF(MatrixCuteTrace,
                    "compute_unit_occupy [sn:%llu] unit=%u step=%llu age=%llu.\n",
                    task.entry.request.seq,
                    static_cast<unsigned>(task.activeUnit),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(
                        backendStep - task.unitIssueStep));
        }
    }
}

void
DetailedCuteBackend::serviceActiveComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.adcReadIssued && !task.adcReadComplete &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::A,
                MatrixRegResource::Client::DataController)) {
            advanceComputeReadA(task);
        }
        if (task.bdcReadIssued && !task.bdcReadComplete &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::B,
                MatrixRegResource::Client::DataController)) {
            advanceComputeReadB(task);
        }
        if (task.cdcReadIssued && !task.cdcReadComplete &&
            backendStep > task.unitIssueStep) {
            advanceComputeReadC(task);
        }
        if (task.activeUnit == ComputeUnitKind::MTE) {
            if (task.unitWorkDone) {
                continue;
            }
            if (task.streamingMteActive) {
                advanceStreamingMte(task);
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
            !task.adcReadIssued &&
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
            task.adcReadComplete &&
            task.bdcReadComplete &&
            task.cdcReadComplete &&
            computeUnitAvailable(ComputeUnitKind::MTE)) {
            beginComputeUnit(task, ComputeUnitKind::MTE);
        }
    }

    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (computeTaskFinishedMte(task) &&
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
        ++counters.fifoDequeue;
        DPRINTF(MatrixCuteTrace,
                "fifo_deq [sn:%llu] kind=%u queued=%llu active=%llu pendingStore=%u.\n",
                head.request.seq,
                static_cast<unsigned>(head.request.kind),
                static_cast<unsigned long long>(fifo.size() - 1),
                static_cast<unsigned long long>(activeTaskCount() + 1),
                pendingStoreCount);
        issueHead(head);
        fifo.dequeue();
        return;
    }

    if (sb_reason != DetailedCuteScoreboard::BlockReason::None) {
        recordScoreboardBlock(sb_reason);
        DPRINTF(MatrixCuteTrace,
                "scoreboard_block [sn:%llu] kind=%u reason=%u pendingStore=%u.\n",
                head.request.seq,
                static_cast<unsigned>(head.request.kind),
                static_cast<unsigned>(sb_reason),
                pendingStoreCount);
        return;
    }

    if (head.isRelease && !releaseReady()) {
        recordFifoBlock(FifoBlockReason::ReleasePendingStore);
        DPRINTF(MatrixCuteTrace,
                "fifo_block [sn:%llu] kind=%u reason=release_pending_store pendingStore=%u.\n",
                head.request.seq,
                static_cast<unsigned>(head.request.kind),
                pendingStoreCount);
        return;
    }

    recordFifoBlock(FifoBlockReason::DownstreamNotAccepting);
    DPRINTF(MatrixCuteTrace,
            "fifo_block [sn:%llu] kind=%u reason=downstream_not_accepting pendingStore=%u.\n",
            head.request.seq,
            static_cast<unsigned>(head.request.kind),
            pendingStoreCount);
}

void
DetailedCuteBackend::step()
{
    ++backendStep;
    matrixRegResource.advanceCycle();
    if (useTimingMemory()) {
        issueLocalMmuTimingRequest();
    } else {
        const auto issued_before = localMmu.issuedCount();
        localMmu.step(backendStep);
        const auto issued_after = localMmu.issuedCount();
        if (issued_after != issued_before) {
            counters.localMmuBeatsIssued += issued_after - issued_before;
            DPRINTF(MatrixCuteTrace,
                    "local_mmu_issue step=%llu issued=%llu pending=%llu "
                    "outstanding=%llu.\n",
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(
                        issued_after - issued_before),
                    static_cast<unsigned long long>(localMmu.pendingCount()),
                    static_cast<unsigned long long>(
                        localMmu.outstandingCount()));
        }
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

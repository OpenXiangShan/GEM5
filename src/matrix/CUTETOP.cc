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
constexpr unsigned LocalMmuReadResponseMatrixRegChunks = 2;

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
matrixRegWriteConflictsWithComputeRead(
    const MatrixRegResource::Request &write_request,
    const DecodedFifoEntry &entry)
{
    if (write_request.bank == MatrixBankKind::A ||
        write_request.bank == MatrixBankKind::B) {
        return true;
    }

    if (write_request.bank == MatrixBankKind::C) {
        return (write_request.entry & 1) == (entry.readRegs[2] & 1);
    }

    return false;
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

MatrixRegResource::Request
MatrixRegResource::makeRead(MatrixBankKind bank, Client client,
                            uint32_t entry)
{
    Request request;
    request.bank = bank;
    request.client = client;
    request.access = Access::Read;
    request.entry = entry;
    return request;
}

MatrixRegResource::Request
MatrixRegResource::makeWrite(MatrixBankKind bank, Client client,
                             uint32_t entry)
{
    Request request;
    request.bank = bank;
    request.client = client;
    request.access = Access::Write;
    request.entry = entry;
    return request;
}

bool
MatrixRegResource::isAbBank(MatrixBankKind bank)
{
    return bank == MatrixBankKind::A || bank == MatrixBankKind::B;
}

void
MatrixRegResource::enqueueReadResponse(const Request &request)
{
    ReadResponse response;
    response.bank = request.bank;
    response.client = request.client;
    response.readyCycle = cycle + ReadLatencyCycles;
    readResponses.push_back(response);
}

std::vector<MatrixRegResource::Grant>
MatrixRegResource::arbitrate(const std::vector<Request> &requests)
{
    std::vector<Grant> grants(requests.size());
    std::vector<bool> eligible(requests.size(), true);

    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].bankMask != FullBankMask) {
            grants[i].reason = StallReason::PartialBankMask;
            eligible[i] = false;
        }
    }

    for (const auto bank : {MatrixBankKind::A, MatrixBankKind::B}) {
        std::vector<size_t> reads;
        std::vector<size_t> writes;
        for (size_t i = 0; i < requests.size(); ++i) {
            if (!eligible[i] || requests[i].bank != bank) {
                continue;
            }
            if (requests[i].access == Access::Read) {
                reads.push_back(i);
            } else {
                writes.push_back(i);
            }
        }

        if (!writes.empty()) {
            grants[writes.front()].granted = true;
            for (size_t i = 1; i < writes.size(); ++i) {
                grants[writes[i]].reason = StallReason::BankConflict;
            }
            for (const auto read_idx : reads) {
                grants[read_idx].reason = StallReason::AbWritePriority;
            }
        } else if (!reads.empty()) {
            grants[reads.front()].granted = true;
            for (size_t i = 1; i < reads.size(); ++i) {
                grants[reads[i]].reason = StallReason::BankConflict;
            }
        }
    }

    std::vector<size_t> c_reads;
    std::vector<size_t> c_writes;
    for (size_t i = 0; i < requests.size(); ++i) {
        if (!eligible[i] || requests[i].bank != MatrixBankKind::C) {
            continue;
        }
        if (requests[i].access == Access::Read) {
            c_reads.push_back(i);
        } else {
            c_writes.push_back(i);
        }
    }

    std::optional<size_t> c_write_grant;
    if (!c_writes.empty()) {
        c_write_grant = c_writes.front();
        grants[*c_write_grant].granted = true;
        for (size_t i = 1; i < c_writes.size(); ++i) {
            grants[c_writes[i]].reason = StallReason::BankConflict;
        }
    }

    if (!c_reads.empty()) {
        const bool same_parity_write =
            c_write_grant &&
            ((requests[c_reads.front()].entry & 1) ==
             (requests[*c_write_grant].entry & 1));
        if (same_parity_write) {
            grants[c_reads.front()].reason =
                StallReason::CReadWriteConflict;
            if (c_writes.size() == 1) {
                grants[*c_write_grant].granted = false;
                grants[*c_write_grant].reason =
                    StallReason::CReadWriteConflict;
            }
        } else if (c_writes.size() > 1) {
            grants[c_reads.front()].reason = StallReason::BankConflict;
        } else {
            grants[c_reads.front()].granted = true;
        }
        for (size_t i = 1; i < c_reads.size(); ++i) {
            grants[c_reads[i]].reason = StallReason::BankConflict;
        }
    }

    for (size_t i = 0; i < requests.size(); ++i) {
        if (grants[i].granted && requests[i].access == Access::Read) {
            enqueueReadResponse(requests[i]);
        }
    }

    return grants;
}

bool
MatrixRegResource::readResponseReady(MatrixBankKind bank, Client client) const
{
    return std::any_of(
        readResponses.begin(), readResponses.end(),
        [&](const ReadResponse &response) {
            return response.bank == bank &&
                   response.client == client &&
                   response.readyCycle <= cycle;
        });
}

bool
MatrixRegResource::consumeReadResponse(MatrixBankKind bank, Client client)
{
    auto it = std::find_if(
        readResponses.begin(), readResponses.end(),
        [&](const ReadResponse &response) {
            return response.bank == bank &&
                   response.client == client &&
                   response.readyCycle <= cycle;
        });
    if (it == readResponses.end()) {
        return false;
    }

    readResponses.erase(it);
    return true;
}

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
DetailedCuteBackend::executeLsu(uint64_t seq, const AmuLsuDesc &desc,
                                MatrixRegFile &state,
                                MatrixMemoryAdapter &memory)
{
    MatrixBankKind bank = MatrixBankKind::C;
    if (desc.isAcc) {
        bank = MatrixBankKind::C;
    } else if (desc.isA) {
        bank = MatrixBankKind::A;
    } else if (desc.isB) {
        bank = MatrixBankKind::B;
    } else {
        return makeCompletion(seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
    }

    if (!desc.isStore) {
        MatrixTensor tensor;
        if (!memory.loadTile(desc, tensor)) {
            return makeCompletion(seq, CuteRequestKind::Lsu,
                                  CuteCompletionStatus::Unsupported);
        }
        state.write(bank, desc.ms, tensor);
        return makeCompletion(seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Success);
    }

    if (!state.hasRegister(bank, desc.ms)) {
        return makeCompletion(seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
    }

    const auto &tensor = state.read(bank, desc.ms);
    if (!memory.storeTile(desc, tensor)) {
        return makeCompletion(seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
    }

    return makeCompletion(seq, CuteRequestKind::Lsu,
                          CuteCompletionStatus::Success);
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
DetailedCuteBackend::executeEntry(const DecodedFifoEntry &entry)
{
    const auto &req = entry.request;
    switch (req.kind) {
      case CuteRequestKind::Lsu:
        return executeLsu(req.seq, req.lsu, regFile, *memory);
      case CuteRequestKind::Mma:
        return executeMma(req.seq, req.mma, regFile);
      case CuteRequestKind::Arith:
        return executeArith(req.seq, req.arith, regFile);
      case CuteRequestKind::Release:
        return execRelease(req.seq, req.release);
    }

    return {};
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

std::optional<MatrixRegResource::Request>
DetailedCuteBackend::matrixRegWriteRequestForTask(
    const TaskSlot &task, const CuteCompletion &completion) const
{
    if (completion.status != CuteCompletionStatus::Success) {
        return std::nullopt;
    }

    if (task.entry.isZeroAcc || task.entry.isZeroTr) {
        return MatrixRegResource::makeWrite(
            destBank(task.entry), MatrixRegResource::Client::DataController,
            task.entry.writeRegs[0]);
    }

    return std::nullopt;
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

unsigned
DetailedCuteBackend::lsuBeatCount(const AmuLsuDesc &desc) const
{
    const size_t bytes = lsuPayloadBytes(desc);
    return bytes == 0 ? 0 : static_cast<unsigned>((bytes + 63) / 64);
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
    task.lsuFunctionalDone = true;
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

void
DetailedCuteBackend::serviceLsuMatrixRegWriteChunk(TaskSlot &task)
{
    if (!task.entry.isLoad || task.lsuPendingMatrixRegWriteChunks == 0) {
        return;
    }
    assert(!task.lsuPendingMatrixRegWriteEntries.empty());
    assert(!task.lsuPendingMatrixRegWriteSourceIds.empty());

    const auto write_request = MatrixRegResource::makeWrite(
        destBank(task.entry), MatrixRegResource::Client::MemoryLoader,
        task.lsuPendingMatrixRegWriteEntries.front());
    const auto source_id = task.lsuPendingMatrixRegWriteSourceIds.front();
    pendingMatrixRegWrites.push_back(write_request);

    const auto grants = matrixRegResource.arbitrate({write_request});
    assert(grants.size() == 1);
    if (!grants[0].granted) {
        ++counters.matrixRegLoaderWriteChunksStalled;
        DPRINTF(MatrixCuteTrace,
                "matrix_reg_loader_write_stall [sn:%llu] unit=%u "
                "bank=%u entry=%u pending=%u done=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(task.microTaskKind),
                static_cast<unsigned>(write_request.bank),
                write_request.entry,
                task.lsuPendingMatrixRegWriteChunks,
                task.lsuMatrixRegWriteChunksDone,
                static_cast<unsigned long long>(backendStep));
        return;
    }

    --task.lsuPendingMatrixRegWriteChunks;
    ++task.lsuMatrixRegWriteChunksDone;
    task.lsuPendingMatrixRegWriteEntries.pop_front();
    task.lsuPendingMatrixRegWriteSourceIds.pop_front();
    if (std::find(task.lsuPendingMatrixRegWriteSourceIds.begin(),
                  task.lsuPendingMatrixRegWriteSourceIds.end(),
                  source_id) ==
        task.lsuPendingMatrixRegWriteSourceIds.end()) {
        if (useTimingMemory()) {
            const bool released =
                localMmu.releaseExternalSource(source_id);
            assert(released);
        }
    }
    ++counters.matrixRegLoaderWriteChunksGranted;
    DPRINTF(MatrixCuteTrace,
            "matrix_reg_loader_write_grant [sn:%llu] unit=%u "
            "bank=%u entry=%u pending=%u done=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(task.microTaskKind),
            static_cast<unsigned>(write_request.bank),
            write_request.entry,
            task.lsuPendingMatrixRegWriteChunks,
            task.lsuMatrixRegWriteChunksDone,
            static_cast<unsigned long long>(backendStep));
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
        serviceLsuMatrixRegWriteChunk(task);
        if (task.lsuPendingMatrixRegWriteChunks != 0 ||
            task.lsuResponsesReceived < task.lsuTotalBeats) {
            return;
        }
    }

    if (task.lsuResponsesReceived < task.lsuTotalBeats) {
        return;
    }

    if (!task.lsuFunctionalDone) {
        task.hasBufferedTensor = false;
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Success);

        if (buildTensorFromTimingLoadData(task)) {
            serviceLsuMatrixRegWriteChunk(task);
            return;
        }

        if (useTimingMemory()) {
            task.bufferedCompletion.status =
                CuteCompletionStatus::Unsupported;
            task.lsuPendingMatrixRegWriteChunks = 0;
            task.lsuPendingMatrixRegWriteEntries.clear();
            task.lsuPendingMatrixRegWriteSourceIds.clear();
            task.lsuFunctionalDone = true;
            return;
        }

        MatrixTensor tensor;
        if (!memory->loadTile(task.entry.request.lsu, tensor)) {
            task.bufferedCompletion.status =
                CuteCompletionStatus::Unsupported;
            task.lsuPendingMatrixRegWriteChunks = 0;
            task.lsuPendingMatrixRegWriteEntries.clear();
            task.lsuPendingMatrixRegWriteSourceIds.clear();
            task.lsuFunctionalDone = true;
            return;
        }

        task.bufferedTensor = std::move(tensor);
        task.hasBufferedTensor = true;
        task.lsuFunctionalDone = true;
    }

    serviceLsuMatrixRegWriteChunk(task);
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
        if (auto request = matrixRegWriteRequestForTask(task, completion)) {
            pendingMatrixRegWrites.push_back(*request);
        }
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
        task.executeCyclesRemaining = computeExecuteLatency(task.entry);
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
    const auto c_read = MatrixRegResource::makeRead(
        MatrixBankKind::C, MatrixRegResource::Client::DataController,
        task.entry.readRegs[2]);

    for (const auto &write_request : pendingMatrixRegWrites) {
        if (matrixRegWriteConflictsWithComputeRead(
                write_request, task.entry)) {
            return false;
        }
    }

    std::vector<MatrixRegResource::Request> requests = {
        a_read, b_read, c_read};
    requests.insert(requests.end(), pendingMatrixRegWrites.begin(),
                    pendingMatrixRegWrites.end());

    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants[0].granted || !grants[1].granted || !grants[2].granted) {
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

    const unsigned n_tiles = desc.mtilen / MatrixTileMn;
    const unsigned addr = task.cdcWritebackBeatsDone %
                          ((desc.mtilem / MatrixTileMn) * n_tiles);
    const unsigned m_tile = addr / n_tiles;
    const unsigned n_tile = addr % n_tiles;
    const unsigned c_tile_entry = m_tile * n_tiles + n_tile;

    std::vector<MatrixRegResource::Request> requests =
        pendingMatrixRegWrites;
    requests.push_back(MatrixRegResource::makeRead(
        MatrixBankKind::C, MatrixRegResource::Client::DataController,
        c_tile_entry));
    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants.back().granted) {
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

    if (!matrixRegResource.consumeReadResponse(
            MatrixBankKind::C, MatrixRegResource::Client::DataController)) {
        return false;
    }

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
        task.cdcTileWriteBeatIndex = task.cdcTileReadBeatIndex;
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
    task.cdcTileWriteBeatIndex = task.cdcTileReadBeatIndex;
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

    const auto write_entry = int8_tile_writeback ?
        task.cdcTileWriteBeatIndex : task.entry.writeRegs[0];
    const auto write_request = MatrixRegResource::makeWrite(
        MatrixBankKind::C, MatrixRegResource::Client::DataController,
        write_entry);
    std::vector<MatrixRegResource::Request> requests =
        pendingMatrixRegWrites;

    const bool can_pipeline_next_read =
        int8_tile_writeback &&
        task.cdcWritebackBeatsRemaining > 1 &&
        !task.cdcTileReadIssued;
    if (can_pipeline_next_read) {
        const auto &desc = task.entry.request.mma;
        const unsigned n_tiles = desc.mtilen / MatrixTileMn;
        const unsigned next_beat = task.cdcWritebackBeatsDone + 1;
        const unsigned addr =
            next_beat % ((desc.mtilem / MatrixTileMn) * n_tiles);
        const unsigned m_tile = addr / n_tiles;
        const unsigned n_tile = addr % n_tiles;
        requests.push_back(MatrixRegResource::makeRead(
            MatrixBankKind::C, MatrixRegResource::Client::DataController,
            m_tile * n_tiles + n_tile));
    }
    requests.push_back(write_request);
    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants.back().granted) {
        return;
    }

    if (can_pipeline_next_read) {
        const auto read_grant = grants[grants.size() - 2];
        if (read_grant.granted) {
            task.cdcTileReadIssued = true;
            task.cdcTileReadBeatIndex = task.cdcWritebackBeatsDone + 1;
        }
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
        if (!task.hasBufferedTensor && task.bufferedCompletion.status == CuteCompletionStatus::Success) {
            break;
        }
        task.stage = TaskStage::StorePending;
        break;
    }
}

void
DetailedCuteBackend::serviceLocalMmuResponses()
{
    for (const auto &response : localMmu.takeReadyResponses()) {
        auto service_slot = [&](std::optional<TaskSlot> &slot) {
            if (!slot.has_value()) {
                return false;
            }
            auto &task = slot.value();
            if (task.entry.request.seq != response.seq ||
                localMmuClient(task) != response.client) {
                return false;
            }

            ++task.lsuResponsesReceived;
            if (!response.isStore) {
                ++counters.localMmuReadResponses;
                if (useTimingMemory() &&
                    !recordTimingLoadResponse(task, response)) {
                    task.bufferedCompletion = makeCompletion(
                        task.entry.request.seq, CuteRequestKind::Lsu,
                        CuteCompletionStatus::Unsupported);
                    task.lsuPendingMatrixRegWriteChunks = 0;
                    task.lsuPendingMatrixRegWriteEntries.clear();
                    task.lsuPendingMatrixRegWriteSourceIds.clear();
                    localMmu.releaseExternalSource(response.sourceId);
                    return true;
                }
                const uint32_t base_entry = task.entry.writeRegs[0] +
                    response.beatIndex *
                    LocalMmuReadResponseMatrixRegChunks;
                for (unsigned chunk = 0;
                     chunk < LocalMmuReadResponseMatrixRegChunks;
                     ++chunk) {
                    task.lsuPendingMatrixRegWriteEntries.push_back(
                        base_entry + chunk);
                    task.lsuPendingMatrixRegWriteSourceIds.push_back(
                        response.sourceId);
                }
                task.lsuPendingMatrixRegWriteChunks +=
                    LocalMmuReadResponseMatrixRegChunks;
                counters.matrixRegLoaderWriteChunksQueued +=
                    LocalMmuReadResponseMatrixRegChunks;
            } else {
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
            return true;
        };

        if (service_slot(amlTask) ||
            service_slot(bmlTask) ||
            service_slot(cmlTask)) {
            continue;
        }
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
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::C,
                MatrixRegResource::Client::DataController)) {
            advanceComputeReadC(task);
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
    pendingMatrixRegWrites.clear();
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

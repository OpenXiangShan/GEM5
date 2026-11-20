/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * Copyright (c) 2011 Regents of the University of California
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

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "arch/riscv/insts/static_inst.hh"
#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/matrix_cpu_state.hh"
#include "debug/Commit.hh"
#include "matrix/CUTETOP.hh"
#include "matrix/MatrixTypes.hh"
#include "matrix/MemoryLoader.hh"
#include "sim/full_system.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

namespace o3
{

Port &
CPU::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "matrix_mem_port") {
        panic_if(!enableMatrixMemPort,
                 "matrix_mem_port requested while enableMatrixMemPort=false");
        return *matrixState->memoryPort;
    }

    return BaseCPU::getPort(if_name, idx);
}

void
CPU::readMatrixRegs(ThreadID tid)
{
    auto diffAllStates = this->diffAllStates[tid];
    diffAllStates->gem5RegFile.mcsr =
        readMiscRegNoEffect(RiscvISA::MISCREG_XMCSR, tid);
    diffAllStates->gem5RegFile.mxrm =
        readMiscRegNoEffect(RiscvISA::MISCREG_XMXRM, tid);
    diffAllStates->gem5RegFile.msat =
        readMiscRegNoEffect(RiscvISA::MISCREG_XMSAT, tid);
    diffAllStates->gem5RegFile.mfflags =
        readMiscRegNoEffect(RiscvISA::MISCREG_XMFFLAGS, tid);
    diffAllStates->gem5RegFile.mfrm =
        readMiscRegNoEffect(RiscvISA::MISCREG_XMFRM, tid);
    diffAllStates->gem5RegFile.msaten =
        readMiscRegNoEffect(RiscvISA::MISCREG_XMSATEN, tid);
    diffAllStates->gem5RegFile.tlenb =
        readMiscRegNoEffect(RiscvISA::MISCREG_XTLENB, tid);
    diffAllStates->gem5RegFile.trlenb =
        readMiscRegNoEffect(RiscvISA::MISCREG_XTRLENB, tid);
    diffAllStates->gem5RegFile.alenb =
        readMiscRegNoEffect(RiscvISA::MISCREG_XALENB, tid);
    diffAllStates->gem5RegFile.mtilem =
        readMiscRegNoEffect(RiscvISA::MISCREG_MTILEM, tid);
    diffAllStates->gem5RegFile.mtilen =
        readMiscRegNoEffect(RiscvISA::MISCREG_MTILEN, tid);
    diffAllStates->gem5RegFile.mtilek =
        readMiscRegNoEffect(RiscvISA::MISCREG_MTILEK, tid);
    // msync is the number of synchronization registers, not a token value.
    diffAllStates->gem5RegFile.msync = RiscvISA::MatrixTokenCount;
    for (unsigned i = 0; i < 8; ++i) {
        diffAllStates->gem5RegFile.mcfg[i] =
            readMiscRegNoEffect(RiscvISA::MISCREG_MCFG0 + i, tid);
    }
}

void
CPU::checkMatrixCheckpoint(ThreadID tid) const
{
    if (tid == 0 && matrixHasLiveState()) {
        panic("Serializing O3 CPU with live matrix register state is "
              "unsupported; checkpoint would lose backend register contents.");
    }

    if (tid == 0 && matrixHasPendingWork()) {
        panic("Serializing O3 CPU with pending or live matrix backend state "
              "is unsupported; drain/checkpoint must not lose matrix "
              "register contents.");
    }
}

void
CPU::checkMatrixDrain() const
{
    if (matrixHasLiveState()) {
        panic("Draining O3 CPU with live matrix register state is unsupported; "
              "checkpoint would lose backend register contents.");
    }
}

void
CPU::checkMatrixSwitchOut() const
{
    panic_if(matrixHasLiveState() || matrixHasPendingWork(),
             "Switching O3 CPU with pending or live Matrix state is "
             "unsupported; CPU switch would lose backend state.");
}

void
CPU::commitMatrixResetToken(const DynInstPtr &inst)
{
    if (const auto &token = inst->matrixData().stagedMatrixTokenReset) {
        commitMatrixResetToken(inst->threadNumber, *token, inst->seqNum);
    }
}

MatrixCpuState::MatrixCpuState() = default;
MatrixCpuState::~MatrixCpuState() = default;

namespace
{

uint8_t
matrixRegNumber(matrix::MatrixBankKind bank, uint8_t reg)
{
    if (bank == matrix::MatrixBankKind::C) {
        return reg >= 4 ? reg : reg + 4;
    }
    return reg;
}

void
toNemuAmuCtrl(const matrix::CuteRequest &request, NemuAmuCtrlEvent &event)
{
    event = {};
    event.valid = 1;
    event.pc = request.pc;

    switch (request.kind) {
      case matrix::CuteRequestKind::Mma:
        event.op = 0;
        event.rm = request.mma.rm;
        event.md = request.mma.md;
        event.sat = request.mma.sat;
        event.ms1 = request.mma.ms1;
        event.ms2 = request.mma.ms2;
        event.mtilem = request.mma.mtilem;
        event.mtilen = request.mma.mtilen;
        event.mtilek = request.mma.mtilek;
        event.types1 = request.mma.types1;
        event.types2 = request.mma.types2;
        event.typed = request.mma.typed;
        event.isfp = request.mma.isFp;
        break;
      case matrix::CuteRequestKind::Lsu:
        event.op = 1;
        // RTL NEMU uses the low three rd bits as the matrix register.
        // Element width is carried separately in the AMU event.
        event.md = request.lsu.ms;
        event.sat = request.lsu.isStore;
        event.isfp = request.lsu.transpose;
        event.types1 = request.lsu.isAcc;
        event.types2 = request.lsu.isA;
        event.mtilem = request.lsu.row;
        event.mtilen = request.lsu.column;
        event.typed = request.lsu.widths;
        event.base = request.lsu.physBaseAddr;
        event.stride = request.lsu.stride;
        break;
      case matrix::CuteRequestKind::Arith:
        event.op = 3;
        event.md = matrixRegNumber(request.arith.bank,
                                   request.arith.reg);
        // NEMU's AMU control ABI uses the historical mzero operation code
        // (0x1b8) in the arith event's base field.
        event.base = 0x1b8;
        break;
      case matrix::CuteRequestKind::Release:
        event.op = 2;
        event.mtilem = request.release.tokenIndex;
        break;
    }
}

matrix::MatrixBankKind
matrixLoadBank(const matrix::AmuLsuDesc &desc)
{
    if (desc.isAcc) {
        return matrix::MatrixBankKind::C;
    }
    if (desc.isB) {
        return matrix::MatrixBankKind::B;
    }
    return matrix::MatrixBankKind::A;
}

std::vector<uint8_t>
packMatrixRegister(const matrix::MatrixTensor &tensor,
                   matrix::MatrixBankKind bank,
                   const std::vector<uint8_t> *previous = nullptr)
{
    const size_t elem_bytes = matrix::elemBytes(tensor.elemType);
    const size_t row_bytes = bank == matrix::MatrixBankKind::C ? 512 : 64;
    const size_t reg_bytes = bank == matrix::MatrixBankKind::C ? 65536 : 8192;
    // Only the active rectangle is written. Preserve previously verified
    // bytes outside it when passing NEMU a full-register lazy buffer.
    auto result = previous ? *previous : std::vector<uint8_t>(reg_bytes, 0);
    panic_if(result.size() != reg_bytes, "Invalid Matrix shadow capacity");
    for (uint32_t row = 0; row < tensor.rows; ++row) {
        for (uint32_t column = 0; column < tensor.cols; ++column) {
            const auto value = tensor.elements[
                static_cast<size_t>(row) * tensor.cols + column];
            const size_t offset = bank == matrix::MatrixBankKind::B ?
                column * row_bytes + row * elem_bytes :
                row * row_bytes + column * elem_bytes;
            panic_if(offset + elem_bytes > result.size(),
                     "Matrix snapshot exceeds NEMU register capacity");
            std::memcpy(result.data() + offset, &value, elem_bytes);
        }
    }
    return result;
}

int
execMatrixLsu(RefProxy &ref, NemuAmuCtrlEvent &event,
              const matrix::AmuLsuDesc &desc, uint8_t *dut)
{
    // amu_exec walks physical memory with the original instruction stride.
    const auto &pages = desc.pageMappings;
    if (!pages || std::all_of(pages->begin(), pages->end(),
            [&](const matrix::MatrixPageMapping &page) {
                return page.ppage - page.vpage ==
                       desc.physBaseAddr - desc.baseAddr;
            })) {
        return ref.amu_exec(&event, dut);
    }

    // A single physical base cannot describe these rows. Transfer between
    // REF memory and REF registers in memory order, splitting at each page.
    fatal_if(!ref.get_mreg_row_addr,
             "Paged Matrix difftest requires get_mreg_row_addr in the REF");
    const auto translate = matrix::frozenMatrixTranslator(pages);
    const size_t width = size_t(1) << event.typed;
    const size_t reg_stride = desc.isAcc ? 512 : 64;
    const unsigned reg_rows = desc.isB ? desc.column : desc.row;
    const unsigned reg_cols = desc.isB ? desc.row : desc.column;
    const unsigned rows = desc.transpose ? reg_cols : reg_rows;
    const size_t row_bytes = (desc.transpose ? reg_rows : reg_cols) * width;
    for (unsigned row = 0; row < rows; ++row) {
        for (size_t offset = 0; offset < row_bytes; ) {
            const Addr va = desc.baseAddr + row * desc.stride + offset;
            const unsigned reg_row = desc.transpose ? offset / width : row;
            const size_t reg_offset = desc.transpose ?
                row * width + offset % width : offset;
            size_t size = std::min<size_t>(row_bytes - offset,
                                           4096 - (va & 4095));
            if (desc.transpose)
                size = std::min(size, width - offset % width);
            Addr pa;
            panic_if(!translate(va, size, pa),
                     "Missing Matrix REF translation pc=%#x va=%#x",
                     event.pc, va);
            auto *reg = ref.get_mreg_row_addr(event.md, reg_row) + reg_offset;
            ref.memcpy(pa, reg, size, desc.isStore ? DUT_TO_REF : REF_TO_DUT);
            if (!desc.isStore) {
                panic_if(std::memcmp(reg, dut + reg_row * reg_stride +
                                     reg_offset, size) != 0,
                         "Matrix load mismatch pc=%#x va=%#x pa=%#x md=%u",
                         event.pc, va, pa, event.md);
            }
            offset += size;
        }
    }
    return 0;
}

} // anonymous namespace

const BaseISA::RegClasses &
CPU::matrixRegClasses(const BaseO3CPUParams &params)
{
    // Configure the architectural count before PhysRegFile and the rename
    // maps retain references to the ISA's register classes.
    for (auto *isa : params.isa) {
        static_cast<TheISA::ISA *>(isa)->setMatrixEnabled(
            FullSystem && params.enableMatrixBackend);
    }
    return params.isa[0]->regClasses();
}

void
CPU::initMatrix(const BaseO3CPUParams &params)
{
    enableMatrixBackend = params.enableMatrixBackend;
    enableMatrixMemPort = params.enableMatrixMemPort;
    if (!enableMatrixBackend && !enableMatrixMemPort) {
        return;
    }
    fatal_if(!FullSystem || params.numThreads != 1,
             "Matrix requires full-system mode and one hardware thread");
    if (enableMatrixBackend && difftestEnabled()) {
        for (const auto &state : diffAllStates) {
            fatal_if(!state->proxy->hasMatrixEvents() ||
                     !state->proxy->matrix_store_commit,
                     "Matrix reference lacks required execution/checking APIs");
        }
    }
    matrixState = std::make_unique<MatrixCpuState>();
    registerExitCallback([this]() { finishMatrixDifftest(); });
    if (enableMatrixMemPort) {
        matrixState->memoryPort = std::make_unique<MatrixMemPort>(
            name() + ".matrix_mem_port", this);
    }
    if (enableMatrixBackend) {
        constexpr unsigned detailedCuteFifoDepth = 8;
        matrix::DetailedCuteBackend::TimingConfig timing_config;
        timing_config.matrixCZeroLoadLatencyCycles =
            params.matrixCZeroLoadLatency;
        auto detailed_backend = std::make_unique<matrix::DetailedCuteBackend>(
            detailedCuteFifoDepth,
            matrix::MatrixRegFile::DefaultAbRegCount,
            matrix::MatrixRegFile::DefaultCRegCount,
            timing_config, this, difftestEnabled());
        if (enableMatrixMemPort) {
            detailed_backend->setTimingMemoryAdapter(matrixState->memoryPort.get());
        }
        matrixState->backend = std::move(detailed_backend);
    }
}

void
CPU::commitMatrixResetToken(ThreadID tid, RegVal token_idx,
                            InstSeqNum seq_num)
{
    assert(token_idx < matrixState->resetSeqs.size());
    DPRINTF(Commit,
            "Matrix reset token commit [tid:%i] [sn:%llu] token=%llu.\n",
            tid, seq_num, token_idx);
    matrixState->resetSeqs[token_idx] = seq_num;
    isa[tid]->resetMatrixToken(token_idx);
}

bool
CPU::matrixTokenReady(ThreadID tid, RegVal token, RegVal threshold) const
{
    return isa[tid]->matrixTokenReady(token, threshold);
}

void
CPU::commitMatrixReleaseToken(ThreadID tid, RegVal token_idx,
                              InstSeqNum seq_num)
{
    assert(token_idx < matrixState->resetSeqs.size());
    const auto reset_seq = matrixState->resetSeqs[token_idx];
    if (reset_seq != 0 && seq_num <= reset_seq) {
        ++matrixState->staleReleases;
        DPRINTF(Commit,
                "Matrix release ignored after newer reset [tid:%i] "
                "[sn:%llu] token=%llu resetSn=%llu.\n",
                tid, seq_num, token_idx, reset_seq);
        return;
    }
    isa[tid]->releaseMatrixToken(token_idx);
    DPRINTF(Commit, "Matrix release [tid:%i] [sn:%llu] token=%llu.\n",
            tid, seq_num, token_idx);
}

void
CPU::serviceMatrixBackend()
{
    if (!matrixState || !matrixState->backend) {
        return;
    }

    if (matrixState->backend->hasWork()) {
        matrixState->backend->step();
    }

    while (matrixState->backend->hasCompletion()) {
        auto completion = matrixState->backend->popCompletion();
        auto it = matrixState->owners.find(completion.seq);
        panic_if(it == matrixState->owners.end(),
                 "Matrix backend completion missing owner [sn:%llu]",
                 completion.seq);

        const auto &request = it->second.request;
        panic_if(completion.status != matrix::CuteCompletionStatus::Success,
                 "Unexpected Matrix backend Unsupported completion "
                 "[tid:%i] [sn:%llu] pc=%#llx kind=%s op=%#llx "
                 "M/N/K=%u/%u/%u lhs/rhs/dst=%u/%u/%u LSU=%ux%u type=%u",
                 it->second.tid, completion.seq, request.pc,
                 requestKindName(request.kind), request.op,
                 request.mma.mtilem, request.mma.mtilen, request.mma.mtilek,
                 unsigned(request.mma.lhsElemType),
                 unsigned(request.mma.rhsElemType),
                 unsigned(request.mma.dstElemType),
                 request.lsu.row, request.lsu.column,
                 unsigned(request.lsu.elemType));

        panic_if(completion.tid != it->second.tid,
                 "Matrix completion thread mismatch [sn:%llu]", completion.seq);
        panic_if(it->second.completion.has_value(),
                 "Duplicate Matrix completion [sn:%llu]", completion.seq);
        for (const auto &[older_seq, older] : matrixState->owners) {
            if (older_seq < completion.seq && !older.completion) {
                ++matrixState->outOfOrder;
                break;
            }
        }
        if (completion.kind == matrix::CuteRequestKind::Release) {
            for (const auto &[older_seq, older] : matrixState->owners) {
                panic_if(older_seq < completion.seq &&
                         older.request.kind == matrix::CuteRequestKind::Lsu &&
                         older.request.lsu.isStore && !older.completion,
                         "Matrix release completed before older store "
                         "[tid:%u] [sn:%llu] storeSn=%llu", it->second.tid,
                         completion.seq, older_seq);
            }
            commitMatrixReleaseToken(it->second.tid, completion.tokenIdx,
                                     completion.seq);
            if (difftestEnabled()) {
                NemuAmuCtrlEvent event;
                toNemuAmuCtrl(request, event);
                const auto token = request.release.tokenIndex;
                if (completion.seq > matrixState->resetSeqs[token]) {
                    const auto &proxy = diffAllStates.at(it->second.tid)->proxy;
                    panic_if(proxy->amu_exec(&event, nullptr) != 0,
                             "Matrix reference release failed [sn:%llu]",
                             completion.seq);
                }
                it->second.refReleaseApplied = true;
                ++matrixState->releaseChecked;
            }
        }
        it->second.completion = std::move(completion);
        ++matrixState->completed;
    }
    checkMatrixResults();

    if (matrixState->backend->hasWork() || matrixState->backend->hasCompletion() ||
        !matrixState->owners.empty()) {
        activityRec.activity();
    }
}

bool
CPU::tryIssueMatrixAmu(ThreadID tid, MatrixAmuBuffer &buffer)
{
    const auto *front = buffer.peekReady();
    if (!front) {
        return false;
    }
    const auto seq_num = front->seqNum;
    panic_if(!matrixState || !matrixState->backend,
             "Matrix backend request [sn:%llu] reached CPU while "
             "enableMatrixBackend=false", seq_num);
    if (!matrixState->backend->canAccept(front->backendReq)) {
        DPRINTF(Commit,
                "Matrix toAMU proxy blocked [tid:%i] [sn:%llu] "
                "kind=%s backend cannot accept yet.\n",
                tid, seq_num, requestKindName(front->backendReq.kind));
        return false;
    }

    auto entry = buffer.popReady();
    const auto &backend_req = entry.backendReq;
    DPRINTF(Commit,
            "Matrix toAMU proxy fire [tid:%i] [sn:%llu] kind=%s.\n",
            tid, seq_num, requestKindName(backend_req.kind));

    panic_if(matrix::validateMatrixRequest(backend_req) !=
                 matrix::MatrixRequestError::None,
             "Unsupported Matrix backend request after commit "
             "[tid:%i] [sn:%llu] kind=%s op=%#llx",
             tid, seq_num, requestKindName(backend_req.kind), backend_req.op);

    const bool inserted = matrixState->owners.emplace(
        seq_num, MatrixCpuState::Owner{tid, backend_req}).second;
    panic_if(!inserted,
             "Duplicate matrix backend owner [tid:%i] [sn:%llu]",
             tid, seq_num);
    auto submit_req = backend_req;
    if (submit_req.kind == matrix::CuteRequestKind::Lsu) {
        submit_req.lsu.tc = tcBase(tid);
    }
    matrixState->backend->submit(submit_req);
    matrixState->checkOrder.push_back(seq_num);
    ++matrixState->submitted;

    if (difftestEnabled()) {
        auto &diff_state = diffAllStates.at(tid);
        NemuAmuCtrlEvent event;
        toNemuAmuCtrl(backend_req, event);
        const auto dut_event = event;
        const int result = diff_state->proxy->amu_ctrl(&event);
        panic_if(result != 0,
                 "Matrix AMU control mismatch [tid:%i] [sn:%llu] pc=%#llx "
                 "kind=%s result=%i refPc=%#llx refOp=%u refMd=%u "
                 "refM=%u refN=%u refK=%u refBase=%#llx refStride=%#llx "
                 "refTyped=%u dutOp=%u dutMd=%u dutBase=%#llx dutPc=%#llx",
                 tid, seq_num, backend_req.pc,
                 requestKindName(backend_req.kind), result,
                 event.pc, unsigned(event.op), unsigned(event.md),
                 unsigned(event.mtilem), unsigned(event.mtilen),
                 unsigned(event.mtilek), event.base, event.stride,
                 unsigned(event.typed), unsigned(dut_event.op),
                 unsigned(dut_event.md), dut_event.base, dut_event.pc);
    }

    if (backend_req.kind == matrix::CuteRequestKind::Release) {
        DPRINTF(Commit,
                "Matrix release backend-visible after toAMU proxy "
                "[tid:%i] [sn:%llu] token=%llu.\n",
                tid, seq_num, backend_req.release.tokenIndex);
    } else if (backend_req.kind == matrix::CuteRequestKind::Lsu) {
        DPRINTF(Commit,
                "Matrix LSU backend-visible after toAMU proxy "
                "[tid:%i] [sn:%llu] load=%d store=%d base=%#llx "
                "stride=%#llx row=%#llx column=%#llx.\n",
                tid, seq_num, !backend_req.lsu.isStore,
                backend_req.lsu.isStore,
                static_cast<unsigned long long>(backend_req.lsu.baseAddr),
                static_cast<unsigned long long>(backend_req.lsu.stride),
                static_cast<unsigned long long>(backend_req.lsu.row),
                static_cast<unsigned long long>(backend_req.lsu.column));
    }
    return true;
}

} // namespace o3
} // namespace gem5

namespace gem5::o3
{

bool
CPU::matrixHasLiveState() const
{
    return matrixState && matrixState->backend &&
           matrixState->backend->hasArchitecturalState();
}

bool
CPU::matrixHasPendingWork() const
{
    return matrixState && matrixState->backend &&
           (matrixState->backend->hasWork() ||
            matrixState->backend->hasCompletion() || !matrixState->owners.empty());
}

} // namespace gem5::o3

namespace gem5::o3
{

void
CPU::recordMatrixResponse(
    const matrix::MatrixTimingMemoryAdapter::Request &request)
{
    if (!difftestEnabled()) {
        return;
    }
    auto it = matrixState->owners.find(request.seq);
    panic_if(it == matrixState->owners.end() || it->second.tid != request.tid,
             "Matrix response missing owner [tid:%u] [sn:%llu]",
             request.tid, request.seq);
    if (request.isStore) {
        it->second.packets.push_back({request.paddr, request.packetSize,
            request.data, request.byteMask});
    }
    ++matrixState->memoryCompleted;
}

void
CPU::checkMatrixCommit(ThreadID tid, const DynInstPtr &inst)
{
    if (!difftestEnabled() || !isMatrixBackendEnabled() ||
        !inst->isMatrixInst() || inst->getFault() != NoFault) {
        return;
    }
    ++matrixState->committed;
    riscv64_CPU_regfile reference = {};
    const auto &proxy = diffAllStates.at(tid)->proxy;
    proxy->regcpy(&reference, REF_TO_DUT);
    for (unsigned i = 0; i < 8; ++i) {
        const auto actual = readMiscRegNoEffect(RiscvISA::MISCREG_MCFG0 + i, tid);
        // The ordinary difftest path can copy this shadow back to REF when
        // resynchronizing scalar state. Retain committed Matrix state too.
        diffAllStates.at(tid)->gem5RegFile.mcfg[i] = actual;
        panic_if(actual != reference.mcfg[i],
                 "Matrix config mismatch [tid:%u] [sn:%llu] pc=%#llx "
                 "mcfg[%u] expected=%#llx actual=%#llx", tid, inst->seqNum,
                 inst->pcState().instAddr(), i, reference.mcfg[i], actual);
    }
    panic_if(reference.mtilem != readMiscRegNoEffect(RiscvISA::MISCREG_MTILEM, tid) ||
             reference.mtilen != readMiscRegNoEffect(RiscvISA::MISCREG_MTILEN, tid) ||
             reference.mtilek != readMiscRegNoEffect(RiscvISA::MISCREG_MTILEK, tid),
             "Matrix tile config mismatch [tid:%u] [sn:%llu] pc=%#llx",
             tid, inst->seqNum, inst->pcState().instAddr());
    auto &dut = diffAllStates.at(tid)->gem5RegFile;
    dut.mtilem = readMiscRegNoEffect(RiscvISA::MISCREG_MTILEM, tid);
    dut.mtilen = readMiscRegNoEffect(RiscvISA::MISCREG_MTILEN, tid);
    dut.mtilek = readMiscRegNoEffect(RiscvISA::MISCREG_MTILEK, tid);
    const auto *riscv = dynamic_cast<const RiscvISA::RiscvStaticInst *>(
        inst->staticInst.get());
    if (!riscv || riscv->machInst.opcode7 != 0x2b ||
        riscv->machInst.funct3 != 4 || riscv->machInst.funct7 == 0x48) {
        return;
    }
    NemuMsyncEvent event = {};
    event.valid = 1;
    event.op = riscv->machInst.funct7 == 0x40 ? 0 : 1;
    event.msyncRd = riscv->machInst.rs2;
    event.pc = inst->pcState().instAddr();
    panic_if(proxy->msync_event(&event) != 0,
             "Matrix sync mismatch [tid:%u] [sn:%llu] pc=%#llx "
             "refOp=%u refToken=%u refPc=%#llx", tid, inst->seqNum,
             inst->pcState().instAddr(), event.op, event.msyncRd, event.pc);
    if (event.op == 1) {
        const auto threshold = readArchIntReg(riscv->machInst.rs1, tid);
        panic_if(!matrixTokenReady(tid, event.msyncRd, threshold),
                 "Matrix acquire committed before release [tid:%u] "
                 "[sn:%llu] token=%u threshold=%llu", tid, inst->seqNum,
                 event.msyncRd, threshold);
    }
    ++matrixState->syncChecked;
}

void
CPU::checkMatrixResults()
{
    auto &state = *matrixState;
    while (!state.checkOrder.empty()) {
        const auto seq = state.checkOrder.front();
        auto it = state.owners.find(seq);
        panic_if(it == state.owners.end(), "Missing Matrix check owner");
        auto &owner = it->second;
        if (!owner.completion) {
            break;
        }
        const auto &completion = *owner.completion;
        const auto &request = owner.request;
        const auto tid = owner.tid;
        if (difftestEnabled()) {
            auto &proxy = diffAllStates.at(tid)->proxy;
            NemuAmuCtrlEvent event;
            toNemuAmuCtrl(request, event);
            std::vector<uint8_t> bytes;
            int result = 0;
            if (request.kind == matrix::CuteRequestKind::Mma) {
                panic_if(!completion.result, "Missing immutable MMA snapshot");
                const auto previous = state.verified.find({tid, event.md});
                bytes = packMatrixRegister(*completion.result,
                    matrix::MatrixBankKind::C, previous == state.verified.end() ?
                    nullptr : &previous->second);
                // NEMU copies entire physical registers, never compact tiles.
                std::vector<uint8_t> a(8192), b(8192), c(65536);
                proxy->amu_lazy(&event, bytes.data(), a.data(), b.data(), c.data());
                // lazy has installed the DUT value. No reference call or
                // reference-chain advance is permitted before this check.
                for (unsigned row = 0; row < event.mtilem; ++row) {
                    for (unsigned col = 0; col < event.mtilen; ++col) {
                        uint32_t expected, actual;
                        const auto offset = row * size_t(512) + col * 4;
                        std::memcpy(&expected, c.data() + offset, 4);
                        for (unsigned k = 0; k < event.mtilek; ++k) {
                            // Independent scalar oracle using NEMU's N x K B
                            // view; it shares no GEM5 arithmetic helpers.
                            const unsigned av = a.at(row * 64 + k);
                            const unsigned bv = b.at(col * 64 + k);
                            const int lhs = event.types1 & 4 && av >= 128 ?
                                int(av) - 256 : int(av);
                            const int rhs = event.types2 & 4 && bv >= 128 ?
                                int(bv) - 256 : int(bv);
                            expected += static_cast<uint32_t>(lhs * rhs);
                        }
                        std::memcpy(&actual, bytes.data() + offset, 4);
                        panic_if(actual != expected,
                                 "Matrix MMA mismatch [tid:%u] [sn:%llu] "
                                 "pc=%#llx md=%u ms1=%u ms2=%u M/N/K=%u/%u/%u "
                                 "element=(%u,%u) expected=%#x actual=%#x",
                                 tid, seq, request.pc, event.md, event.ms1,
                                 event.ms2, event.mtilem, event.mtilen,
                                 event.mtilek, row, col, expected, actual);
                    }
                }
                state.verified[{tid, event.md}] = bytes;
                ++state.mmaChecked;
            } else if (request.kind == matrix::CuteRequestKind::Lsu &&
                       !request.lsu.isStore) {
                panic_if(!completion.result, "Missing immutable load snapshot");
                const auto previous = state.verified.find({tid, event.md});
                bytes = packMatrixRegister(*completion.result,
                    matrixLoadBank(request.lsu), previous == state.verified.end() ?
                    nullptr : &previous->second);
                result = execMatrixLsu(*proxy, event, request.lsu, bytes.data());
                state.verified[{tid, event.md}] = bytes;
                ++state.loadChecked;
            } else if (request.kind == matrix::CuteRequestKind::Arith) {
                panic_if(!completion.result, "Missing immutable mzero snapshot");
                bytes = packMatrixRegister(*completion.result, request.arith.bank);
                for (const auto value : bytes) {
                    panic_if(value != 0, "Matrix mzero nonzero byte "
                             "[tid:%u] [sn:%llu] pc=%#llx", tid, seq, request.pc);
                }
                result = proxy->amu_exec(&event, bytes.data());
                state.verified[{tid, event.md}] = bytes;
                ++state.zeroChecked;
            } else if (request.kind == matrix::CuteRequestKind::Lsu) {
                const auto &desc = request.lsu;
                const auto source = state.verified.find({tid, event.md});
                struct ExpectedLine
                {
                    std::array<uint8_t, 64> data = {};
                    uint64_t mask = 0;
                    uint64_t covered = 0;
                };
                std::map<Addr, ExpectedLine> expected;
                const auto translate = matrix::frozenMatrixTranslator(desc.pageMappings);
                const unsigned mem_rows = desc.transpose ? desc.column : desc.row;
                const unsigned mem_cols = desc.transpose ? desc.row : desc.column;
                for (unsigned row = 0; row < mem_rows; ++row) {
                    for (unsigned col = 0; col < mem_cols; ++col) {
                        const auto reg_offset = desc.transpose ?
                            col * 512 + row * 4 : row * 512 + col * 4;
                        const Addr offset = row * desc.stride + col * 4;
                        for (unsigned byte = 0; byte < 4; ++byte) {
                            const Addr va = desc.baseAddr + offset + byte;
                            Addr pa = desc.physBaseAddr + offset + byte;
                            panic_if(translate && !translate(va, 1, pa),
                                     "Missing Matrix store page [sn:%llu] va=%#x", seq, va);
                            auto &line = expected[pa & ~Addr(63)];
                            line.mask |= uint64_t(1) << (pa & 63);
                            line.data[pa & 63] = source == state.verified.end() ?
                                0 : source->second.at(reg_offset + byte);
                        }
                    }
                }
                for (const auto &packet : owner.packets) {
                    const unsigned offset = packet.paddr & 63;
                    auto line_it = expected.find(packet.paddr & ~Addr(63));
                    panic_if(packet.packetSize == 0 ||
                             packet.packetSize > 64 - offset ||
                             line_it == expected.end(),
                             "Invalid Matrix store packet/response [sn:%llu] "
                             "address=%#llx", seq, packet.paddr);
                    auto &line = line_it->second;
                    uint64_t expected_mask = line.mask >> offset;
                    if (packet.packetSize < 64) {
                        expected_mask &= (uint64_t(1) << packet.packetSize) - 1;
                    }
                    panic_if(expected_mask == 0,
                             "Matrix store has no expected bytes [sn:%llu]", seq);
                    panic_if(packet.byteMask != expected_mask,
                             "Matrix store request mask mismatch [tid:%u] "
                             "[sn:%llu] pc=%#llx address=%#llx stride=%llu "
                             "expectedMask=%#llx actualMask=%#llx",
                             tid, seq, request.pc, packet.paddr, desc.stride,
                             expected_mask, packet.byteMask);
                    for (unsigned byte = 0; byte < packet.packetSize; ++byte) {
                        if (packet.byteMask & (uint64_t(1) << byte)) {
                            panic_if(line.data[offset + byte] != packet.data[byte],
                                     "Matrix store request mismatch [tid:%u] "
                                     "[sn:%llu] pc=%#llx address=%#llx "
                                     "expected=%#x actual=%#x", tid, seq, request.pc,
                                     packet.paddr + byte, unsigned(line.data[offset + byte]),
                                     unsigned(packet.data[byte]));
                        }
                    }
                    const uint64_t mask = packet.byteMask << offset;
                    panic_if(line.covered & mask,
                             "Duplicate final Matrix store byte [sn:%llu] "
                             "address=%#llx", seq, packet.paddr);
                    line.covered |= mask;
                }
                for (const auto &[address, line] : expected) {
                    panic_if(line.covered != line.mask,
                             "Matrix store missing bytes [tid:%u] [sn:%llu] "
                             "pc=%#llx address=%#llx missingMask=%#llx", tid, seq,
                             request.pc, address, line.mask & ~line.covered);
                }
                // The coherent cache acknowledges the completed write.
                // Scalar readback after acquire checks cache visibility.
                result = execMatrixLsu(*proxy, event, desc, nullptr);
                uint64_t base = desc.physBaseAddr, stride = desc.stride;
                uint32_t rows = desc.row, cols = desc.column, width = desc.widths;
                bool transpose = desc.transpose;
                panic_if(proxy->matrix_store_commit(&base, &stride, &rows,
                             &cols, &width, &transpose) != 0,
                         "Matrix store control mismatch [tid:%u] [sn:%llu] "
                         "pc=%#llx base=%#llx stride=%llu shape=%ux%u width=%u",
                         tid, seq, request.pc, base, stride, rows, cols, width);
                ++state.storeChecked;
            } else {
                panic_if(!owner.refReleaseApplied,
                         "Matrix release reference effect missing [sn:%llu]", seq);
            }
            panic_if(result != 0,
                     "Matrix result mismatch [tid:%u] [sn:%llu] pc=%#llx "
                     "kind=%s result=%i", tid, seq, request.pc,
                     requestKindName(request.kind), result);
            ++state.checked;
        }
        state.owners.erase(it);
        state.checkOrder.pop_front();
    }
}

void
CPU::finishMatrixDifftest()
{
    if (!difftestEnabled() || !matrixState || !matrixState->backend) {
        return;
    }
    const auto &s = *matrixState;
    const bool memory_pending = s.memoryPort && s.memoryPort->hasPendingRequests();
    inform("MatrixDiff committed=%llu submitted=%llu completed=%llu checked=%llu "
           "load=%llu zero=%llu mma=%llu store=%llu release=%llu sync=%llu "
           "staleReleases=%llu outOfOrder=%llu memoryPreparation=0 "
           "memorySent=%llu memoryCompleted=%llu pendingChecks=%llu "
           "pendingOwners=%llu pendingMemory=%u\n", s.committed, s.submitted,
           s.completed, s.checked, s.loadChecked, s.zeroChecked, s.mmaChecked,
           s.storeChecked, s.releaseChecked, s.syncChecked, s.staleReleases,
           s.outOfOrder, s.memorySent,
           s.memoryCompleted, s.checkOrder.size(), s.owners.size(), memory_pending);
    panic_if(matrixHasPendingWork() || !s.checkOrder.empty() || memory_pending ||
             s.submitted != s.checked || s.memorySent != s.memoryCompleted,
             "Matrix difftest ended with unverified work");
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        NemuAmuCtrlEvent amu = {};
        NemuMsyncEvent sync = {};
        const auto &proxy = diffAllStates.at(tid)->proxy;
        panic_if(proxy->amu_ctrl(&amu) != -1 || proxy->msync_event(&sync) != -1,
                 "Matrix reference has unconsumed control/sync events");
        uint64_t base = ~uint64_t(0), stride = ~uint64_t(0);
        uint32_t rows = ~uint32_t(0), cols = ~uint32_t(0);
        uint32_t width = ~uint32_t(0);
        bool transpose = false;
        const int store = proxy->matrix_store_commit(
            &base, &stride, &rows, &cols, &width, &transpose);
        // An empty queue returns one without changing the sentinels.
        // A real store either matches (zero) or replaces the arguments.
        panic_if(store != 1 || base != ~uint64_t(0) || stride != ~uint64_t(0) ||
                 rows != ~uint32_t(0) || cols != ~uint32_t(0) ||
                 width != ~uint32_t(0) || transpose,
                 "Matrix reference has an unconsumed store or inactive "
                 "store checking API");
    }
}

} // namespace gem5::o3

namespace gem5
{

namespace o3
{

namespace
{

std::vector<bool>
byteEnableFromMask(uint64_t byte_mask, uint32_t packet_size)
{
    std::vector<bool> byte_enable(packet_size, byte_mask == 0);
    for (uint32_t byte = 0; byte < packet_size && byte < 64; ++byte) {
        if ((byte_mask & (uint64_t(1) << byte)) != 0) {
            byte_enable[byte] = true;
        }
    }
    return byte_enable;
}

} // anonymous namespace

MatrixMemPort::MatrixMemPort(const std::string &name, CPU *cpu_)
    : RequestPort(name), stats(cpu_), cpu(cpu_)
{
}

MatrixMemPort::PortStats::PortStats(statistics::Group *parent)
    : statistics::Group(parent, "matrixMemoryPort"),
      ADD_STAT(sourcesAccepted, statistics::units::Count::get(),
               "Backend sources accepted by the timing adapter"),
      ADD_STAT(sourcesCompleted, statistics::units::Count::get(),
               "Sources completed at final read data or store acknowledgement"),
      ADD_STAT(sourceTicks, statistics::units::Tick::get(),
               "Total source lifetime including preparation and retry"),
      ADD_STAT(sourceTicksMax, statistics::units::Tick::get(),
               "Maximum source lifetime"),
      ADD_STAT(sourcesMax, statistics::units::Count::get(),
               "Maximum sources awaiting final response")
{
}

void
MatrixMemPort::noteSourceCompletion(uint32_t source)
{
    const auto it = sourceStarted.find(source);
    panic_if(it == sourceStarted.end(), "Missing Matrix source lifetime");
    const Tick lifetime = curTick() - it->second;
    sourceStarted.erase(it);
    stats.sourcesCompleted++;
    stats.sourceTicks += lifetime;
    stats.sourceTicksMax = std::max<double>(stats.sourceTicksMax.value(), lifetime);
}

PacketPtr
MatrixMemPort::buildPacket(const Request &request)
{
    auto req = std::make_shared<gem5::Request>(
        request.paddr, request.packetSize, gem5::Request::PHYSICAL,
        cpu->dataRequestorId());
    req->taskId(cpu->taskId());
    if (request.contextId != InvalidContextID) {
        req->setContext(request.contextId);
    }
    if (request.isStore) {
        assert(request.packetSize <= request.data.size());
        auto enabled = byteEnableFromMask(request.byteMask, request.packetSize);
        static bool injected = false;
        const char *fault = std::getenv("GEM5_MATRIX_TEST_FAULT");
        if (!injected && fault &&
            std::strcmp(fault, "store-mask") == 0) {
            for (unsigned i = 0; i < enabled.size(); ++i) {
                if (enabled[i]) {
                    enabled[i] = false;
                    injected = true;
                    warn("Inject Matrix store mask [tid:%u] [sn:%llu] "
                         "address=%#llx byte=%u", request.tid, request.seq,
                         request.paddr, i);
                    break;
                }
            }
        }
        req->setByteEnable(enabled);
    }
    auto *pkt = new Packet(req, request.isStore ?
        MemCmd::WriteReq : MemCmd::ReadReq);
    auto *data = new uint8_t[request.packetSize]{};
    if (request.isStore) {
        std::memcpy(data, request.data.data(), request.packetSize);
    }
    pkt->dataDynamic(data);
    pkt->senderState = new SenderState(request);
    static bool data_injected = false;
    const char *fault = std::getenv("GEM5_MATRIX_TEST_FAULT");
    if (!data_injected && request.isStore && fault &&
        std::strcmp(fault, "store-data") == 0) {
        for (unsigned byte = 0; byte < request.packetSize; ++byte) {
            if (!request.byteMask || (request.byteMask & (uint64_t(1) << byte))) {
                data[byte] ^= 1;
                data_injected = true;
                warn("Inject Matrix store data [tid:%u] [sn:%llu] "
                     "address=%#llx byte=%u", request.tid, request.seq,
                     request.paddr, byte);
                break;
            }
        }
    }
    return pkt;
}

bool
MatrixMemPort::sendTimingRequest(const Request &request)
{
    panic_if(!sourceStarted.emplace(request.sourceId, curTick()).second,
             "Matrix source reused before final response");
    stats.sourcesAccepted++;
    stats.sourcesMax = std::max<double>(stats.sourcesMax.value(), sourceStarted.size());
    blockedPackets.push_back(buildPacket(request));
    if (!waitingForRetry) {
        trySendBlocked();
    }
    return true;
}

bool
MatrixMemPort::recvTimingResp(PacketPtr pkt)
{
    auto *state = dynamic_cast<SenderState *>(pkt->senderState);
    panic_if(state == nullptr,
             "Matrix memory response missing sender state");

    const uint32_t source_id = state->request.sourceId;
    const bool valid_response = state->request.isStore ?
        pkt->cmd == MemCmd::WriteResp :
        (pkt->cmd == MemCmd::ReadResp ||
         pkt->cmd == MemCmd::ReadRespWithInvalidate);
    panic_if(!valid_response,
             "Matrix memory received unexpected response command %s",
             pkt->cmd.toString());
    const bool has_data = pkt->hasData() && pkt->getSize() != 0;
    const uint32_t data_size = has_data ? pkt->getSize() : 0;
    std::vector<uint8_t> data;
    if (has_data) {
        data.resize(data_size);
        std::memcpy(data.data(), pkt->getPtr<uint8_t>(), data.size());
    }

    cpu->recordMatrixResponse(state->request);
    delete pkt->senderState;
    pkt->senderState = nullptr;
    delete pkt;

    noteSourceCompletion(source_id);

    const bool completed =
        cpu->matrixState->backend &&
        cpu->matrixState->backend->completeTimingMemoryResponse(
            source_id, data.empty() ? nullptr : data.data(), data.size());
    panic_if(!completed,
             "Matrix memory response for unknown source %u", source_id);

    cpu->activityRec.activity();
    cpu->scheduleTickEvent(Cycles(0));
    return true;
}

void
MatrixMemPort::trySendBlocked()
{
    while (!blockedPackets.empty()) {
        PacketPtr pkt = blockedPackets.front();
        auto *sender = static_cast<SenderState *>(pkt->senderState);
        if (cpu->difftestEnabled() && sender->request.isStore) {
            auto &observed = sender->request;
            observed.paddr = pkt->getAddr();
            observed.packetSize = pkt->getSize();
            std::memcpy(observed.data.data(), pkt->getConstPtr<uint8_t>(),
                        observed.packetSize);
            observed.byteMask = 0;
            const auto &enabled = pkt->req->getByteEnable();
            for (unsigned i = 0; i < observed.packetSize; ++i) {
                if (enabled.empty() || enabled.at(i)) {
                    observed.byteMask |= uint64_t(1) << i;
                }
            }
        }
        if (!sendTimingReq(pkt)) {
            waitingForRetry = true;
            return;
        }
        if (cpu->difftestEnabled()) {
            ++cpu->matrixState->memorySent;
        }
        blockedPackets.pop_front();
    }
}

void
MatrixMemPort::recvReqRetry()
{
    panic_if(!waitingForRetry, "Unexpected Matrix request retry");
    waitingForRetry = false;
    trySendBlocked();
    cpu->activityRec.activity();
    cpu->scheduleTickEvent(Cycles(0));
}

} // namespace o3
} // namespace gem5

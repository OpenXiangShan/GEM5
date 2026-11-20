/*
 * Copyright (c) 2011-2012, 2014, 2017-2019, 2021 ARM Limited
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
 * Copyright (c) 2005-2006 The Regents of The University of Michigan
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

#include "cpu/o3/mls_unit.hh"

#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <utility>

#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/exec_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/op_class.hh"
#include "debug/LSQ.hh"
#include "matrix/MemoryLoader.hh"
#include "mem/request.hh"
#include "params/BaseO3CPU.hh"
#include "sim/cur_tick.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

namespace
{


class MlsTranslation : public BaseMMU::Translation
{
  public:
    MlsTranslation(
        CPU *cpu_,
        const DynInstPtr &inst_,
        std::shared_ptr<MlsReplayQueue::TranslationContext> context_)
        : cpu(cpu_), inst(inst_), context(std::move(context_))
    {
        assert(cpu);
        assert(context);
    }

    void
    markDelayed() override
    {
    }

    bool
    squashed() const override
    {
        return context->squashed || !inst || inst->isSquashed();
    }

    void
    finish(const Fault &fault, const RequestPtr &req,
           gem5::ThreadContext *tc, BaseMMU::Mode mode) override
    {
        if (squashed()) {
            context->finished = true;
            delete this;
            return;
        }
        Fault access_fault = fault;
        if (access_fault == NoFault) {
            assert(req->hasPaddr());
            const auto &lines = context->pageLines[context->nextPage];
            if (auto addr = unsupportedAddress(req, tc, lines)) {
                const auto code = mode == BaseMMU::Read ?
                    RiscvISA::LOAD_ACCESS : RiscvISA::STORE_ACCESS;
                access_fault = std::make_shared<RiscvISA::AddressFault>(
                    *addr, 0, code);
            }
        }
        if (access_fault == NoFault) {
            constexpr Addr page_mask = 4095;
            context->pageMappings->push_back({
                req->getVaddr() & ~page_mask, req->getPaddr() & ~page_mask});
            ++context->nextPage;
            if (context->nextPage < context->pageRequests.size()) {
                cpu->mmu->translateTiming(
                    context->pageRequests[context->nextPage], tc,
                    new MlsTranslation(cpu, inst, context), mode);
                delete this;
                return;
            }
        }
        if (access_fault == NoFault) {
            std::sort(context->pageMappings->begin(),
                      context->pageMappings->end(),
                      [](const auto &a, const auto &b) {
                          return a.vpage < b.vpage;
                      });
        }
        context->fault = access_fault;
        context->finished = true;
        if (inst && !inst->isSquashed()) {
            inst->translatedTick = curTick();
            inst->translationCompleted(true);
            inst->getFault() = access_fault;
            if (access_fault == NoFault) {
                inst->physEffAddr = context->request->getPaddr();
                inst->memReqFlags = context->request->getFlags();
            }
            cpu->wakeCPU();
            cpu->activityThisCycle();
        }
        delete this;
    }

  private:
    static std::optional<Addr>
    unsupportedAddress(const RequestPtr &req, gem5::ThreadContext *tc,
                       const std::array<uint8_t, 64> &lines)
    {
        if (req->isUncacheable() || req->isStrictlyOrdered()) {
            return req->getVaddr();
        }

        // The backend sends whole cache lines, even for a partial tile.
        constexpr size_t line_size = matrix::timing_load_detail::CacheLineBytes;
        const auto &ram = tc->getSystemPtr()->params().mem_ranges;
        const auto *pma = static_cast<RiscvISA::MMU *>(tc->getMMUPtr())->pma;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] == line_size) {
                continue;
            }
            const Addr offset = i * line_size;
            const AddrRange beat = RangeSize(
                (req->getPaddr() & ~Addr(4095)) + offset, line_size);
            const bool in_ram = std::any_of(ram.begin(), ram.end(),
                [&](const auto &r) { return beat.isSubset(r); });
            const bool uncached = std::any_of(
                pma->uncacheable.begin(), pma->uncacheable.end(),
                [&](const auto &r) {
                    if (!r.interleaved()) {
                        return beat.intersects(r);
                    }
                    for (size_t byte = 0; byte < line_size; ++byte) {
                        if (r.contains(beat.start() + byte)) {
                            return true;
                        }
                    }
                    return false;
                });
            if (!in_ram || uncached) {
                return (req->getVaddr() & ~Addr(4095)) + offset + lines[i];
            }
        }
        return std::nullopt;
    }

    CPU *cpu;
    DynInstPtr inst;
    std::shared_ptr<MlsReplayQueue::TranslationContext> context;
};


} // anonymous namespace

MlsVirtualQueue::MlsVirtualQueue(unsigned num_threads, unsigned capacity)
    : queueCapacity(capacity),
      queues(num_threads),
      entries(num_threads, std::vector<Entry>(capacity)),
      nextSlots(num_threads, 0)
{
}

bool
MlsVirtualQueue::canAllocate(ThreadID tid, unsigned count) const
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);
    return queues[tid].size() + count <= queueCapacity;
}

MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryBySlot(ThreadID tid, unsigned slot)
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

const MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryBySlot(ThreadID tid, unsigned slot) const
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    const auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryByInst(const DynInstPtr &inst)
{
    if (!inst || inst->threadNumber >= queues.size()) {
        return nullptr;
    }

    if (inst->matrixData().mlsqSlot) {
        auto *entry =
            findEntryBySlot(inst->threadNumber, *inst->matrixData().mlsqSlot);
        if (entry && entry->robSeqNum == inst->seqNum) {
            return entry;
        }
    }

    return nullptr;
}

const MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryByInst(const DynInstPtr &inst) const
{
    if (!inst || inst->threadNumber >= queues.size()) {
        return nullptr;
    }

    if (inst->matrixData().mlsqSlot) {
        auto *entry =
            findEntryBySlot(inst->threadNumber, *inst->matrixData().mlsqSlot);
        if (entry && entry->robSeqNum == inst->seqNum) {
            return entry;
        }
    }

    return nullptr;
}

bool
MlsVirtualQueue::hasEntry(const DynInstPtr &inst) const
{
    return findEntryByInst(inst) != nullptr;
}

bool
MlsVirtualQueue::allocate(const DynInstPtr &inst)
{
    panic_if(!inst, "Attempted to allocate null matrix mem instruction");
    const ThreadID tid = inst->threadNumber;
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);

    if (hasEntry(inst) || !canAllocate(tid)) {
        return false;
    }

    const unsigned slot = nextSlots[tid];
    auto &entry = entries[tid][slot];
    panic_if(entry.allocated,
             "MLSQ slot still allocated [tid:%i] slot=%u [sn:%llu]",
             tid, slot, inst->seqNum);

    entry = {};
    entry.robSeqNum = inst->seqNum;
    entry.allocated = true;
    queues[tid].push_back(slot);
    nextSlots[tid] = (slot + 1) % queueCapacity;
    inst->matrixData().mlsqSlot = slot;

    return true;
}

bool
MlsVirtualQueue::markFinished(const DynInstPtr &inst)
{
    auto *entry = findEntryByInst(inst);
    if (!entry) {
        return false;
    }

    entry->finished = true;
    return true;
}

void
MlsVirtualQueue::waitForReplaySlot(const DynInstPtr &inst)
{
    auto *entry = findEntryByInst(inst);
    assert(entry && !entry->waitingForReplay);
    entry->waitingForReplay = inst;
}

DynInstPtr
MlsVirtualQueue::takeReplayWaiter(ThreadID tid)
{
    for (unsigned slot : queues[tid]) {
        auto &waiting = entries[tid][slot].waitingForReplay;
        if (waiting && !waiting->isSquashed()) {
            return std::move(waiting);
        }
    }
    return nullptr;
}

unsigned
MlsVirtualQueue::retireCommitted(ThreadID tid, InstSeqNum committed_seq)
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);

    auto &queue = queues[tid];
    unsigned retired = 0;
    while (!queue.empty()) {
        auto &head = entries[tid][queue.front()];
        if (!head.finished || head.robSeqNum > committed_seq) {
            break;
        }

        head = {};
        queue.pop_front();
        retired++;
    }

    return retired;
}

unsigned
MlsVirtualQueue::squash(ThreadID tid, InstSeqNum squash_seq)
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);

    auto &queue = queues[tid];
    unsigned canceled = 0;
    while (!queue.empty()) {
        auto &tail = entries[tid][queue.back()];
        if (tail.robSeqNum <= squash_seq) {
            break;
        }

        tail = {};
        queue.pop_back();
        canceled++;
    }

    if (canceled != 0) {
        nextSlots[tid] =
            (nextSlots[tid] + queueCapacity - (canceled % queueCapacity)) %
            queueCapacity;
    }

    return canceled;
}

unsigned
MlsVirtualQueue::freeEntries(ThreadID tid) const
{
    return queueCapacity - size(tid);
}

unsigned
MlsVirtualQueue::size(ThreadID tid) const
{
    panic_if(tid >= queues.size(), "Invalid thread id %u for MlsVirtualQueue", tid);
    return queues[tid].size();
}

MlsReplayQueue::MlsReplayQueue(unsigned num_threads, unsigned capacity,
                               Tick replay_select_latency)
    : replaySelectLatency(replay_select_latency),
      entries(num_threads, std::vector<Entry>(capacity))
{
}

MlsReplayQueue::ReplayState *
MlsReplayQueue::reserve(const DynInstPtr &inst)
{
    auto &slots = entries.at(inst->threadNumber);
    auto &index = inst->matrixData().replaySlot;
    if (!index) {
        const auto it = std::find_if(slots.begin(), slots.end(),
            [](const auto &slot) { return !slot.allocated; });
        if (it == slots.end()) {
            return nullptr;
        }
        index = it - slots.begin();
        it->allocated = true;
        it->robSeqNum = inst->seqNum;
        it->inst = inst;
    }
    auto &entry = slots[*index];
    assert(entry.inst == inst);
    return &entry.state;
}

bool
MlsReplayQueue::scheduleNext(ThreadID tid, DynInstPtr &inst_out)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    Entry *selected = nullptr;
    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.scheduled ||
            !entry.state.translationContext ||
            !entry.state.translationContext->finished) {
            continue;
        }
        if (!entry.ready) {
            entry.ready = true;
            entry.availableTick = curTick() + replaySelectLatency;
        }
        if (entry.availableTick > curTick()) {
            continue;
        }
        if (!selected || entry.robSeqNum < selected->robSeqNum) {
            selected = &entry;
        }
    }

    if (!selected) {
        return false;
    }

    selected->scheduled = true;
    inst_out = selected->inst;
    return true;
}

void
MlsReplayQueue::freeEntry(Entry &entry)
{
    if (entry.state.translationContext) {
        entry.state.translationContext->squashed = true;
    }
    if (entry.inst) {
        entry.inst->matrixData().replaySlot.reset();
    }
    entry = {};
}

void
MlsReplayQueue::complete(const DynInstPtr &inst)
{
    auto &entry = entries.at(inst->threadNumber).at(*inst->matrixData().replaySlot);
    assert(entry.inst == inst);
    freeEntry(entry);
}

unsigned
MlsReplayQueue::squash(ThreadID tid, InstSeqNum squash_seq)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    unsigned canceled = 0;
    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.robSeqNum <= squash_seq) {
            continue;
        }

        freeEntry(entry);
        canceled++;
    }
    return canceled;
}

matrix::MatrixElemType
toMatrixElemType(RiscvISA::MatrixElemKind elem_kind)
{
    switch (elem_kind) {
      case RiscvISA::MatrixElemKind::Int8:
        return matrix::MatrixElemType::Int8;
      case RiscvISA::MatrixElemKind::Fp16:
        return matrix::MatrixElemType::Fp16;
      case RiscvISA::MatrixElemKind::Int32:
        return matrix::MatrixElemType::Int32;
      case RiscvISA::MatrixElemKind::None:
        break;
    }

    panic("Unsupported matrix MLS element kind %#x",
          static_cast<unsigned>(elem_kind));
}

RegVal
trLimitForWidths(uint8_t widths)
{
    if (widths == RiscvISA::MatrixSewE16) {
        return RiscvISA::MatrixTrLenE16Max;
    }
    return RiscvISA::MatrixTrLenE8Max;
}

const char *
matrixMemCheckName(const MatrixDynInstState::InstInfo &info, uint8_t widths)
{
    if (info.lsuIsA) {
        return widths == RiscvISA::MatrixSewE16 ?
            "mlae16 parameter check failed" :
            "mlae8 parameter check failed";
    }
    if (info.lsuIsB) {
        return widths == RiscvISA::MatrixSewE16 ?
            "mlbe16 parameter check failed" :
            "mlbe8 parameter check failed";
    }
    if (info.storeLike) {
        return "msce32 parameter check failed";
    }
    return "mlce32 parameter check failed";
}

Fault
MlsUnit::matrixMemEarlyFault(
    const DynInstPtr &inst, const MlsReplayQueue::ReplayState &state) const
{
    const auto &matrix = inst->matrixData();
    const auto &info = matrix.matrixInst;
    const auto *riscv_inst =
        dynamic_cast<const RiscvISA::RiscvStaticInst *>(inst->staticInst.get());
    panic_if(!riscv_inst, "Matrix mem inst missing RISC-V static inst");

    auto illegal = [&](const char *msg) -> Fault {
        return std::make_shared<RiscvISA::IllegalInstFault>(
            msg, riscv_inst->machInst);
    };

    if (!RiscvISA::matrix_static_info::lsuEncodingLegal(
            info, matrix.lsuWidths, matrix.lsuElemKind) ||
        state.tile0 == 0 || state.tile1 == 0) {
        return illegal(matrixMemCheckName(info, matrix.lsuWidths));
    }

    if (info.lsuIsA) {
        if (state.tile0 > RiscvISA::MatrixRowNum ||
            state.tile1 > trLimitForWidths(matrix.lsuWidths)) {
            return illegal(matrixMemCheckName(info, matrix.lsuWidths));
        }
    } else if (info.lsuIsB) {
        if (state.tile1 > RiscvISA::MatrixRowNum ||
            state.tile0 > trLimitForWidths(matrix.lsuWidths)) {
            return illegal(matrixMemCheckName(info, matrix.lsuWidths));
        }
    } else if (info.lsuIsAcc) {
        if (state.tile0 > RiscvISA::MatrixRowNum ||
            state.tile1 > RiscvISA::MatrixAccE32Max) {
            return illegal(matrixMemCheckName(info, matrix.lsuWidths));
        }
    }

    return NoFault;
}


Fault
MlsUnit::prepareTranslation(const DynInstPtr &inst,
                             MlsReplayQueue::ReplayState &state,
                             Fault fault) const
{
    inst->freezeMatrixConfig();
    const auto &matrix = inst->matrixData();
    const auto &info = matrix.matrixInst;
    const auto mode = info.loadLike ? BaseMMU::Read : BaseMMU::Write;
    const unsigned access_size = matrix.lsuAccessSize;
    if (fault == NoFault) {
        fault = matrixMemEarlyFault(inst, state);
    }
    if (fault != NoFault) {
        inst->getFault() = fault;
        return fault;
    }

    inst->effAddr = state.vaddr;
    inst->effSize = access_size;
    inst->effAddrValid(true);
    inst->translationStarted(true);

    if (!state.request) {
        state.request = std::make_shared<Request>(
            state.vaddr, access_size, Request::Flags{},
            cpu->dataRequestorId(), inst->pcState().instAddr(),
            inst->contextId());
        state.request->taskId(cpu->taskId());
        state.request->setReqInstSeqNum(inst->seqNum);
    }

    if (!state.translationContext) {
        inst->translationCompleted(false);
        state.translationContext =
            std::make_shared<MlsReplayQueue::TranslationContext>();
        state.translationContext->request = state.request;
        auto &ctx = *state.translationContext;

        // Enumerate only pages containing tensor bytes. The bounded
        // shape contract limits this state to at most 256 pages; one
        // timing translation is active per instruction.
        matrix::AmuLsuDesc desc;
        desc.baseAddr = desc.physBaseAddr = state.vaddr;
        desc.stride = state.stride;
        desc.row = state.tile0;
        desc.column = state.tile1;
        desc.transpose = info.lsuTranspose;
        desc.isB = info.lsuIsB;
        desc.elemType = toMatrixElemType(matrix.lsuElemKind);
        constexpr size_t line_size = matrix::timing_load_detail::CacheLineBytes;
        static_assert(4096 / line_size == 64);
        std::map<Addr, std::array<uint8_t, 64>> pages;
        matrix::timing_load_detail::forEachTensorByte(
            desc, {}, [&](Addr line, size_t offset, size_t, size_t) {
                auto [page, inserted] = pages.try_emplace(line & ~Addr(4095));
                if (inserted) {
                    page->second.fill(line_size);
                }
                auto &first = page->second[(line & 4095) / line_size];
                first = std::min(first, static_cast<uint8_t>(offset));
            });
        const Addr first_page = state.vaddr & ~Addr(4095);
        ctx.pageRequests.push_back(state.request);
        ctx.pageLines.push_back(pages.at(first_page));
        for (const auto &[page, lines] : pages) {
            if (page == first_page) {
                continue;
            }
            const auto first = std::find_if(lines.begin(), lines.end(),
                [](uint8_t offset) { return offset < line_size; });
            const Addr addr = page + (first - lines.begin()) * line_size + *first;
            auto request = std::make_shared<Request>(
                addr, 1, Request::Flags{}, cpu->dataRequestorId(),
                inst->pcState().instAddr(), inst->contextId());
            request->taskId(cpu->taskId());
            request->setReqInstSeqNum(inst->seqNum);
            ctx.pageRequests.push_back(std::move(request));
            ctx.pageLines.push_back(lines);
        }
        cpu->mmu->translateTiming(
            state.request, inst->tcBase(),
            new MlsTranslation(cpu, inst, state.translationContext),
            mode);
    }

    fault = state.translationContext->finished ?
        state.translationContext->fault : NoFault;
    inst->translationCompleted(state.translationContext->finished);
    inst->translatedTick = curTick();
    inst->getFault() = fault;

    if (fault == NoFault && state.translationContext->finished) {
        inst->physEffAddr = state.request->getPaddr();
        inst->memReqFlags = state.request->getFlags();
    } else if (fault != NoFault && state.translationContext) {
        state.translationContext->finished = true;
        state.translationContext->fault = fault;
    }
    return fault;
}

MlsUnit::MlsUnit(CPU *cpu_, const BaseO3CPUParams &params)
    : cpu(cpu_),
      virtualQueue(params.numThreads, params.matrixMlsVirtualQueueEntries),
      replayQueue(params.numThreads, params.matrixMlsReplayQueueEntries,
                  params.matrixMlsReplaySelectLatency)
{
    fatal_if(!params.matrixMlsReplayQueueEntries ||
             !params.matrixMlsVirtualQueueEntries,
             "Matrix MLS queues require at least one entry");
}

bool
MlsUnit::issue(const DynInstPtr &inst)
{
    assert(inst);
    assert(inst->isMatrixInst());
    assert(inst->opClass() == MatrixMemOp);

    // Reserve before the first translation. If all slots are in use,
    // wait in the virtual queue until a slot becomes available.
    auto *entry = replayQueue.reserve(inst);
    if (!entry) {
        virtualQueue.waitForReplaySlot(inst);
        return true;
    }
    auto &state = *entry;
    if (!state.translationContext) {
        state.vaddr = inst->getRegOperand(inst->staticInst.get(), 0);
        state.stride = inst->getRegOperand(inst->staticInst.get(), 1);
        state.tile0 = inst->getRegOperand(inst->staticInst.get(), 2);
        state.tile1 = inst->getRegOperand(inst->staticInst.get(), 3);
        // Preserve the modeled CSR read without an unused TLB probe.
        inst->readMiscReg(RiscvISA::MISCREG_SATP);
    }
    const Fault fault = prepareTranslation(inst, state, inst->getFault());
    if (fault == NoFault && !state.translationContext->finished) {
        return true;
    }
    if (fault == NoFault) {
        const auto &matrix = inst->matrixData();
        const auto &info = matrix.matrixInst;
        matrix::CuteRequest request;
        request.kind = matrix::CuteRequestKind::Lsu;
        request.op = info.funct7;
        auto &desc = request.lsu;
        desc.isStore = info.storeLike;
        desc.isA = info.lsuIsA;
        desc.isB = info.lsuIsB;
        desc.isAcc = info.lsuIsAcc;
        desc.transpose = info.lsuTranspose;
        desc.ms = info.rd & 0x7;
        desc.baseAddr = state.vaddr;
        desc.physBaseAddr = state.request->getPaddr();
        desc.pageMappings = state.translationContext->pageMappings;
        desc.stride = state.stride;
        desc.row = state.tile0;
        desc.column = state.tile1;
        desc.widths = matrix.lsuWidths;
        desc.elemType = toMatrixElemType(matrix.lsuElemKind);
        inst->stageMatrixRequest(request);
    }

    const bool marked = virtualQueue.markFinished(inst);
    panic_if(!marked,
             "Matrix virtual queue entry missing at finish "
             "[tid:%i] [sn:%llu]",
             inst->threadNumber, inst->seqNum);

    if (fault == NoFault && !inst->readPredicate()) {
        inst->forwardOldRegs();
    }

    replayQueue.complete(inst);
    retryWaiting(inst->threadNumber);
    return false;
}

void
MlsUnit::retryWaiting(ThreadID tid)
{
    if (auto waiter = virtualQueue.takeReplayWaiter(tid)) {
        // Reserve the freed slot before reissuing so younger instructions
        // cannot repeatedly overtake an instruction waiting for translation.
        auto *reserved = replayQueue.reserve(waiter);
        panic_if(!reserved, "Freed Matrix replay slot was unavailable");
        waiter->issueQue->retryMem(waiter);
    }
}

bool
MlsUnit::canAllocate(ThreadID tid, unsigned count) const
{
    return virtualQueue.canAllocate(tid, count);
}

bool
MlsUnit::allocate(const DynInstPtr &inst)
{
    return virtualQueue.allocate(inst);
}

void
MlsUnit::scheduleReplay(
    const std::list<ThreadID> &active_threads)
{
    for (auto tid : active_threads) {
        DynInstPtr replay_inst;
        if (!replayQueue.scheduleNext(tid, replay_inst)) {
            continue;
        }

        panic_if(!replay_inst || !replay_inst->issueQue,
                 "Matrix replay inst missing issue queue [tid:%i]", tid);
        replay_inst->issueQue->retryMem(replay_inst);
        DPRINTF(LSQ,
                "[tid:%i] Matrix replay scheduled [sn:%llu].\n",
                tid, replay_inst->seqNum);
    }
}

unsigned
MlsUnit::squash(ThreadID tid, InstSeqNum squash_seq)
{
    const unsigned mlsqCanceled = virtualQueue.squash(tid, squash_seq);
    if (mlsqCanceled != 0) {
        DPRINTF(LSQ,
                "[tid:%i] MlsVirtualQueue squash canceled=%u free=%u.\n",
            tid, mlsqCanceled, virtualQueue.freeEntries(tid));
    }

    const unsigned replayCanceled = replayQueue.squash(tid, squash_seq);
    for (unsigned slot = 0; slot < replayCanceled; ++slot) {
        retryWaiting(tid);
    }
    if (replayCanceled != 0) {
        DPRINTF(LSQ, "[tid:%i] MlsReplayQueue squash canceled=%u.\n",
                tid, replayCanceled);
    }

    return mlsqCanceled + replayCanceled;
}

unsigned
MlsUnit::retireCommitted(ThreadID tid, InstSeqNum committed_seq)
{
    const unsigned retired = virtualQueue.retireCommitted(tid, committed_seq);
    if (retired != 0) {
        DPRINTF(LSQ,
                "[tid:%i] MlsVirtualQueue retire count=%u free=%u.\n",
            tid, retired, virtualQueue.freeEntries(tid));
    }
    return retired;
}

} // namespace o3
} // namespace gem5

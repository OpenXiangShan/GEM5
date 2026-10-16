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
#include "arch/riscv/tlb.hh"
#include "base/logging.hh"
#include "cpu/exec_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/op_class.hh"
#include "matrix/MemoryLoader.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"
#include "sim/full_system.hh"

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
        if (fault == NoFault) {
            panic_if(!req->hasPaddr(), "Matrix page translation lacks paddr");
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
        if (fault == NoFault) {
            std::sort(context->pageMappings->begin(),
                      context->pageMappings->end(),
                      [](const auto &a, const auto &b) {
                          return a.vpage < b.vpage;
                      });
        }
        context->fault = fault;
        context->finished = true;
        if (inst && !inst->isSquashed()) {
            inst->translatedTick = curTick();
            inst->translationCompleted(true);
            inst->getFault() = fault;
            if (fault == NoFault) {
                inst->physEffAddr = context->request->getPaddr();
                inst->memReqFlags = context->request->getFlags();
            }
            cpu->wakeCPU();
            cpu->activityThisCycle();
        }
        delete this;
    }

  private:
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

    if (inst->hasMatrixMlsqSlot()) {
        auto *entry =
            findEntryBySlot(inst->threadNumber, inst->getMatrixMlsqSlot());
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

    if (inst->hasMatrixMlsqSlot()) {
        auto *entry =
            findEntryBySlot(inst->threadNumber, inst->getMatrixMlsqSlot());
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
    inst->setMatrixMlsqSlot(slot);

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
    : queueCapacity(capacity),
      replaySelectLatency(replay_select_latency),
      entries(num_threads, std::vector<Entry>(capacity))
{
}

MlsReplayQueue::Entry *
MlsReplayQueue::findEntryBySlot(ThreadID tid, unsigned slot)
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

const MlsReplayQueue::Entry *
MlsReplayQueue::findEntryBySlot(ThreadID tid, unsigned slot) const
{
    if (tid >= entries.size() || slot >= queueCapacity) {
        return nullptr;
    }

    const auto &entry = entries[tid][slot];
    return entry.allocated ? &entry : nullptr;
}

MlsReplayQueue::Entry *
MlsReplayQueue::findEntryByInst(const DynInstPtr &inst)
{
    if (!inst || inst->threadNumber >= entries.size() ||
        !inst->hasMatrixMlsReplaySlot()) {
        return nullptr;
    }

    auto *entry =
        findEntryBySlot(inst->threadNumber, inst->getMatrixMlsReplaySlot());
    if (entry && entry->robSeqNum == inst->seqNum) {
        return entry;
    }

    return nullptr;
}

const MlsReplayQueue::Entry *
MlsReplayQueue::findEntryByInst(const DynInstPtr &inst) const
{
    if (!inst || inst->threadNumber >= entries.size() ||
        !inst->hasMatrixMlsReplaySlot()) {
        return nullptr;
    }

    auto *entry =
        findEntryBySlot(inst->threadNumber, inst->getMatrixMlsReplaySlot());
    if (entry && entry->robSeqNum == inst->seqNum) {
        return entry;
    }

    return nullptr;
}

bool
MlsReplayQueue::hasEntry(const DynInstPtr &inst) const
{
    return findEntryByInst(inst) != nullptr;
}

const MlsReplayQueue::ReplayState *
MlsReplayQueue::getState(const DynInstPtr &inst) const
{
    const auto *entry = findEntryByInst(inst);
    return entry ? &entry->state : nullptr;
}

std::optional<unsigned>
MlsReplayQueue::allocateSlot(ThreadID tid)
{
    for (unsigned slot = 0; slot < queueCapacity; ++slot) {
        if (!entries[tid][slot].allocated) {
            return slot;
        }
    }
    return std::nullopt;
}

bool
MlsReplayQueue::allocateOrUpdate(
    const DynInstPtr &inst, const ReplayState &state, bool ready)
{
    panic_if(!inst, "Attempted to allocate null matrix replay entry");
    panic_if(state.cause == ReplayCause::None,
             "Matrix replay entry missing cause [tid:%i] [sn:%llu]",
             inst->threadNumber, inst->seqNum);
    const ThreadID tid = inst->threadNumber;
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    if (auto *entry = findEntryByInst(inst)) {
        if (entry->state.translationContext &&
            entry->state.translationContext != state.translationContext) {
            entry->state.translationContext->squashed = true;
        }
        entry->scheduled = false;
        entry->ready = ready;
        entry->availableTick = ready ? curTick() + replaySelectLatency : 0;
        entry->state = state;
        return true;
    }

    auto slot = allocateSlot(tid);
    if (!slot) {
        return false;
    }

    auto &entry = entries[tid][*slot];
    entry = {};
    entry.allocated = true;
    entry.scheduled = false;
    entry.ready = ready;
    entry.availableTick = ready ? curTick() + replaySelectLatency : 0;
    entry.robSeqNum = inst->seqNum;
    entry.inst = inst;
    entry.state = state;
    inst->setMatrixMlsReplaySlot(*slot);
    return true;
}

void
MlsReplayQueue::refreshReady(
    ThreadID tid,
    const std::function<bool(const DynInstPtr &, ReplayState &)> &ready_fn)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.scheduled) {
            continue;
        }

        const bool was_ready = entry.ready;
        entry.ready = ready_fn(entry.inst, entry.state);
        if (entry.ready && !was_ready) {
            entry.availableTick = curTick() + replaySelectLatency;
        }
    }
}

bool
MlsReplayQueue::scheduleNext(ThreadID tid, DynInstPtr &inst_out)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    Entry *selected = nullptr;
    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.scheduled || !entry.ready ||
            entry.availableTick > curTick()) {
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
        entry.inst->clearMatrixMlsReplaySlot();
    }
    entry = {};
}

bool
MlsReplayQueue::completeRetry(const DynInstPtr &inst)
{
    auto *entry = findEntryByInst(inst);
    if (!entry) {
        return false;
    }

    freeEntry(*entry);
    return true;
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

unsigned
MlsReplayQueue::size(ThreadID tid) const
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);
    unsigned total = 0;
    for (const auto &entry : entries[tid]) {
        total += entry.allocated ? 1 : 0;
    }
    return total;
}

unsigned
MlsReplayQueue::freeEntries(ThreadID tid) const
{
    return queueCapacity - size(tid);
}


struct MlsUnit::StageState
{
    RegVal vaddr = 0;
    Addr paddr = 0;
    RegVal stride = 0;
    RegVal tile0 = 0;
    RegVal tile1 = 0;
    RegVal mtilem = 0;
    RegVal mtilen = 0;
    RegVal mtilek = 0;
    BaseMMU::Mode mode = BaseMMU::Read;
    unsigned accessSize = 0;
    RequestPtr request;
    std::shared_ptr<MlsReplayQueue::TranslationContext> translationContext;
    Fault fault = NoFault;
    uint16_t asid = 0;
    bool tlbMiss = false;
    bool replayReady = false;
    bool needReplay = false;
    MlsReplayQueue::ReplayCause replayCause =
        MlsReplayQueue::ReplayCause::None;
    ExecContext::MatrixExecPayload payload = {};
};

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

template <class StageState>
RegVal
stateValueForOperand(const StageState &state,
                     RiscvISA::MatrixStateOperand operand)
{
    switch (operand) {
      case RiscvISA::MatrixStateOperand::Mtilem:
        return state.mtilem;
      case RiscvISA::MatrixStateOperand::Mtilen:
        return state.mtilen;
      case RiscvISA::MatrixStateOperand::Mtilek:
        return state.mtilek;
      case RiscvISA::MatrixStateOperand::None:
        break;
    }

    return 0;
}

template <class StageState>
void
setStateValueForOperand(StageState &state,
                        RiscvISA::MatrixStateOperand operand,
                        RegVal value)
{
    switch (operand) {
      case RiscvISA::MatrixStateOperand::Mtilem:
        state.mtilem = value;
        break;
      case RiscvISA::MatrixStateOperand::Mtilen:
        state.mtilen = value;
        break;
      case RiscvISA::MatrixStateOperand::Mtilek:
        state.mtilek = value;
        break;
      case RiscvISA::MatrixStateOperand::None:
        break;
    }
}

RegVal
trLimitForWidths(uint8_t widths)
{
    if (widths == RiscvISA::ISA::MatrixSewE16) {
        return RiscvISA::ISA::MatrixTrLenE16Max;
    }
    return RiscvISA::ISA::MatrixTrLenE8Max;
}

const char *
matrixMemCheckName(const DynInst::MatrixInstInfo &info)
{
    if (info.lsuIsA) {
        return info.lsuWidths == RiscvISA::ISA::MatrixSewE16 ?
            "mlae16 parameter check failed" :
            "mlae8 parameter check failed";
    }
    if (info.lsuIsB) {
        return info.lsuWidths == RiscvISA::ISA::MatrixSewE16 ?
            "mlbe16 parameter check failed" :
            "mlbe8 parameter check failed";
    }
    if (info.storeLike) {
        return "msce32 parameter check failed";
    }
    return "mlce32 parameter check failed";
}

unsigned
MlsUnit::matrixMemAccessSizeBytes(const DynInstPtr &inst) const
{
    const auto &info = inst->matrixInstInfo();
    return info.lsuAccessSize != 0 ? info.lsuAccessSize : 1;
}


Fault
MlsUnit::matrixMemEarlyFault(const DynInstPtr &inst,
                             const StageState &state) const
{
    const auto &info = inst->matrixInstInfo();
    const auto *riscv_inst =
        dynamic_cast<const RiscvISA::RiscvStaticInst *>(inst->staticInst.get());
    panic_if(!riscv_inst, "Matrix mem inst missing RISC-V static inst");

    auto illegal = [&](const char *msg) -> Fault {
        return std::make_shared<RiscvISA::IllegalInstFault>(
            msg, riscv_inst->machInst);
    };

    if (!RiscvISA::matrix_static_info::lsuEncodingLegal(info) ||
        state.tile0 == 0 || state.tile1 == 0) {
        return illegal(matrixMemCheckName(info));
    }

    if (info.lsuIsA) {
        if (stateValueForOperand(state, info.rowState) >
                RiscvISA::ISA::MatrixRowNum ||
            stateValueForOperand(state, info.columnState) >
                trLimitForWidths(info.lsuWidths)) {
            return illegal(matrixMemCheckName(info));
        }
    } else if (info.lsuIsB) {
        if (stateValueForOperand(state, info.columnState) >
                RiscvISA::ISA::MatrixRowNum ||
            stateValueForOperand(state, info.rowState) >
                trLimitForWidths(info.lsuWidths)) {
            return illegal(matrixMemCheckName(info));
        }
    } else if (info.lsuIsAcc) {
        if (stateValueForOperand(state, info.rowState) >
                RiscvISA::ISA::MatrixRowNum ||
            stateValueForOperand(state, info.columnState) >
                RiscvISA::ISA::MatrixAccE32Max) {
            return illegal(matrixMemCheckName(info));
        }
    }

    return NoFault;
}


void
MlsUnit::probeTlbState(StageState &state) const
{
    auto *mmu = dynamic_cast<RiscvISA::MMU *>(cpu->mmu);
    panic_if(!mmu, "Matrix MLS replay requires RISC-V MMU");
    auto *dtb = static_cast<RiscvISA::TLB *>(mmu->dtb);
    auto *entry = dtb->lookup(
        state.vaddr, state.asid, state.mode, true, false, RiscvISA::direct);
    state.tlbMiss = (entry == nullptr);
}

bool
MlsUnit::ensureReplayReady(
    const DynInstPtr &inst, MlsReplayQueue::ReplayState &state) const
{
    auto applyFinishedTranslation =
        [&](const std::shared_ptr<MlsReplayQueue::TranslationContext> &ctx) {
            if (!ctx || !ctx->finished) {
                return false;
            }

            state.request = ctx->request ? ctx->request : state.request;
            state.translationFault = ctx->fault != NoFault;
            if (ctx->fault != NoFault) {
                inst->translationCompleted(true);
                inst->translatedTick = curTick();
                inst->getFault() = ctx->fault;
                return true;
            }

            if (state.request && state.request->hasPaddr()) {
                state.paddr = state.request->getPaddr();
                state.requestFlags = state.request->getFlags();
                state.translationComplete = true;
                inst->physEffAddr = state.paddr;
                inst->memReqFlags = state.requestFlags;
                inst->translationCompleted(true);
                inst->translatedTick = curTick();
                inst->getFault() = NoFault;
                return true;
            }

            return false;
        };

    if (state.translationComplete || state.translationFault) {
        return true;
    }

    if (applyFinishedTranslation(state.translationContext)) {
        return true;
    }
    if (state.translationContext && !state.translationContext->finished) {
        inst->translationCompleted(false);
        return false;
    }

    if (inst->translationCompleted() && inst->getFault() != NoFault) {
        state.translationFault = true;
        return true;
    }

    // A replay without a live translation uses the same complete range
    // preparation as first issue. A translated first element alone must
    // never qualify a multi-page request for backend submission.
    StageState stage;
    restoreStage0FromReplay(inst, state, stage);
    runStage1(inst, stage);
    state.request = stage.request;
    state.translationContext = stage.translationContext;
    if (stage.fault != NoFault) {
        state.translationFault = true;
        return true;
    }
    return applyFinishedTranslation(state.translationContext);
}


void
MlsUnit::deriveStage0Shape(const DynInstPtr &inst, StageState &state) const
{
    const auto &info = inst->matrixInstInfo();
    state.mode = info.loadLike ? BaseMMU::Read : BaseMMU::Write;
    state.accessSize = matrixMemAccessSizeBytes(inst);

    setStateValueForOperand(state, info.tile0State, state.tile0);
    setStateValueForOperand(state, info.tile1State, state.tile1);

    if (state.fault == NoFault) {
        state.fault = matrixMemEarlyFault(inst, state);
    }
    if (state.fault != NoFault) {
        inst->getFault() = state.fault;
    }

}


void
MlsUnit::captureStage0(const DynInstPtr &inst, StageState &state) const
{
    state.vaddr = inst->getRegOperand(inst->staticInst.get(), 0);
    state.stride = inst->getRegOperand(inst->staticInst.get(), 1);
    state.tile0 = inst->getRegOperand(inst->staticInst.get(), 2);
    state.tile1 = inst->getRegOperand(inst->staticInst.get(), 3);
    state.asid =
        RiscvISA::SATP(inst->readMiscReg(RiscvISA::MISCREG_SATP)).asid;
    deriveStage0Shape(inst, state);
}

void
MlsUnit::restoreStage0FromReplay(
    const DynInstPtr &inst,
    const MlsReplayQueue::ReplayState &replay_state,
    StageState &state) const
{
    state.vaddr = replay_state.vaddr;
    state.paddr = replay_state.paddr;
    state.stride = replay_state.stride;
    state.tile0 = replay_state.tile0;
    state.tile1 = replay_state.tile1;
    state.mode = replay_state.mode;
    state.asid = replay_state.asid;
    state.request = replay_state.request;
    state.translationContext = replay_state.translationContext;
    state.replayCause = replay_state.cause;
    deriveStage0Shape(inst, state);

}

void
MlsUnit::runStage1(const DynInstPtr &inst, StageState &state) const
{
    if (state.fault == NoFault) {
        inst->effAddr = state.vaddr;
        inst->effSize = state.accessSize;
        inst->effAddrValid(true);
        inst->translationStarted(true);

        if (!state.request) {
            state.request = std::make_shared<Request>(
                state.vaddr, state.accessSize, Request::Flags{},
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
            ctx.pageRequests.push_back(state.request);

            // Enumerate only pages containing tensor bytes. The bounded
            // shape contract limits this state to at most 256 pages; one
            // timing translation is active per instruction.
            matrix::AmuLsuDesc desc;
            desc.baseAddr = desc.physBaseAddr = state.vaddr;
            desc.stride = state.stride;
            desc.row = state.tile0;
            desc.column = state.tile1;
            desc.transpose = inst->matrixInstInfo().lsuTranspose;
            desc.elemType = toMatrixElemType(
                inst->matrixInstInfo().lsuElemKind);
            std::map<Addr, Addr> pages;
            matrix::timing_load_detail::forEachTensorByte(
                desc, {}, [&](Addr line, size_t offset, size_t, size_t) {
                    const Addr addr = line + offset;
                    pages.emplace(addr & ~Addr(4095), addr);
                });
            const Addr first_page = state.vaddr & ~Addr(4095);
            for (const auto &[page, addr] : pages) {
                if (page == first_page) {
                    continue;
                }
                auto request = std::make_shared<Request>(
                    addr, 1, Request::Flags{}, cpu->dataRequestorId(),
                    inst->pcState().instAddr(), inst->contextId());
                request->taskId(cpu->taskId());
                request->setReqInstSeqNum(inst->seqNum);
                ctx.pageRequests.push_back(std::move(request));
            }
            cpu->mmu->translateTiming(
                state.request, inst->tcBase(),
                new MlsTranslation(cpu, inst, state.translationContext),
                state.mode);
        }

        state.fault = state.translationContext->finished ?
            state.translationContext->fault : NoFault;
        inst->translationCompleted(state.translationContext->finished);
        inst->translatedTick = curTick();
        inst->getFault() = state.fault;

        if (state.fault == NoFault && state.translationContext->finished) {
            state.paddr = state.request->getPaddr();
            inst->physEffAddr = state.paddr;
            inst->memReqFlags = state.request->getFlags();
        } else if (state.fault != NoFault && state.translationContext) {
            state.translationContext->finished = true;
            state.translationContext->fault = state.fault;
        }
    }

}

void
MlsUnit::runStage3(const DynInstPtr &inst, StageState &state) const
{
    if (state.needReplay) {
        return;
    }

    if (state.fault != NoFault) {
        return;
    }

    const auto &info = inst->matrixInstInfo();
    auto &payload = state.payload;
    payload = {};
    payload.valid = true;
    payload.kind = ExecContext::MatrixExecPayload::Kind::Lsu;
    payload.isLoad = info.loadLike;
    payload.isStore = info.storeLike;
    payload.isA = info.lsuIsA;
    payload.isB = info.lsuIsB;
    payload.isAcc = info.lsuIsAcc;
    payload.transpose = info.lsuTranspose;
    payload.op = info.funct7;
    payload.ms = info.rd & 0x7;
    payload.baseAddr = state.vaddr;
    payload.physBaseAddr = state.paddr;
    payload.pageMappings = state.translationContext->pageMappings;
    payload.stride = state.stride;
    payload.row = state.tile0;
    payload.column = state.tile1;
    payload.mtilem = state.mtilem;
    payload.mtilen = state.mtilen;
    payload.mtilek = state.mtilek;
    payload.widths = info.lsuWidths;
    payload.elemType = toMatrixElemType(info.lsuElemKind);

    inst->stageMatrixExecPayload(payload);
}

MlsReplayQueue::ReplayState
MlsUnit::buildReplayState(const StageState &state) const
{
    MlsReplayQueue::ReplayState replay_state;
    replay_state.vaddr = state.vaddr;
    replay_state.paddr = state.paddr;
    replay_state.stride = state.stride;
    replay_state.tile0 = state.tile0;
    replay_state.tile1 = state.tile1;
    replay_state.mode = state.mode;
    replay_state.accessSize = state.accessSize;
    replay_state.request = state.request;
    replay_state.translationContext = state.translationContext;
    replay_state.asid = state.asid;
    replay_state.cause = state.replayCause;
    replay_state.translationComplete =
        state.fault == NoFault && state.request && state.request->hasPaddr() &&
        state.translationContext && state.translationContext->finished;
    replay_state.translationFault = state.fault != NoFault;
    if (state.request) {
        replay_state.requestFlags = state.request->getFlags();
    }
    return replay_state;
}

MlsUnit::MlsUnit(CPU *cpu_) : cpu(cpu_)
{
    assert(cpu);
}

bool
MlsUnit::replayReady(
    const DynInstPtr &inst, MlsReplayQueue::ReplayState &state) const
{
    switch (state.cause) {
      case MlsReplayQueue::ReplayCause::TranslationResultPending:
        return ensureReplayReady(inst, state);
      case MlsReplayQueue::ReplayCause::None:
        return false;
    }

    return false;
}

MlsUnit::IssueResult
MlsUnit::issue(const DynInstPtr &inst)
{
    IssueResult result;
    assert(inst);
    assert(inst->isMatrixInst());
    assert(inst->opClass() == MatrixMemOp);

    StageState state;
    state.fault = inst->getFault();
    if (replayQueue) {
        if (const auto *replay_state = replayQueue->getState(inst)) {
            restoreStage0FromReplay(inst, *replay_state, state);
        } else {
            captureStage0(inst, state);
        }
    } else {
        captureStage0(inst, state);
    }
    if (state.fault == NoFault) {
        probeTlbState(state);
    }
    runStage1(inst, state);
    auto replayState = buildReplayState(state);

    const bool translationSucceeded =
        state.fault == NoFault && replayState.translationComplete;
    if (state.fault == NoFault && !translationSucceeded) {
        state.replayCause =
            MlsReplayQueue::ReplayCause::TranslationResultPending;
        replayState.cause = state.replayCause;
        state.replayReady =
            replayState.translationComplete || replayState.translationFault;
        state.needReplay = true;
    }

    runStage3(inst, state);

    if (state.needReplay && replayQueue) {
        const bool queued = replayQueue->allocateOrUpdate(
            inst, replayState, state.replayReady);
        panic_if(!queued,
                 "Matrix replay queue full [tid:%i] [sn:%llu]",
                 inst->threadNumber, inst->seqNum);
        result.needReplay = true;
    }

    if (!state.needReplay && virtualQueue) {
        const bool marked = virtualQueue->markFinished(inst);
        panic_if(!marked,
                 "Matrix virtual queue entry missing at finish [tid:%i] [sn:%llu]",
                 inst->threadNumber, inst->seqNum);
    }

    if (state.fault == NoFault && !inst->readPredicate()) {
        inst->forwardOldRegs();
    }

    if (replayQueue && replayQueue->hasEntry(inst) && !result.needReplay) {
        const bool completed = replayQueue->completeRetry(inst);
        panic_if(!completed,
                 "Matrix replay entry missing at complete [tid:%i] [sn:%llu]",
                 inst->threadNumber, inst->seqNum);
    }

    return result;
}


} // namespace o3
} // namespace gem5

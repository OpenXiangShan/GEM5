#include "cpu/o3/mls_unit.hh"

#include <algorithm>
#include <cassert>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/IEW.hh"
#include "sim/cur_tick.hh"

#if THE_ISA_IS_RISCV
#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/tlb.hh"
#include "cpu/exec_context.hh"
#include "cpu/op_class.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"
#endif

namespace gem5
{

namespace o3
{

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
    entry.tid = tid;
    entry.slot = slot;
    entry.allocated = true;
    queues[tid].push_back(slot);
    nextSlots[tid] = (slot + 1) % queueCapacity;
    inst->setMatrixMlsqSlot(slot);

    DPRINTF(IEW,
            "MlsVirtualQueue alloc [tid:%i] [sn:%llu] slot=%u robOrder=%llu "
            "size=%u free=%u.\n",
            tid, inst->seqNum, entry.slot, entry.robSeqNum,
            static_cast<unsigned>(queues[tid].size()), freeEntries(tid));

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
    DPRINTF(IEW,
            "MlsVirtualQueue finish [tid:%i] [sn:%llu] slot=%u robOrder=%llu.\n",
            entry->tid, inst->seqNum, entry->slot, entry->robSeqNum);
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

        DPRINTF(IEW,
                "MlsVirtualQueue free [tid:%i] [sn:%llu] slot=%u "
                "robOrder=%llu committed=%llu.\n",
                tid, head.robSeqNum, head.slot, head.robSeqNum, committed_seq);
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

        DPRINTF(IEW,
                "MlsVirtualQueue cancel [tid:%i] [sn:%llu] slot=%u "
                "robOrder=%llu squash=%llu.\n",
                tid, tail.robSeqNum, tail.slot, tail.robSeqNum, squash_seq);
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
    const ThreadID tid = inst->threadNumber;
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    if (auto *entry = findEntryByInst(inst)) {
        entry->scheduled = false;
        entry->ready = ready;
        entry->availableTick = ready ? curTick() + replaySelectLatency : 0;
        entry->state = state;
        DPRINTF(IEW,
                "MlsReplayQueue retry-arm [tid:%i] [sn:%llu] slot=%u "
                "robOrder=%llu ready=%d vaddr=%#llx.\n",
                tid, inst->seqNum, entry->slot, entry->robSeqNum,
                ready, state.vaddr);
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
    entry.tid = tid;
    entry.slot = *slot;
    entry.inst = inst;
    entry.state = state;
    inst->setMatrixMlsReplaySlot(*slot);

    DPRINTF(IEW,
            "MlsReplayQueue alloc [tid:%i] [sn:%llu] slot=%u robOrder=%llu "
            "ready=%d vaddr=%#llx stride=%#llx tile0=%#llx tile1=%#llx.\n",
            tid, inst->seqNum, entry.slot, entry.robSeqNum,
            ready, state.vaddr, state.stride, state.tile0, state.tile1);
    return true;
}

void
MlsReplayQueue::refreshReady(
    ThreadID tid, const std::function<bool(const ReplayState &)> &ready_fn)
{
    panic_if(tid >= entries.size(), "Invalid thread id %u for MlsReplayQueue", tid);

    for (auto &entry : entries[tid]) {
        if (!entry.allocated || entry.scheduled) {
            continue;
        }

        const bool was_ready = entry.ready;
        entry.ready = ready_fn(entry.state);
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

    DPRINTF(IEW,
            "MlsReplayQueue schedule [tid:%i] [sn:%llu] slot=%u robOrder=%llu.\n",
            tid, selected->robSeqNum, selected->slot, selected->robSeqNum);
    return true;
}

void
MlsReplayQueue::freeEntry(Entry &entry)
{
    if (entry.inst) {
        entry.inst->clearMatrixMlsReplaySlot();
    }

    DPRINTF(IEW,
            "MlsReplayQueue free [tid:%i] [sn:%llu] slot=%u robOrder=%llu.\n",
            entry.tid, entry.robSeqNum, entry.slot, entry.robSeqNum);
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

        DPRINTF(IEW,
                "MlsReplayQueue cancel [tid:%i] [sn:%llu] slot=%u robOrder=%llu squash=%llu.\n",
                tid, entry.robSeqNum, entry.slot, entry.robSeqNum, squash_seq);
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

#if THE_ISA_IS_RISCV

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
    Fault fault = NoFault;
    uint16_t asid = 0;
    bool tlbMiss = false;
    bool replayReady = false;
    bool needReplay = false;
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

    const bool rd_bit2_set = (info.rd & 0x4) != 0;
    if (rd_bit2_set != info.rdMustSetBit2) {
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
MlsUnit::replayTlbReady(const MlsReplayQueue::ReplayState &state) const
{
    auto *mmu = dynamic_cast<RiscvISA::MMU *>(cpu->mmu);
    panic_if(!mmu, "Matrix MLS replay requires RISC-V MMU");
    auto *dtb = static_cast<RiscvISA::TLB *>(mmu->dtb);
    auto *entry = dtb->lookup(
        state.vaddr, state.asid, state.mode, true, false, RiscvISA::direct);
    return entry != nullptr;
}

bool
MlsUnit::ensureReplayReady(const MlsReplayQueue::ReplayState &state) const
{
    if (replayTlbReady(state)) {
        return true;
    }

    if (!FullSystem) {
        auto *mmu = dynamic_cast<RiscvISA::MMU *>(cpu->mmu);
        panic_if(!mmu, "Matrix MLS replay requires RISC-V MMU");
        auto *dtb = static_cast<RiscvISA::TLB *>(mmu->dtb);

        RiscvISA::TlbEntry entry;
        entry.vaddr = state.vaddr;
        entry.paddr = state.paddr >> RiscvISA::PGSHFT;
        entry.logBytes = RiscvISA::PGSHFT;
        entry.translateMode = RiscvISA::direct;
        entry.asid = state.asid;
        entry.pte = 0;
        entry.pte.ppn = state.paddr >> RiscvISA::PGSHFT;
        entry.pte.v = 1;
        entry.pte.r = 1;
        entry.pte.w = 1;
        entry.pte.a = 1;
        entry.pte.d = 1;
        dtb->insert(state.vaddr, entry, false, RiscvISA::direct);
    }

    return replayTlbReady(state);
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

    DPRINTF(IEW,
            "MlsUnit S0 capture [tid:%i] [sn:%llu] vaddr=%#llx stride=%#llx "
            "tile0=%#llx tile1=%#llx mtilem=%#llx mtilen=%#llx mtilek=%#llx "
            "mode=%s accessSize=%u fault=%d.\n",
            inst->threadNumber, inst->seqNum, state.vaddr, state.stride,
            state.tile0, state.tile1, state.mtilem, state.mtilen,
            state.mtilek, state.mode == BaseMMU::Read ? "read" : "write",
            state.accessSize, state.fault != NoFault);
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
    deriveStage0Shape(inst, state);

    DPRINTF(IEW,
            "MlsUnit S0 replay-restore [tid:%i] [sn:%llu] vaddr=%#llx "
            "stride=%#llx tile0=%#llx tile1=%#llx.\n",
            inst->threadNumber, inst->seqNum, state.vaddr,
            state.stride, state.tile0, state.tile1);
}

void
MlsUnit::runStage1(const DynInstPtr &inst, StageState &state) const
{
    if (state.fault == NoFault) {
        state.request = std::make_shared<Request>(
            state.vaddr, state.accessSize, Request::Flags{},
            cpu->dataRequestorId(), inst->pcState().instAddr(),
            inst->contextId());
        state.request->taskId(cpu->taskId());
        state.request->setReqInstSeqNum(inst->seqNum);

        inst->effAddr = state.vaddr;
        inst->effSize = state.accessSize;
        inst->effAddrValid(true);
        inst->translationStarted(true);

        state.fault =
            cpu->mmu->translateAtomic(state.request, inst->tcBase(), state.mode);

        inst->translationCompleted(true);
        inst->translatedTick = curTick();
        inst->getFault() = state.fault;

        if (state.fault == NoFault) {
            state.paddr = state.request->getPaddr();
            inst->physEffAddr = state.paddr;
            inst->memReqFlags = state.request->getFlags();
        }
    }

    DPRINTF(IEW,
            "MlsUnit S1 translate [tid:%i] [sn:%llu] mode=%s vaddr=%#llx "
            "paddr=%#llx size=%u fault=%d tlbMiss=%d.\n",
            inst->threadNumber, inst->seqNum,
            state.mode == BaseMMU::Read ? "ld" : "st",
            state.vaddr, state.paddr, state.accessSize,
            state.fault != NoFault, state.tlbMiss);
}

void
MlsUnit::runStage2(const DynInstPtr &inst, StageState &state) const
{
    DPRINTF(IEW,
            "MlsUnit S2 resolve [tid:%i] [sn:%llu] paddr=%#llx "
            "fault=%d predicate=%d.\n",
            inst->threadNumber, inst->seqNum, state.paddr,
            state.fault != NoFault,
            inst->readPredicate());
}

void
MlsUnit::runStage3(const DynInstPtr &inst, StageState &state) const
{
    if (state.needReplay) {
        DPRINTF(IEW,
                "MlsUnit S3 replay request [tid:%i] [sn:%llu] cause=tlb-miss "
                "vaddr=%#llx stride=%#llx tile0=%#llx tile1=%#llx.\n",
                inst->threadNumber, inst->seqNum, state.vaddr,
                state.stride, state.tile0, state.tile1);
        return;
    }

    if (state.fault != NoFault) {
        DPRINTF(IEW,
                "MlsUnit S3 payload skipped [tid:%i] [sn:%llu] fault=%d.\n",
                inst->threadNumber, inst->seqNum, state.fault != NoFault);
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
    payload.stride = state.stride;
    payload.row = state.tile0;
    payload.column = state.tile1;
    payload.mtilem = state.mtilem;
    payload.mtilen = state.mtilen;
    payload.mtilek = state.mtilek;
    payload.widths = info.lsuWidths;
    payload.elemType = toMatrixElemType(info.lsuElemKind);

    inst->stageMatrixExecPayload(payload);

    DPRINTF(IEW,
            "MlsUnit S3 payload [tid:%i] [sn:%llu] load=%d store=%d "
            "isAcc=%d isA=%d isB=%d op=%#llx ms=%#llx base=%#llx "
            "stride=%#llx row=%#llx column=%#llx widths=%#llx.\n",
            inst->threadNumber, inst->seqNum,
            payload.isLoad, payload.isStore, payload.isAcc,
            payload.isA, payload.isB, payload.op, payload.ms,
            payload.baseAddr, payload.stride, payload.row,
            payload.column, payload.widths);
}

void
MlsUnit::runStage4(const DynInstPtr &inst, const StageState &state) const
{
    DPRINTF(IEW,
            "MlsUnit S4 handoff [tid:%i] [sn:%llu] payloadValid=%d "
            "payload=%s fault=%d needReplay=%d.\n",
            inst->threadNumber, inst->seqNum, state.payload.valid,
            inst->matrixPayloadKindName(), state.fault != NoFault,
            state.needReplay);
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
    replay_state.asid = state.asid;
    return replay_state;
}

MlsUnit::MlsUnit(CPU *cpu_) : cpu(cpu_)
{
    assert(cpu);
}

bool
MlsUnit::replayReady(const MlsReplayQueue::ReplayState &state) const
{
    return replayTlbReady(state);
}

MlsUnit::IssueResult
MlsUnit::issue(const DynInstPtr &inst)
{
    IssueResult result;
    assert(inst);
    assert(inst->isMatrixInst());
    assert(inst->opClass() == MatrixMemOp);

    const auto &info = inst->matrixInstInfo();
    DPRINTF(IEW,
            "MlsUnit issue [tid:%i] [sn:%llu] class=%s route=%s "
            "boundary=%s funct7=%#x rd=x%u rs1=x%u rs2=x%u "
            "loadLike=%d storeLike=%d needAmu=%d.\n",
            inst->threadNumber, inst->seqNum, inst->matrixInstClassName(),
            inst->matrixRouteName(), inst->matrixCommitBoundaryName(),
            info.funct7, info.rd, info.rs1, info.rs2,
            info.loadLike, info.storeLike, inst->matrixNeedAmuCtrl());

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

    const bool directPhysTranslation =
        state.fault == NoFault && state.request &&
        (state.request->getFlags() & Request::PHYSICAL);

    if (state.fault == NoFault && state.tlbMiss && !directPhysTranslation) {
        state.replayReady = ensureReplayReady(replayState);
        state.needReplay = true;
    } else if (state.fault == NoFault && state.tlbMiss &&
               directPhysTranslation) {
        DPRINTF(IEW,
                "MlsUnit bypass replay on physical translation [tid:%i] "
                "[sn:%llu] vaddr=%#llx paddr=%#llx flags=%#llx.\n",
                inst->threadNumber, inst->seqNum, state.vaddr, state.paddr,
                static_cast<unsigned long long>(static_cast<Request::FlagsType>(
                    state.request->getFlags())));
    }

    if (state.fault == NoFault && !state.needReplay) {
        runStage2(inst, state);
    } else {
        DPRINTF(IEW,
                "MlsUnit S2 resolve skipped [tid:%i] [sn:%llu] "
                "prefault=%d fault=%d needReplay=%d.\n",
                inst->threadNumber, inst->seqNum,
                inst->getFault() != NoFault, state.fault != NoFault,
                state.needReplay);
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

    if (!state.needReplay) {
        runStage4(inst, state);
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

#else

MlsUnit::MlsUnit(CPU *cpu_) : cpu(cpu_)
{
}

bool
MlsUnit::replayReady(const MlsReplayQueue::ReplayState &state) const
{
    panic("Matrix MLS replay is only supported by the RISC-V ISA");
}

MlsUnit::IssueResult
MlsUnit::issue(const DynInstPtr &inst)
{
    panic("Matrix MLS execution is only supported by the RISC-V ISA");
}

#endif

} // namespace o3
} // namespace gem5

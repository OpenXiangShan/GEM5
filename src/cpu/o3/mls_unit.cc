#include "cpu/o3/mls_unit.hh"

#include <cassert>
#include <cstdint>

#include "base/logging.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
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
    if (!inst || inst->threadNumber >= queues.size() ||
        !inst->hasMatrixMlsqSlot()) {
        return nullptr;
    }

    auto *entry =
        findEntryBySlot(inst->threadNumber, inst->getMatrixMlsqSlot());
    if (entry && entry->robSeqNum == inst->seqNum) {
        return entry;
    }

    return nullptr;
}

const MlsVirtualQueue::Entry *
MlsVirtualQueue::findEntryByInst(const DynInstPtr &inst) const
{
    if (!inst || inst->threadNumber >= queues.size() ||
        !inst->hasMatrixMlsqSlot()) {
        return nullptr;
    }

    auto *entry =
        findEntryBySlot(inst->threadNumber, inst->getMatrixMlsqSlot());
    if (entry && entry->robSeqNum == inst->seqNum) {
        return entry;
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

namespace
{

constexpr uint8_t MatrixFunct7A = 0x02;
constexpr uint8_t MatrixFunct7B = 0x0a;
constexpr uint8_t MatrixFunct7CLoad = 0x12;
constexpr uint8_t MatrixFunct7CStore = 0x13;

uint8_t
matrixMd(uint8_t rd)
{
    return RiscvISA::ISA::matrixMd(rd);
}

uint8_t
matrixMemWidth(uint8_t rd)
{
    return RiscvISA::ISA::matrixMemWidth(rd);
}

bool
isMatrixAClass(uint8_t funct7)
{
    return funct7 == MatrixFunct7A;
}

bool
isMatrixBClass(uint8_t funct7)
{
    return funct7 == MatrixFunct7B;
}

bool
isMatrixCClass(uint8_t funct7)
{
    return funct7 == MatrixFunct7CLoad || funct7 == MatrixFunct7CStore;
}

bool
isSupportedAbWidth(uint8_t width)
{
    return RiscvISA::ISA::matrixAbWidthSupported(width);
}

unsigned
matrixAccessSize(uint8_t width)
{
    switch (width) {
      case RiscvISA::ISA::MatrixSewE8:
        return 1;
      case RiscvISA::ISA::MatrixSewE16:
        return 2;
      case RiscvISA::ISA::MatrixSewE32:
        return 4;
      default:
        return 1;
    }
}

matrix::MatrixElemType
matrixElemType(uint8_t width)
{
    switch (width) {
      case RiscvISA::ISA::MatrixSewE8:
        return matrix::MatrixElemType::Int8;
      case RiscvISA::ISA::MatrixSewE16:
        return matrix::MatrixElemType::Fp16;
      case RiscvISA::ISA::MatrixSewE32:
        return matrix::MatrixElemType::Int32;
      default:
        panic("Unsupported matrix MLS width %#x at payload build", width);
    }
}

RiscvISA::TLB *
matrixDataTlb(CPU *cpu)
{
    auto *mmu = dynamic_cast<RiscvISA::MMU *>(cpu->mmu);
    panic_if(!mmu, "Matrix MLS replay requires RISC-V MMU");
    return static_cast<RiscvISA::TLB *>(mmu->dtb);
}

} // anonymous namespace

unsigned
MlsUnit::matrixMemAccessSizeBytes(const DynInstPtr &inst) const
{
    return matrixAccessSize(matrixMemWidth(inst->matrixInstInfo().rd));
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

    const auto md = matrixMd(info.rd);
    const auto width = matrixMemWidth(info.rd);
    switch (info.funct7) {
      case MatrixFunct7A:
        if (md >= 4 || !isSupportedAbWidth(width) ||
            state.mtilem > RiscvISA::ISA::MatrixRowNum ||
            state.mtilek > RiscvISA::ISA::MatrixTrLenE8Max) {
            return illegal("mlae parameter check failed");
        }
        if (width == RiscvISA::ISA::MatrixSewE16 &&
            state.mtilek > RiscvISA::ISA::MatrixTrLenE16Max) {
            return illegal("mlae parameter check failed");
        }
        break;
      case MatrixFunct7B:
        if (md >= 4 || !isSupportedAbWidth(width) ||
            state.mtilen > RiscvISA::ISA::MatrixRowNum ||
            state.mtilek > RiscvISA::ISA::MatrixTrLenE8Max) {
            return illegal("mlbe parameter check failed");
        }
        if (width == RiscvISA::ISA::MatrixSewE16 &&
            state.mtilek > RiscvISA::ISA::MatrixTrLenE16Max) {
            return illegal("mlbe parameter check failed");
        }
        break;
      case MatrixFunct7CLoad:
        if (md < 4 || width != RiscvISA::ISA::MatrixSewE32 ||
            state.mtilem > RiscvISA::ISA::MatrixRowNum ||
            state.mtilen > RiscvISA::ISA::MatrixAccE32Max) {
            return illegal("mlce32 parameter check failed");
        }
        break;
      case MatrixFunct7CStore:
        if (md < 4 || width != RiscvISA::ISA::MatrixSewE32 ||
            state.mtilem > RiscvISA::ISA::MatrixRowNum ||
            state.mtilen > RiscvISA::ISA::MatrixAccE32Max) {
            return illegal("msce32 parameter check failed");
        }
        break;
      default:
        break;
    }

    return NoFault;
}

void
MlsUnit::probeTlbState(StageState &state) const
{
    auto *entry = matrixDataTlb(cpu)->lookup(
        state.vaddr, state.asid, state.mode, true, false, RiscvISA::direct);
    state.tlbMiss = (entry == nullptr);
}

bool
MlsUnit::replayTlbReady(const MlsReplayQueue::ReplayState &state) const
{
    auto *entry = matrixDataTlb(cpu)->lookup(
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
        matrixDataTlb(cpu)->insert(state.vaddr, entry, false,
                                   RiscvISA::direct);
    }

    return replayTlbReady(state);
}

void
MlsUnit::deriveStage0Shape(const DynInstPtr &inst, StageState &state) const
{
    const auto &info = inst->matrixInstInfo();
    state.mode = info.loadLike ? BaseMMU::Read : BaseMMU::Write;
    state.accessSize = matrixMemAccessSizeBytes(inst);

    switch (info.funct7) {
      case MatrixFunct7A:
        state.mtilem = state.tile0;
        state.mtilek = state.tile1;
        break;
      case MatrixFunct7B:
        state.mtilek = state.tile0;
        state.mtilen = state.tile1;
        break;
      case MatrixFunct7CLoad:
      case MatrixFunct7CStore:
        state.mtilem = state.tile0;
        state.mtilen = state.tile1;
        break;
      default:
        break;
    }

    if (state.fault == NoFault) {
        state.fault = matrixMemEarlyFault(inst, state);
    }
    if (state.fault != NoFault) {
        inst->getFault() = state.fault;
    }

    DPRINTF(IEW,
            "MlsUnit S0 capture [tid:%i] [sn:%llu] vaddr=%#llx stride=%#llx "
            "tile0=%#llx tile1=%#llx mtilem=%#llx mtilen=%#llx mtilek=%#llx "
            "size=%u prefault=%d.\n",
            inst->threadNumber, inst->seqNum,
            state.vaddr, state.stride, state.tile0, state.tile1,
            state.mtilem, state.mtilen, state.mtilek,
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

    auto &payload = state.payload;
    payload = {};
    payload.valid = true;
    payload.kind = ExecContext::MatrixExecPayload::Kind::Lsu;
    payload.isLoad = inst->matrixInstInfo().loadLike;
    payload.isStore = inst->matrixInstInfo().storeLike;
    payload.isA = isMatrixAClass(inst->matrixInstInfo().funct7);
    payload.isB = isMatrixBClass(inst->matrixInstInfo().funct7);
    payload.isAcc = isMatrixCClass(inst->matrixInstInfo().funct7);
    payload.transpose = isMatrixBClass(inst->matrixInstInfo().funct7);
    payload.op = inst->matrixInstInfo().funct7;
    payload.ms = matrixMd(inst->matrixInstInfo().rd);
    payload.baseAddr = state.vaddr;
    payload.physBaseAddr = state.paddr;
    payload.stride = state.stride;
    payload.row = state.tile0;
    payload.column = state.tile1;
    payload.mtilem = state.mtilem;
    payload.mtilen = state.mtilen;
    payload.mtilek = state.mtilek;
    payload.widths = matrixMemWidth(inst->matrixInstInfo().rd);
    payload.elemType = matrixElemType(payload.widths);

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

    if (state.fault == NoFault && state.tlbMiss) {
        state.replayReady = ensureReplayReady(replayState);
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

#else // THE_ISA_IS_RISCV

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

#endif // THE_ISA_IS_RISCV

} // namespace o3
} // namespace gem5

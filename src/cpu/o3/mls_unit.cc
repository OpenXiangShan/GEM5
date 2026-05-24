#include "cpu/o3/mls_unit.hh"

#include <cassert>

#include "arch/generic/mmu.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/tlb.hh"
#include "cpu/exec_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/mls_replay_queue.hh"
#include "cpu/o3/mls_virtual_queue.hh"
#include "cpu/op_class.hh"
#include "debug/IEW.hh"
#include "mem/request.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace o3
{

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

unsigned
MlsUnit::matrixMemAccessSizeBytes(const DynInstPtr &inst) const
{
    switch (inst->matrixInstInfo().funct7) {
      case 0x02:
      case 0x0a:
        return 1;
      case 0x12:
      case 0x1a:
        return 2;
      case 0x22:
      case 0x13:
        return 4;
      default:
        return 1;
    }
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

    switch (info.funct7) {
      case 0x02:
        if ((info.rd & 0x4) != 0 || state.mtilem > RiscvISA::ISA::MatrixRowNum ||
            state.mtilek > RiscvISA::ISA::MatrixTrLenE8Max) {
            return illegal("mlae8 parameter check failed");
        }
        break;
      case 0x0a:
        if ((info.rd & 0x4) != 0 || state.mtilen > RiscvISA::ISA::MatrixRowNum ||
            state.mtilek > RiscvISA::ISA::MatrixTrLenE8Max) {
            return illegal("mlbe8 parameter check failed");
        }
        break;
      case 0x12:
        if ((info.rd & 0x4) != 0 || state.mtilem > RiscvISA::ISA::MatrixRowNum ||
            state.mtilek > RiscvISA::ISA::MatrixTrLenE16Max) {
            return illegal("mlae16 parameter check failed");
        }
        break;
      case 0x1a:
        if ((info.rd & 0x4) != 0 || state.mtilen > RiscvISA::ISA::MatrixRowNum ||
            state.mtilek > RiscvISA::ISA::MatrixTrLenE16Max) {
            return illegal("mlbe16 parameter check failed");
        }
        break;
      case 0x22:
        if ((info.rd & 0x4) == 0 || state.mtilem > RiscvISA::ISA::MatrixRowNum ||
            state.mtilen > RiscvISA::ISA::MatrixAccE32Max) {
            return illegal("mlce32 parameter check failed");
        }
        break;
      case 0x13:
        if ((info.rd & 0x4) == 0 || state.mtilem > RiscvISA::ISA::MatrixRowNum ||
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

    switch (info.funct7) {
      case 0x02:
        state.mtilem = state.tile0;
        state.mtilek = state.tile1;
        break;
      case 0x0a:
        state.mtilek = state.tile0;
        state.mtilen = state.tile1;
        break;
      case 0x12:
        state.mtilem = state.tile0;
        state.mtilek = state.tile1;
        break;
      case 0x1a:
        state.mtilek = state.tile0;
        state.mtilen = state.tile1;
        break;
      case 0x22:
      case 0x13:
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

    auto &payload = state.payload;
    payload = {};
    payload.valid = true;
    payload.kind = ExecContext::MatrixExecPayload::Kind::Lsu;
    payload.isLoad = inst->matrixInstInfo().loadLike;
    payload.isStore = inst->matrixInstInfo().storeLike;
    payload.isA = inst->matrixInstInfo().funct7 == 0x02 ||
                  inst->matrixInstInfo().funct7 == 0x12;
    payload.isB = inst->matrixInstInfo().funct7 == 0x0a ||
                  inst->matrixInstInfo().funct7 == 0x1a;
    payload.isAcc = inst->matrixInstInfo().funct7 == 0x22 ||
                    inst->matrixInstInfo().funct7 == 0x13;
    payload.transpose = inst->matrixInstInfo().funct7 == 0x0a ||
                        inst->matrixInstInfo().funct7 == 0x1a;
    payload.op = inst->matrixInstInfo().funct7;
    payload.ms = inst->matrixInstInfo().rd & 0x7;
    payload.baseAddr = state.vaddr;
    payload.physBaseAddr = state.paddr;
    payload.stride = state.stride;
    payload.row = state.tile0;
    payload.column = state.tile1;
    payload.mtilem = state.mtilem;
    payload.mtilen = state.mtilen;
    payload.mtilek = state.mtilek;
    switch (inst->matrixInstInfo().funct7) {
      case 0x02:
      case 0x0a:
        payload.widths = RiscvISA::ISA::MatrixSewE8;
        payload.elemType = matrix::MatrixElemType::Int8;
        break;
      case 0x12:
      case 0x1a:
        payload.widths = RiscvISA::ISA::MatrixSewE16;
        payload.elemType = matrix::MatrixElemType::Fp16;
        break;
      case 0x22:
      case 0x13:
        payload.widths = RiscvISA::ISA::MatrixSewE32;
        payload.elemType = matrix::MatrixElemType::Int32;
        break;
      default:
        panic("Unsupported matrix MLS funct7 %#x at payload build",
              inst->matrixInstInfo().funct7);
    }

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

    if (state.fault == NoFault && state.tlbMiss) {
        state.replayReady = ensureReplayReady(replayState);
        state.needReplay = true;
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

} // namespace o3
} // namespace gem5

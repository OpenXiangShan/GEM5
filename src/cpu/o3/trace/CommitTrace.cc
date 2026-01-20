#include <algorithm>
#include <memory>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/logging.hh"
#include "config/the_isa.hh"
#include "cpu/o3/commit.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/trace/TraceInstruction.hh"
#include "cpu/thread_context.hh"
#include "debug/CommitTrace.hh"
#include "sim/faults.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

namespace o3
{

namespace {

class TraceCtrlFlowFault : public FaultBase
{
  public:
    explicit TraceCtrlFlowFault(Addr target) : targetPC(target) {}

    FaultName name() const override { return "TraceCtrlFlowFault"; }
    bool isFromISA() const override { return false; }

    void invoke(::gem5::ThreadContext *tc,
                const StaticInstPtr &inst = nullStaticInstPtr) override
    {
        auto pc_state = tc->pcState().as<TheISA::PCState>();
        pc_state.set(targetPC);
        tc->pcState(pc_state);
    }

  private:
    Addr targetPC;
};

} // anonymous namespace

bool
Commit::traceMaybeExitOnPipelineDrainFromStuckCheck()
{
    if (cpu->isTraceMode() && cpu->isTracePipelineDrained()) {
        warn("[Commit] Trace mode pipeline drained without further commits; "
             "treating as normal exit instead of CommitStuck panic.\n");
        exitSimLoop("Trace-driven CPU drained (EOF or maxinsts)");
        return true;
    }

    return false;
}

bool
Commit::traceMaybeExitOnEofDrainFromTick()
{
    if (cpu->isTraceMode() && cpu->isTraceEOF() && cpu->isTracePipelineDrained()) {
        warn("[Commit] Trace mode reached EOF and pipeline drained; exiting cleanly.\n");
        exitSimLoop("Trace-driven CPU reached EOF and drained");
        return true;
    }

    return false;
}

void
Commit::traceUpdateSquashInfo(ThreadID tid, InstSeqNum squashed_inst)
{
    InstSeqNum notify_done_seq = squashed_inst;
    if (traceCtrlFaultPending[tid]) {
        notify_done_seq = std::max(notify_done_seq, traceCtrlFaultSeqNum[tid]);
    }

    // For trace ctrl-flow faults, notify fetch rollback to skip the faulting inst.
    toIEW->commitInfo[tid].traceTrapSeqNum = notify_done_seq;
    toIEW->commitInfo[tid].traceTrapSkipInst = traceCtrlFaultPending[tid];

    traceCtrlFaultPending[tid] = false;
}

void
Commit::traceMaybeInjectCtrlFlowChangeFault(ThreadID tid, const DynInstPtr &head_inst)
{
    if (!cpu->isTraceMode() || head_inst->getFault() != NoFault) {
        return;
    }

    if (const o3::TraceInstruction *ti_meta =
            cpu->getTraceInstMetadata(head_inst->seqNum)) {
        if (ti_meta->isCtrlFlowChange()) {
            head_inst->setTraceCtrlFlowChange(true);
            traceCtrlFaultPending[tid] = true;
            traceCtrlFaultSeqNum[tid] = head_inst->seqNum;
            head_inst->getFault() =
                std::make_shared<TraceCtrlFlowFault>(ti_meta->getCtrlFlowTarget());
            DPRINTF(
                CommitTrace,
                "[tid:%d idx:%llu sn:%llu] Trace ctrl-flow change -> inject fault to "
                "target 0x%lx\n",
                tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
                (unsigned long)ti_meta->getCtrlFlowTarget());
        }
    }
}

bool
Commit::traceMaybeExitOnLastTraceInst(const DynInstPtr &head_inst)
{
    if (cpu->isTraceMode() && head_inst->isLastTraceInst()) {
        warn("[Commit] Committed LAST trace-driven instruction [sn:%llu]; exiting cleanly.\n",
             head_inst->seqNum);
        exitSimLoop("Trace-driven CPU committed last traced instruction");
        return true;
    }

    return false;
}

void
Commit::traceOnCommit(ThreadID tid, const DynInstPtr &head_inst)
{
    if (!cpu->isTraceMode()) {
        return;
    }

    traceCommitDifftest(tid, head_inst);

    // In trace mode, release per-instruction trace metadata on commit to prevent
    // unbounded growth of fetch-side metadata structures. The actual cleanup uses
    // a sliding window based on the oldest in-flight seqNum and a guard distance.
    if (cpu->isTraceInstruction(head_inst->seqNum)) {
        cpu->cleanupTraceMetadataOnCommit(head_inst->seqNum);
    }
}

void
Commit::traceOnMacroCommit(ThreadID tid)
{
    if (cpu->isTraceMode()) {
        traceCommitIndex[tid]++;
    }
}

void
Commit::traceLogHandleInterrupt()
{
    DPRINTF(CommitTrace, "Handle interrupt No.%lx\n",
            cpu->getInterruptsNO() | (1ULL << 63));
}

void
Commit::traceLogInstFault(const DynInstPtr &head_inst, const Fault &inst_fault)
{
    DPRINTF(CommitTrace, "[sn:%lu pc:%#lx] %s has a fault: %s\n", head_inst->seqNum,
            head_inst->pcState().instAddr(), head_inst->genDisassembly(),
            inst_fault->name());
}

void
Commit::traceLogPrivReturn(const DynInstPtr &head_inst, ThreadID tid)
{
    DPRINTF(CommitTrace, "Priv return [sn:%llu] PC %s: %s, mepc: %#lx, sepc: %#lx\n",
            head_inst->seqNum, head_inst->pcState(),
            head_inst->staticInst->disassemble(head_inst->pcState().instAddr()),
            cpu->readMiscRegNoEffect(RiscvISA::MISCREG_MEPC, tid),
            cpu->readMiscRegNoEffect(RiscvISA::MISCREG_SEPC, tid));
}

void
Commit::traceLogCommitBlockedCycles(const DynInstPtr &head_inst, uint64_t delta)
{
    DPRINTF(CommitTrace, "Inst[sn:%lu] commit blocked cycles: %lu\n",
            head_inst->seqNum, delta);
}

// Perform trace-mode commit difftest and logging (PC, InstType, memory ops, branch info)
void
Commit::traceCommitDifftest(ThreadID tid, const DynInstPtr &head_inst)
{
    // Initialize/align expected trace index from fetch mapping on first use
    uint64_t mapped_idx = cpu->getTraceIndexForSeqNum(head_inst->seqNum);
    if (traceCommitIndex[tid] == 0 && mapped_idx != 0) {
        traceCommitIndex[tid] = mapped_idx;
        DPRINTF(CommitTrace, "[tid:%d] Init traceCommitIndex -> %llu (sn:%llu)\n",
                tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum);
    }

    // 1) Resolve expected trace PC
    // Prefer O(1) metadata lookup (seqNum -> TraceInstruction) to avoid
    // expensive checkpoint/seek on each diff. Fallback to index-based
    // lookup only when metadata is unavailable.
    Addr expected_trace_pc = 0;
    bool have_expected = false;
    if (const o3::TraceInstruction *ti_cur =
            cpu->getTraceInstMetadata(head_inst->seqNum)) {
        expected_trace_pc = ti_cur->getPC();
        have_expected = ti_cur->isValid();
    } else {
        expected_trace_pc = cpu->getTracePCByIndex(traceCommitIndex[tid]);
        have_expected = (expected_trace_pc != 0);
    }

    Addr commit_pc = head_inst->pcState().instAddr();
    DPRINTF(CommitTrace,
            "[tid:%d idx:%llu sn:%llu] Commit vs Trace PC: commit=0x%lx expect=0x%lx "
            "(have=%d)\n",
            tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
            (unsigned long)commit_pc, (unsigned long)expected_trace_pc, have_expected);

    // 2) Strong check on PC
    if (have_expected && commit_pc != expected_trace_pc) {
        panic(
            "[Commit][tid:%d idx:%llu sn:%llu] PC mismatch: commit=0x%lx, trace=0x%lx",
            tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
            (unsigned long)commit_pc, (unsigned long)expected_trace_pc);
    }

    // 2.1) Strong check on instruction type (if metadata available)
    // metadata must be available for this check
    if (!cpu->isTraceInstruction(head_inst->seqNum)) {
        panic(
            "[Commit][tid:%d idx:%llu sn:%llu] Missing trace metadata for instruction "
            "type difftest (pc=0x%lx)",
            tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
            (unsigned long)commit_pc);
    }
    if (const o3::TraceInstruction *ti_meta =
            cpu->getTraceInstMetadata(head_inst->seqNum)) {
        auto classifyInstType = [](const StaticInstPtr &si) -> o3::TraceInstruction::InstType {
            if (!si) return o3::TraceInstruction::InstType::UNDEFINED;
            if (si->isLoad()) return o3::TraceInstruction::InstType::LOAD;
            if (si->isStore()) return o3::TraceInstruction::InstType::STORE;
            if (si->isControl()) {
                if (si->isReturn()) return o3::TraceInstruction::InstType::RETURN;
                if (si->isCall()) {
                    return si->isIndirectCtrl()
                        ? o3::TraceInstruction::InstType::CALL_INDIRECT
                        : o3::TraceInstruction::InstType::CALL_DIRECT;
                }
                if (si->isUncondCtrl()) {
                    return si->isIndirectCtrl()
                        ? o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH
                        : o3::TraceInstruction::InstType::UNCOND_DIRECT_BRANCH;
                }
                return o3::TraceInstruction::InstType::COND_BRANCH;
            }
            if (si->isFloating() || si->isVector()) return o3::TraceInstruction::InstType::FP;
            return o3::TraceInstruction::InstType::ALU;
        };

        auto commit_type = classifyInstType(head_inst->staticInst);
        auto trace_type = ti_meta->getInstType();
        // Allow trace hints to override static decode for call/return/indirect
        if (head_inst->traceIsCall()) {
            trace_type = head_inst->traceIsIndirect()
                ? o3::TraceInstruction::InstType::CALL_INDIRECT
                : o3::TraceInstruction::InstType::CALL_DIRECT;
        } else if (head_inst->traceIsReturn()) {
            trace_type = o3::TraceInstruction::InstType::RETURN;
        }
        const bool skip_type_check =
            (ti_meta->getInstSizeBytes() == 2 &&
             ti_meta->getInstType() == o3::TraceInstruction::InstType::FP);
        DPRINTF(CommitTrace, "[tid:%d idx:%llu sn:%llu] InstType: commit=%s, trace=%s\n",
                tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
                ([&]() {
                    switch (commit_type) {
                        case o3::TraceInstruction::InstType::ALU: return "ALU";
                        case o3::TraceInstruction::InstType::LOAD: return "LOAD";
                        case o3::TraceInstruction::InstType::STORE: return "STORE";
                        case o3::TraceInstruction::InstType::COND_BRANCH:
                            return "COND_BRANCH";
                        case o3::TraceInstruction::InstType::UNCOND_DIRECT_BRANCH:
                            return "UNCOND_DIRECT_BRANCH";
                        case o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH:
                            return "UNCOND_INDIRECT_BRANCH";
                        case o3::TraceInstruction::InstType::FP: return "FP";
                        case o3::TraceInstruction::InstType::SLOW_ALU: return "SLOW_ALU";
                        case o3::TraceInstruction::InstType::CALL_DIRECT:
                            return "CALL_DIRECT";
                        case o3::TraceInstruction::InstType::CALL_INDIRECT:
                            return "CALL_INDIRECT";
                        case o3::TraceInstruction::InstType::RETURN: return "RETURN";
                        default: return "UNDEFINED";
                    }
                })(),
                ti_meta->getInstTypeStr());

        if (!skip_type_check && commit_type != trace_type) {
            panic(
                "[Commit][tid:%d idx:%llu sn:%llu] InstType mismatch: commit=%s, "
                "trace=%s (pc=0x%lx)",
                tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
                ([&]() {
                    switch (commit_type) {
                        case o3::TraceInstruction::InstType::ALU: return "ALU";
                        case o3::TraceInstruction::InstType::LOAD: return "LOAD";
                        case o3::TraceInstruction::InstType::STORE: return "STORE";
                        case o3::TraceInstruction::InstType::COND_BRANCH:
                            return "COND_BRANCH";
                        case o3::TraceInstruction::InstType::UNCOND_DIRECT_BRANCH:
                            return "UNCOND_DIRECT_BRANCH";
                        case o3::TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH:
                            return "UNCOND_INDIRECT_BRANCH";
                        case o3::TraceInstruction::InstType::FP: return "FP";
                        case o3::TraceInstruction::InstType::SLOW_ALU: return "SLOW_ALU";
                        case o3::TraceInstruction::InstType::CALL_DIRECT:
                            return "CALL_DIRECT";
                        case o3::TraceInstruction::InstType::CALL_INDIRECT:
                            return "CALL_INDIRECT";
                        case o3::TraceInstruction::InstType::RETURN: return "RETURN";
                        default: return "UNDEFINED";
                    }
                })(),
                ti_meta->getInstTypeStr(), (unsigned long)commit_pc);
        }

        // 2.2) Optional memory diffs (non-fatal)
        if (ti_meta->isMemoryOp()) {
            if (!ti_meta->getLoadAddresses().empty() ||
                !ti_meta->getStoreAddresses().empty()) {
                Addr trace_addr = 0;
                if (ti_meta->getLoad()) {
                    trace_addr = ti_meta->getLoadAddresses().empty()
                        ? 0
                        : ti_meta->getLoadAddresses()[0];
                } else if (ti_meta->getStore()) {
                    trace_addr = ti_meta->getStoreAddresses().empty()
                        ? 0
                        : ti_meta->getStoreAddresses()[0];
                }
                if (head_inst->effAddrValid()) {
                    Addr commit_addr = head_inst->effAddr;
                    if (trace_addr != 0 && commit_addr != trace_addr) {
                        DPRINTF(
                            CommitTrace,
                            "[tid:%d idx:%llu sn:%llu] Mem addr mismatch: commit=0x%lx "
                            "trace=0x%lx (pc=0x%lx)\n",
                            tid, (unsigned long long)traceCommitIndex[tid],
                            head_inst->seqNum, (unsigned long)commit_addr,
                            (unsigned long)trace_addr, (unsigned long)commit_pc);
                    }
                }
            }

            if (!ti_meta->getMemSizes().empty()) {
                uint32_t trace_size = ti_meta->getMemSizes()[0];
                if (head_inst->effSize != 0 && head_inst->effSize != trace_size) {
                    DPRINTF(
                        CommitTrace,
                        "[tid:%d idx:%llu sn:%llu] Mem size mismatch: commit=%uB trace=%uB "
                        "(pc=0x%lx)\n",
                        tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
                        head_inst->effSize, trace_size, (unsigned long)commit_pc);
                }
            }
        }
    }

    // 3) Branch info (non-fatal)
    if (const o3::TraceInstruction *ti_br = cpu->getTraceInstMetadata(head_inst->seqNum)) {
        if (ti_br->getBranch()) {
            auto &rvpc = head_inst->pcState().as<RiscvISA::PCState>();
            const Addr actual_next = rvpc.npc();
            const Addr fall_through = rvpc.getFallThruPC();
            const bool actual_taken = (actual_next != fall_through);
            const bool expect_taken = ti_br->getBranchTaken();
            const bool expect_has_tgt = ti_br->getHasBranchTarget();
            const Addr expect_tgt = expect_has_tgt ? ti_br->getBranchTarget() : fall_through;

            DPRINTF(
                CommitTrace,
                "[tid:%d idx:%llu sn:%llu] Branch: actual_taken=%d expect_taken=%d next=0x%lx "
                "tgt=0x%lx ft=0x%lx\n",
                tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
                actual_taken, expect_taken, (unsigned long)actual_next,
                (unsigned long)expect_tgt, (unsigned long)fall_through);

            if (actual_taken != expect_taken) {
                DPRINTF(CommitTrace,
                        "[tid:%d idx:%llu sn:%llu] Branch taken mismatch: actual=%d "
                        "expect=%d\n",
                        tid, (unsigned long long)traceCommitIndex[tid], head_inst->seqNum,
                        actual_taken, expect_taken);
            }
            if (expect_taken) {
                if (actual_next != expect_tgt) {
                    DPRINTF(CommitTrace,
                            "[tid:%d idx:%llu sn:%llu] Branch target mismatch: actual=0x%lx "
                            "expect=0x%lx\n",
                            tid, (unsigned long long)traceCommitIndex[tid],
                            head_inst->seqNum, (unsigned long)actual_next,
                            (unsigned long)expect_tgt);
                }
            } else {
                if (actual_next != fall_through) {
                    DPRINTF(CommitTrace,
                            "[tid:%d idx:%llu sn:%llu] Branch fall-through mismatch: actual=0x%lx "
                            "fall=0x%lx\n",
                            tid, (unsigned long long)traceCommitIndex[tid],
                            head_inst->seqNum, (unsigned long)actual_next,
                            (unsigned long)fall_through);
                }
            }
        }
    }
}

} // namespace o3
} // namespace gem5

/*
 * Copyright (c) 2010-2012, 2015, 2017, 2018, 2020 ARM Limited
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
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
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

#include "cpu/simple/base.hh"

#include "arch/generic/decoder.hh"
#include "base/cprintf.hh"
#include "base/inifile.hh"
#include "base/loader/symtab.hh"
#include "base/logging.hh"
#include "base/pollevent.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/checker/thread_context.hh"
#include "cpu/exetrace.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/simple/exec_context.hh"
#include "cpu/simple_thread.hh"
#include "cpu/smt.hh"
#include "cpu/static_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Decode.hh"
#include "debug/ExecFaulting.hh"
#include "debug/Fetch.hh"
#include "debug/HtmCpu.hh"
#include "debug/Quiesce.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/BaseSimpleCPU.hh"
#include "sim/byteswap.hh"
#include "sim/debug.hh"
#include "sim/faults.hh"
#include "sim/full_system.hh"
#include "sim/sim_events.hh"
#include "sim/sim_object.hh"
#include "sim/stats.hh"
#include "sim/system.hh"
#include "arch/riscv/regs/misc.hh"
#include "debug/SimpleCPU.hh"

namespace gem5
{

BaseSimpleCPU::BaseSimpleCPU(const BaseSimpleCPUParams &p)
    : BaseCPU(p),
      curThread(0),
      branchPred(p.branchPred),
      traceData(NULL),
      _status(Idle),
      enableDifftest(p.enable_difftest)
{
    SimpleThread *thread;

    for (unsigned i = 0; i < numThreads; i++) {
        if (FullSystem) {
            thread = new SimpleThread(
                this, i, p.system, p.mmu, p.isa[i], p.decoder[i]);
        } else {
            thread = new SimpleThread(
                this, i, p.system, p.workload[i], p.mmu, p.isa[i],
                p.decoder[i]);
        }
        threadInfo.push_back(new SimpleExecContext(this, thread));
        ThreadContext *tc = thread->getTC();
        threadContexts.push_back(tc);
    }

    if (p.checker) {
        if (numThreads != 1)
            fatal("Checker currently does not support SMT");

        BaseCPU *temp_checker = p.checker;
        checker = dynamic_cast<CheckerCPU *>(temp_checker);
        checker->setSystem(p.system);
        // Manipulate thread context
        ThreadContext *cpu_tc = threadContexts[0];
        threadContexts[0] = new CheckerThreadContext<ThreadContext>(
                cpu_tc, this->checker);
    } else {
        checker = NULL;
    }
    if (enableDifftest) {
        assert(p.difftest_ref_so.length() > 2);
        diff.nemu_reg = referenceRegFile;
        // diff.wpc = diffWPC;
        // diff.wdata = diffWData;
        // diff.wdst = diffWDst;
        diff.nemu_this_pc = 0x80000000u;
        diff.cpu_id = p.cpu_id;
        warn("cpu_id set to %d\n", p.cpu_id);
        proxy = new NemuProxy(
            p.cpu_id, p.difftest_ref_so.c_str(),
            p.nemuSDimg.size() && p.nemuSDCptBin.size());
        warn("Difftest is enabled with ref so: %s.\n",
             p.difftest_ref_so.c_str());
        proxy->regcpy(gem5RegFile, REF_TO_DUT);
        diff.dynamic_config.ignore_illegal_mem_access = false;
        diff.dynamic_config.debug_difftest = false;
        proxy->update_config(&diff.dynamic_config);
        if (p.nemuSDimg.size() && p.nemuSDCptBin.size()) {
            proxy->sdcard_init(p.nemuSDimg.c_str(),
                               p.nemuSDCptBin.c_str());
        }
        diff.will_handle_intr = false;
    }
    else {
        warn("Difftest is disabled\n");
        hasCommit = true;
    }
}

void
BaseSimpleCPU::checkPcEventQueue()
{
    Addr oldpc, pc = threadInfo[curThread]->thread->pcState().instAddr();
    do {
        oldpc = pc;
        threadInfo[curThread]->thread->pcEventQueue.service(
                oldpc, threadContexts[curThread]);
        pc = threadInfo[curThread]->thread->pcState().instAddr();
    } while (oldpc != pc);
}

void
BaseSimpleCPU::swapActiveThread()
{
    if (numThreads > 1) {
        if ((!curStaticInst || !curStaticInst->isDelayedCommit()) &&
             !threadInfo[curThread]->stayAtPC) {
            // Swap active threads
            if (!activeThreads.empty()) {
                curThread = activeThreads.front();
                activeThreads.pop_front();
                activeThreads.push_back(curThread);
            }
        }
    }
}

void
BaseSimpleCPU::countInst()
{
    SimpleExecContext& t_info = *threadInfo[curThread];

    if (!curStaticInst->isMicroop() || curStaticInst->isLastMicroop()) {
        t_info.numInst++;
        t_info.execContextStats.numInsts++;
    }
    t_info.numOp++;
    t_info.execContextStats.numOps++;
}

Counter
BaseSimpleCPU::totalInsts() const
{
    Counter total_inst = 0;
    for (auto& t_info : threadInfo) {
        total_inst += t_info->numInst;
    }

    return total_inst;
}

Counter
BaseSimpleCPU::totalOps() const
{
    Counter total_op = 0;
    for (auto& t_info : threadInfo) {
        total_op += t_info->numOp;
    }

    return total_op;
}

BaseSimpleCPU::~BaseSimpleCPU()
{
}

void
BaseSimpleCPU::haltContext(ThreadID thread_num)
{
    // for now, these are equivalent
    suspendContext(thread_num);
    updateCycleCounters(BaseCPU::CPU_STATE_SLEEP);
}

void
BaseSimpleCPU::resetStats()
{
    BaseCPU::resetStats();
    for (auto &thread_info : threadInfo) {
        thread_info->execContextStats.notIdleFraction = (_status != Idle);
    }
}

void
BaseSimpleCPU::serializeThread(CheckpointOut &cp, ThreadID tid) const
{
    assert(_status == Idle || _status == Running);

    threadInfo[tid]->thread->serialize(cp);
}

void
BaseSimpleCPU::unserializeThread(CheckpointIn &cp, ThreadID tid)
{
    threadInfo[tid]->thread->unserialize(cp);
}

void
change_thread_state(ThreadID tid, int activate, int priority)
{
}

void
BaseSimpleCPU::wakeup(ThreadID tid)
{
    getCpuAddrMonitor(tid)->gotWakeup = true;

    if (threadInfo[tid]->thread->status() == ThreadContext::Suspended) {
        DPRINTF(Quiesce,"[tid:%d] Suspended Processor awoke\n", tid);
        threadInfo[tid]->thread->activate();
    }
}

void
BaseSimpleCPU::traceFault()
{
    if (debug::ExecFaulting) {
        traceData->setFaulting(true);
    } else {
        delete traceData;
        traceData = NULL;
    }
}

void
BaseSimpleCPU::checkForInterrupts()
{
    SimpleExecContext&t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;
    ThreadContext* tc = thread->getTC();
    if (checkInterrupts(curThread)) {
        Fault interrupt = interrupts[curThread]->getInterrupt();

        if (interrupt != NoFault) {
            // hardware transactional memory
            // Postpone taking interrupts while executing transactions.
            assert(!std::dynamic_pointer_cast<GenericHtmFailureFault>(
                interrupt));
            if (t_info.inHtmTransactionalState()) {
                DPRINTF(HtmCpu, "Deferring pending interrupt - %s -"
                    "due to transactional state\n",
                    interrupt->name());
                return;
            }

            t_info.fetchOffset = 0;
            interrupts[curThread]->updateIntrInfo();
            interrupt->invoke(tc);
            thread->decoder->reset();
        }
    }
}


void
BaseSimpleCPU::setupFetchRequest(const RequestPtr &req)
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;

    auto &decoder = thread->decoder;
    Addr instAddr = thread->pcState().instAddr();
    Addr fetchPC = (instAddr & decoder->pcMask()) + t_info.fetchOffset;

    // set up memory request for instruction fetch
    DPRINTF(Fetch, "Fetch: Inst PC:%08p, Fetch PC:%08p\n", instAddr, fetchPC);

    req->setVirt(fetchPC, decoder->moreBytesSize(), Request::INST_FETCH,
                 instRequestorId(), instAddr);
}

void
BaseSimpleCPU::serviceInstCountEvents()
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    t_info.thread->comInstEventQueue.serviceEvents(t_info.numInst);
}

void
BaseSimpleCPU::preExecute()
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;

    // resets predicates
    t_info.setPredicate(true);
    t_info.setMemAccPredicate(true);

    // decode the instruction
    set(preExecuteTempPC, thread->pcState());
    auto &pc_state = *preExecuteTempPC;

    auto &decoder = thread->decoder;

    if (isRomMicroPC(pc_state.microPC())) {
        t_info.stayAtPC = false;
        curStaticInst = decoder->fetchRomMicroop(
                pc_state.microPC(), curMacroStaticInst);
    } else if (!curMacroStaticInst) {
        //We're not in the middle of a macro instruction
        StaticInstPtr instPtr = NULL;

        //Predecode, ie bundle up an ExtMachInst
        //If more fetch data is needed, pass it in.
        Addr fetch_pc =
            (pc_state.instAddr() & decoder->pcMask()) + t_info.fetchOffset;

        decoder->moreBytes(pc_state, fetch_pc);

        //Decode an instruction if one is ready. Otherwise, we'll have to
        //fetch beyond the MachInst at the current pc.
        instPtr = decoder->decode(pc_state);
        if (instPtr) {
            t_info.stayAtPC = false;
            thread->pcState(pc_state);
        } else {
            t_info.stayAtPC = true;
            t_info.fetchOffset += decoder->moreBytesSize();
        }

        //If we decoded an instruction and it's microcoded, start pulling
        //out micro ops
        if (instPtr && instPtr->isMacroop()) {
            curMacroStaticInst = instPtr;
            curStaticInst =
                curMacroStaticInst->fetchMicroop(pc_state.microPC());
        } else {
            curStaticInst = instPtr;
        }
    } else {
        //Read the next micro op from the macro op
        curStaticInst = curMacroStaticInst->fetchMicroop(pc_state.microPC());
    }
    curInstStrictOrdered = false;

    //If we decoded an instruction this "tick", record information about it.
    if (curStaticInst) {
#if TRACING_ON
        traceData = tracer->getInstRecord(curTick(), thread->getTC(),
                curStaticInst, thread->pcState(), curMacroStaticInst);
#endif // TRACING_ON
    }

    if (branchPred && curStaticInst &&
        curStaticInst->isControl()) {
        // Use a fake sequence number since we only have one
        // instruction in flight at the same time.
        const InstSeqNum cur_sn(0);
        set(t_info.predPC, thread->pcState());
        const bool predict_taken(
            branchPred->predict(curStaticInst, cur_sn, *t_info.predPC,
                curThread));

        if (predict_taken)
            ++t_info.execContextStats.numPredictedBranches;
    }
}

void
BaseSimpleCPU::postExecute()
{
    SimpleExecContext &t_info = *threadInfo[curThread];

    assert(curStaticInst);
    
    Addr instAddr = threadContexts[curThread]->pcState().instAddr();

    if (curStaticInst->isMemRef()) {
        t_info.execContextStats.numMemRefs++;
    }

    if (curStaticInst->isLoad()) {
        ++t_info.numLoad;
    }

    if (curStaticInst->isControl()) {
        ++t_info.execContextStats.numBranches;
    }

    /* Power model statistics */
    //integer alu accesses
    if (curStaticInst->isInteger()){
        t_info.execContextStats.numIntAluAccesses++;
        t_info.execContextStats.numIntInsts++;
    }

    //float alu accesses
    if (curStaticInst->isFloating()){
        t_info.execContextStats.numFpAluAccesses++;
        t_info.execContextStats.numFpInsts++;
    }

    //vector alu accesses
    if (curStaticInst->isVector()){
        t_info.execContextStats.numVecAluAccesses++;
        t_info.execContextStats.numVecInsts++;
    }

    //number of function calls/returns to get window accesses
    if (curStaticInst->isCall() || curStaticInst->isReturn()){
        t_info.execContextStats.numCallsReturns++;
    }

    //the number of branch predictions that will be made
    if (curStaticInst->isCondCtrl()){
        t_info.execContextStats.numCondCtrlInsts++;
    }

    //result bus acceses
    if (curStaticInst->isLoad()){
        t_info.execContextStats.numLoadInsts++;
    }

    if (curStaticInst->isStore() || curStaticInst->isAtomic()){
        t_info.execContextStats.numStoreInsts++;
    }
    /* End power model statistics */

    t_info.execContextStats.statExecutedInstType[curStaticInst->opClass()]++;

    if (FullSystem)
        traceFunctions(instAddr);

    if (traceData) {
        traceData->dump();
        delete traceData;
        traceData = NULL;
    }
    //
    if (enableDifftest) {
        difftestStep(curStaticInst, threadContexts[curThread]->pcState());
    }
    // Call CPU instruction commit probes
    probeInstCommit(curStaticInst, instAddr);
}

void
BaseSimpleCPU::advancePC(const Fault &fault)
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;

    const bool branching = thread->pcState().branching();

    //Since we're moving to a new pc, zero out the offset
    t_info.fetchOffset = 0;
    if (fault != NoFault) {
        curMacroStaticInst = nullStaticInstPtr;
        fault->invoke(threadContexts[curThread], curStaticInst);
        thread->decoder->reset();
    } else {
        if (curStaticInst) {
            if (curStaticInst->isLastMicroop())
                curMacroStaticInst = nullStaticInstPtr;
            curStaticInst->advancePC(thread);
        }
    }

    if (branchPred && curStaticInst && curStaticInst->isControl()) {
        // Use a fake sequence number since we only have one
        // instruction in flight at the same time.
        const InstSeqNum cur_sn(0);

        if (*t_info.predPC == thread->pcState()) {
            // Correctly predicted branch
            branchPred->update(cur_sn, curThread);
        } else {
            // Mis-predicted branch
            branchPred->squash(cur_sn, thread->pcState(), branching,
                    curThread);
            ++t_info.execContextStats.numBranchMispred;
        }
    }
}

void
BaseSimpleCPU::readGem5Regs()
{
    for (int i = 0; i < 32; i++) {
        gem5RegFile[i] =
            threadContexts[curThread]->getReg(RegId(IntRegClass, i));
        gem5RegFile[i + 32] =
            threadContexts[curThread]->getReg(RegId(FloatRegClass, i));
    }
}

std::pair<int, bool>
BaseSimpleCPU::diffWithNEMU(const StaticInstPtr &inst,
                            const PCStateBase &curPC)
{
    int diff_at = DiffAt::NoneDiff;
    bool npc_match = false;
    bool is_mmio = curInstStrictOrdered;

    if (inst->isStoreConditional()) {
        diff.sync.lrscValid = true;
        proxy->uarchstatus_cpy(&diff.sync, DIFFTEST_TO_REF);
    }
    if (is_mmio) {
        // ismmio
        diff.dynamic_config.ignore_illegal_mem_access = true;
        proxy->update_config(&diff.dynamic_config);
    }

    if (diff.will_handle_intr) {
        proxy->regcpy(diff.nemu_reg, REF_TO_DIFFTEST);
        diff.nemu_this_pc = diff.nemu_reg[DIFFTEST_THIS_PC];
        diff.will_handle_intr = false;
    }
    // difftest step start
    proxy->exec(1);
    proxy->regcpy(diff.nemu_reg, REF_TO_DIFFTEST);

    uint64_t next_pc = diff.nemu_reg[DIFFTEST_THIS_PC];

    // replace with "this pc" for checking
    diff.nemu_commit_inst_pc = diff.nemu_this_pc;
    diff.nemu_this_pc = next_pc;
    diff.npc = next_pc;
    // difftest step end

    if (is_mmio) {
        // ismmio
        diff.dynamic_config.ignore_illegal_mem_access = false;
        proxy->update_config(&diff.dynamic_config);
    }
    auto gem5_pc = curPC.instAddr();
    auto nemu_pc = diff.nemu_commit_inst_pc;
    DPRINTF(SimpleCPU, "NEMU PC: %#10lx, GEM5 PC: %#10lx, inst: %s\n", nemu_pc,
            gem5_pc, inst->disassemble(curPC.instAddr()).c_str());

    // auto nemu_store_addr = referenceRegFile[DIFFTEST_STORE_ADDR];
    // if (nemu_store_addr) {
    //     DPRINTF(ValueCommit, "NEMU store addr: %#lx\n", nemu_store_addr);
    // }

    // uint8_t gem5_inst[5];
    // uint8_t nemu_inst[9];

    // int nemu_inst_len = referenceRegFile[DIFFTEST_RVC] ? 2 : 4;
    // int gem5_inst_len = inst->pcState().compressed() ? 2 : 4;

    // assert(inst->staticInst->asBytes(gem5_inst, 8));
    // *reinterpret_cast<uint32_t *>(nemu_inst) =
    //     htole<uint32_t>(referenceRegFile[DIFFTEST_INST_PAYLOAD]);

    if (nemu_pc != gem5_pc) {
        // warn("NEMU store addr: %#lx\n", nemu_store_addr);
        DPRINTF(SimpleCPU, "Inst [sn:%lli]\n", 0);
        DPRINTF(SimpleCPU, "Diff at %s, NEMU: %#lx, GEM5: %#lx\n", "PC",
                nemu_pc, gem5_pc);
        if (!diff_at) {
            diff_at = PCDiff;
            if (diff.npc == gem5_pc) {
                npc_match = true;
            }
        }
    }
    DPRINTF(SimpleCPU, "Inst [sn:%lli] PC, NEMU: %#lx, GEM5: %#lx\n", 0,
            nemu_pc, gem5_pc);

    DPRINTF(SimpleCPU, "Inst [sn:%llu] @ %#lx in GEM5 is %s\n", 0,
            curPC.instAddr(), inst->disassemble(curPC.instAddr()));
    if (inst->numDestRegs() > 0) {
        const auto &dest = inst->destRegIdx(0);
        auto dest_tag = dest.index() + dest.isFloatReg() * 32;

        if ((dest.isFloatReg() || dest.isIntReg()) && !dest.isZeroReg()) {
            auto gem5_val = threadContexts[curThread]->getReg(dest);
            auto nemu_val = referenceRegFile[dest_tag];

            DPRINTF(SimpleCPU, "At %s Ref value: %#lx, GEM5 value: %#lx\n",
                    reg_name[dest_tag], nemu_val, gem5_val);
            if (gem5_val != nemu_val) {
                if (dest.isFloatReg() &&
                    (gem5_val ^ nemu_val) == ((0xffffffffULL) << 32)) {
                    DPRINTF(SimpleCPU,
                            "Difference might be caused by box,"
                            " ignore it\n");

                } else if (is_mmio) {
                    // DPRINTF(SimpleCPU,
                    // 		"Difference might be caused by read %s at %#lx,"
                    // 		" ignore it\n",
                    // 		"mmio", inst->physEffAddr);
                    referenceRegFile[dest_tag] = gem5_val;
                    proxy->regcpy(referenceRegFile, DUT_TO_REF);
                } else {
                    for (int i = 0; i < inst->numSrcRegs(); i++) {
                        const auto &src = inst->srcRegIdx(i);
                        DPRINTF(SimpleCPU, "Src%d %s = %lx\n", i,
                                reg_name[src.index()],
                                threadContexts[curThread]->getReg(src));
                    }
                    DPRINTF(SimpleCPU, "Inst src count: %u, dest count: %u\n",
                            inst->numSrcRegs(), inst->numDestRegs());
                    warn("Inst [sn:%lli] pc:%s\n", 0, curPC);
                    warn("Diff at %s Ref value: %#lx, GEM5 value: %#lx\n",
                         reg_name[dest_tag], nemu_val, gem5_val);
                    if (!diff_at)
                        diff_at = ValueDiff;
                }
            }
        }

        // always check some CSR regs
        {
            // mstatus
            auto gem5_val = threadContexts[curThread]->readMiscRegNoEffect(
                RiscvISA::MiscRegIndex::MISCREG_STATUS);
            // readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, 0);
            auto ref_val = referenceRegFile[DIFFTEST_MSTATUS];
            if (gem5_val != ref_val) {
                warn("Inst [sn:%lli] pc:%s\n", 0, curPC);
                warn("Diff at %s Ref value: %#lx, GEM5 value: %#lx\n",
                     "mstatus", ref_val, gem5_val);
                if (!diff_at)
                    diff_at = ValueDiff;
            }
            // mcause
            gem5_val = threadContexts[curThread]->readMiscRegNoEffect(
                RiscvISA::MiscRegIndex::MISCREG_MCAUSE);
            // readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_MCAUSE, 0);
            ref_val = referenceRegFile[DIFFTEST_MCAUSE];
            if (gem5_val != ref_val) {
                warn("Inst [sn:%lli] pc:%s\n", 0, curPC);
                warn("Diff at %s Ref value: %#lx, GEM5 value: %#lx\n",
                     "mcause", ref_val, gem5_val);
                if (!diff_at)
                    diff_at = ValueDiff;
            }
            // satp
            gem5_val = threadContexts[curThread]->readMiscRegNoEffect(
                RiscvISA::MiscRegIndex::MISCREG_SATP);
            // readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_SATP, 0);
            ref_val = referenceRegFile[DIFFTEST_SATP];
            if (gem5_val != ref_val) {
                warn("Inst [sn:%lli] pc:%s\n", 0, curPC);
                warn("Diff at %s Ref value: %#lx, GEM5 value: %#lx\n", "satp",
                     ref_val, gem5_val);
                if (!diff_at)
                    diff_at = ValueDiff;
            }

            // mie
            gem5_val = threadContexts[curThread]->readMiscReg(
                RiscvISA::MiscRegIndex::MISCREG_IE);
            // readMiscReg(RiscvISA::MiscRegIndex::MISCREG_IE, 0);
            ref_val = referenceRegFile[DIFFTEST_MIE];
            if (gem5_val != ref_val) {
                warn("Inst [sn:%lli] pc:%s\n", 0, curPC);
                warn("Diff at %s Ref value: %#lx, GEM5 value: %#lx\n", "mie",
                     ref_val, gem5_val);
                if (!diff_at)
                    diff_at = ValueDiff;
            }
            // mip
            gem5_val = threadContexts[curThread]->readMiscReg(
                RiscvISA::MiscRegIndex::MISCREG_IP);
            // readMiscReg(RiscvISA::MiscRegIndex::MISCREG_IP, 0);
            ref_val = referenceRegFile[DIFFTEST_MIP];
            if (gem5_val != ref_val) {
                DPRINTF(SimpleCPU, "Inst [sn:%lli] pc:%s\n", 0, curPC);
                DPRINTF(SimpleCPU,
                        "Diff at %s Ref value: %#lx, GEM5 value: %#lx\n",
                        "mip", ref_val, gem5_val);
            }

            if (diff_at != NoneDiff) {
                warn("Inst [sn:%llu] @ %#lx in GEM5 is %s\n", 0,
                     curPC.instAddr(), inst->disassemble(curPC.instAddr()));
                if (inst->isLoad()) {
                    // warn("Load addr: %#lx\n", inst->physEffAddr);
                }
            }
        }
    }
    return std::make_pair(diff_at, npc_match);
}


void
BaseSimpleCPU::difftestStep(const StaticInstPtr &inst,
                            const PCStateBase &curPC)
{
    bool should_diff = false;
    DPRINTF(SimpleCPU, "DiffTest step on inst pc: %#lx: %s\n",
            curPC.instAddr(), inst->disassemble(curPC.instAddr()));
    // Keep an instruction count.
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        should_diff = true;
        if (!hasCommit && curPC.instAddr() == 0x80000000u) {
            hasCommit = true;
            readGem5Regs();
            gem5RegFile[DIFFTEST_THIS_PC] = curPC.instAddr();
            fprintf(stderr, "Will start memcpy to NEMU from %#lx, size=%lu\n",
                    (uint64_t)pmemStart, pmemSize);
            proxy->memcpy(0x80000000u, pmemStart + pmemSize * diff.cpu_id,
                          pmemSize, DUT_TO_REF);
            fprintf(stderr, "Will start regcpy to NEMU\n");
            proxy->regcpy(gem5RegFile, DUT_TO_REF);
        }

        if (scFenceInFlight) {
            assert(inst->isWriteBarrier() && inst->isReadBarrier());
            should_diff = false;
        }
    }

    scFenceInFlight = false;

    if (!inst->isLastMicroop() && inst->isStoreConditional() &&
        inst->isDelayedCommit()) {
        scFenceInFlight = true;
        should_diff = true;
    }

    if (enableDifftest && should_diff) {
        auto [diff_at, npc_match] = diffWithNEMU(inst, curPC);
        if (diff_at != NoneDiff) {
            if (npc_match && diff_at == PCDiff) {
                // warn("Found PC mismatch, Let NEMU run one more
                // instruction\n");
                std::tie(diff_at, npc_match) = diffWithNEMU(inst, curPC);
                if (diff_at != NoneDiff) {
                    proxy->isa_reg_display();
                    panic("Difftest failed again!\n");
                } else {
                    warn(
                        "Difftest matched again, "
                        "NEMU seems to commit the failed mem instruction\n");
                }
            } else {
                proxy->isa_reg_display();
                panic("Difftest failed!\n");
            }
        }
    }
    DPRINTF(SimpleCPU, "commit_pc: %s\n", curPC);
}

void
BaseSimpleCPU::difftestRaiseIntr(uint64_t no)
{
    diff.will_handle_intr = true;
    proxy->raise_intr(no);
}




} // namespace gem5

/*
 * Copyright (c) 2011-2012,2016-2017, 2019-2020 ARM Limited
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
 * Copyright (c) 2011 Regents of the University of California
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * Copyright (c) 2013 Mark D. Hill and David A. Wood
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

#include "arch/riscv/insts/fusion.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/cprintf.hh"
#include "base/trace.hh"
#include "cpu/base.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Diff.hh"
#include "debug/Diff2.hh"
#include "debug/DiffValue.hh"
#include "debug/DumpCommit.hh"
#include "params/BaseCPU.hh"
#include "sim/system.hh"

namespace gem5
{

void
BaseCPU::captureInitialDifftestState(ThreadID tid)
{
    auto &context = *diffAllStates[tid];
    auto &state = context.initialDutState;
    state = {};
    readDutRegs(tid, state);

    state.mode = readMiscRegNoEffect(RiscvISA::MISCREG_PRV, tid);
    state.mstatus = readMiscRegNoEffect(RiscvISA::MISCREG_STATUS, tid);
    state.sstatus = state.mstatus & RiscvISA::NEMU_SSTATUS_RMASK;
    state.mepc = readMiscRegNoEffect(RiscvISA::MISCREG_MEPC, tid);
    state.sepc = readMiscRegNoEffect(RiscvISA::MISCREG_SEPC, tid);
    state.mtval = readMiscRegNoEffect(RiscvISA::MISCREG_MTVAL, tid);
    state.stval = readMiscRegNoEffect(RiscvISA::MISCREG_STVAL, tid);
    state.mtvec = readMiscRegNoEffect(RiscvISA::MISCREG_MTVEC, tid);
    state.stvec = readMiscRegNoEffect(RiscvISA::MISCREG_STVEC, tid);
    state.mcause = readMiscRegNoEffect(RiscvISA::MISCREG_MCAUSE, tid);
    state.scause = readMiscRegNoEffect(RiscvISA::MISCREG_SCAUSE, tid);
    state.satp = readMiscRegNoEffect(RiscvISA::MISCREG_SATP, tid);
    // IP/IE live in the interrupt controller, not the raw CSR array.
    state.mip = readMiscReg(RiscvISA::MISCREG_IP, tid);
    state.mie = readMiscReg(RiscvISA::MISCREG_IE, tid);
    state.mscratch = readMiscRegNoEffect(RiscvISA::MISCREG_MSCRATCH, tid);
    state.sscratch = readMiscRegNoEffect(RiscvISA::MISCREG_SSCRATCH, tid);
    state.mideleg = readMiscRegNoEffect(RiscvISA::MISCREG_MIDELEG, tid);
    state.medeleg = readMiscRegNoEffect(RiscvISA::MISCREG_MEDELEG, tid);
    state.pc = threadContexts[tid]->pcState().instAddr();

    state.v = readMiscRegNoEffect(RiscvISA::MISCREG_VIRMODE, tid);
    state.mtval2 = readMiscRegNoEffect(RiscvISA::MISCREG_MTVAL2, tid);
    state.mtinst = readMiscRegNoEffect(RiscvISA::MISCREG_MTINST, tid);
    state.hstatus = readMiscRegNoEffect(RiscvISA::MISCREG_HSTATUS, tid);
    state.hideleg = readMiscReg(RiscvISA::MISCREG_HIDELEG, tid);
    state.hedeleg = readMiscRegNoEffect(RiscvISA::MISCREG_HEDELEG, tid);
    state.hcounteren =
        readMiscRegNoEffect(RiscvISA::MISCREG_HCOUNTEREN, tid);
    state.htval = readMiscRegNoEffect(RiscvISA::MISCREG_HTVAL, tid);
    state.htinst = readMiscRegNoEffect(RiscvISA::MISCREG_HTINST, tid);
    state.hgatp = readMiscRegNoEffect(RiscvISA::MISCREG_HGATP, tid);
    state.vsstatus = readMiscRegNoEffect(RiscvISA::MISCREG_VSSTATUS, tid);
    state.vstvec = readMiscRegNoEffect(RiscvISA::MISCREG_VSTVEC, tid);
    state.vsepc = readMiscRegNoEffect(RiscvISA::MISCREG_VSEPC, tid);
    state.vscause = readMiscRegNoEffect(RiscvISA::MISCREG_VSCAUSE, tid);
    state.vstval = readMiscRegNoEffect(RiscvISA::MISCREG_VSTVAL, tid);
    state.vsatp = readMiscRegNoEffect(RiscvISA::MISCREG_VSATP, tid);
    state.vsscratch =
        readMiscRegNoEffect(RiscvISA::MISCREG_VSSCRATCH, tid);

    state.vstart = readMiscRegNoEffect(RiscvISA::MISCREG_VSTART, tid);
    state.vxsat = readMiscRegNoEffect(RiscvISA::MISCREG_VXSAT, tid);
    state.vxrm = readMiscRegNoEffect(RiscvISA::MISCREG_VXRM, tid);
    // These architectural values are synthesized by the ISA reader.
    state.vcsr = readMiscReg(RiscvISA::MISCREG_VCSR, tid);
    state.vl = readMiscRegNoEffect(RiscvISA::MISCREG_VL, tid);
    state.vtype = readMiscRegNoEffect(RiscvISA::MISCREG_VTYPE, tid);
    state.vlenb = readMiscReg(RiscvISA::MISCREG_VLENB, tid);

    state.fcsr =
        (readMiscRegNoEffect(RiscvISA::MISCREG_FFLAGS, tid) &
         RiscvISA::FFLAGS_MASK) |
        ((readMiscRegNoEffect(RiscvISA::MISCREG_FRM, tid) &
          RiscvISA::FRM_MASK) << RiscvISA::FRM_OFFSET);

    context.initialStateCaptured = true;

    DPRINTF(Diff,
            "Captured initial DUT state for tid %d: pc %#lx, mstatus %#lx\n",
            tid, state.pc, state.mstatus);
}

void
BaseCPU::recordCommittedStore(ThreadID tid, const o3::DynInstPtr &inst)
{
    RecentCommittedStore recent;

    if (!system->multiContextDifftest() || !_goldenMemManager ||
        !inst->isStore() || inst->isAtomic() ||
        (inst->isStoreConditional() && !inst->lockedWriteSuccess()) ||
        !inst->memData || inst->effSize == 0 ||
        inst->effSize > sizeof(recent.data) ||
        !_goldenMemManager->inPmem(inst->physEffAddr)) {
        return;
    }

    auto &recent_history = recentCommittedStores.at(tid);
    recent.valid = true;
    recent.addr = inst->physEffAddr;
    recent.size = inst->effSize;
    recent.seq = inst->seqNum;
    std::memcpy(recent.data, inst->memData, recent.size);
    recent_history.push_back(recent);
    constexpr size_t max_store_history = 16;
    if (recent_history.size() > max_store_history) {
        recent_history.pop_front();
    }
}

int
BaseCPU::difftestHartId(ThreadID tid) const
{
    return params().cpu_id * numThreads + tid;
}

void
BaseCPU::csrDiffMessage(uint64_t gem5_val, uint64_t ref_val, int error_num, uint64_t &error_reg, InstSeqNum seq,
                        std::string error_csr_name, int &diff_at)
{
    DPRINTF(DiffValue, "Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
    DPRINTF(DiffValue, "Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
            error_csr_name, ref_val, gem5_val);
    diffInfo.errorCsrsValue[error_num] = 1;
    error_reg = gem5_val;
    if (!diff_at)
        diff_at = ValueDiff;
}



void
BaseCPU::step_difftest_reference(ThreadID tid)
{
    auto &context = *diffAllStates[tid];
    auto &proxy = *context.proxy;
    auto &ref = context.referenceRegFile;
    auto &tracking = context.diff;
    assert(context.referenceInitialized);

    if (diffInfo.inst->isStoreConditional()) {
        proxy.uarchstatus_cpy(&tracking.sync, DIFFTEST_TO_REF);
    }

    if (tracking.will_handle_intr) {
        proxy.copyRegsFromRef(ref);
        tracking.nemu_this_pc = ref.pc;
        tracking.will_handle_intr = false;
    }

    Addr commit_pc = tracking.nemu_this_pc;
    if (diffInfo.curInstStrictOrdered) {
        DPRINTF(Diff, "Skip step NEMU due to mmio access\n");
        // Preserve all REF state, including when the first event is MMIO.
        proxy.copyRegsFromRef(ref);
        ref.pc = diffInfo.pc->as<RiscvISA::PCState>().npc();
        if (diffInfo.inst->numDestRegs() > 0) {
            assert(diffInfo.inst->numDestRegs() == 1);
            const auto &dest = diffInfo.inst->destRegIdx(0);
            assert(dest.isIntReg() || dest.isFloatReg());
            if (!dest.isZeroReg()) {
                unsigned index = dest.index() +
                    (dest.isFloatReg() ? FPRegIndexBase : IntRegIndexBase);
                ref[index] = diffInfo.scalarResults[0];
            }
        }
        proxy.copyRegsToRef(ref);
        commit_pc = diffInfo.inst->isFusion() ?
            dynamic_cast<RiscvISA::FusionInst *>(
                diffInfo.inst.get())->getSecondPC() : diffInfo.pc->instAddr();
    } else {
        DPRINTF(Diff, "Step NEMU\n");
        proxy.exec(1);
        if (diffInfo.inst->isFusion()) {
            proxy.exec(1);
        }
        proxy.copyRegsFromRef(ref);
    }

    tracking.nemu_commit_inst_pc = commit_pc;
    tracking.nemu_this_pc = ref.pc;
    tracking.npc = ref.pc;
}

std::pair<int, bool>
BaseCPU::diffWithNEMU(ThreadID tid, InstSeqNum seq)
{
    step_difftest_reference(tid);
    if (diffInfo.curInstStrictOrdered) {
        return std::make_pair(NoneDiff, true);
    }
    return compare_difftest_state(tid, seq);
}

std::pair<int, bool>
BaseCPU::compare_difftest_state(ThreadID tid, InstSeqNum seq)
{
    auto diffAllStates = this->diffAllStates[tid];
    int diff_at = DiffAt::NoneDiff;
    bool npc_match = false;

    auto gem5_pc = diffInfo.pc->instAddr();
    diffAllStates->gem5RegFile.pc = gem5_pc;
    auto nemu_pc = diffAllStates->diff.nemu_commit_inst_pc;

    if (nemu_pc != gem5_pc) {
        // warn("NEMU store addr: %#lx\n", nemu_store_addr);
        diffMsg << csprintf("Inst [sn:%lli]\n", seq);
        diffMsg << csprintf( "Diff at %s, NEMU: %#lx, GEM5: %#lx\n", "PC", nemu_pc,
                gem5_pc);
        if (!diff_at) {
            diff_at = PCDiff;
            diffInfo.errorPcValue = 1;
            diffMsg << csprintf("GEM5 pc: %#lx, NEMU npc: %#lx\n", gem5_pc,
                    diffAllStates->diff.npc);
            if (diffAllStates->diff.npc == gem5_pc) {
                npc_match = true;
            }
        }
    }
    DPRINTF(Diff2, "pc %#x inst %#x @ %s\n", gem5_pc, diffInfo.pc->instAddr(),
            diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
    DPRINTF(Diff, "Inst [sn:%lli] PC, NEMU: %#lx, GEM5: %#lx\n", seq, nemu_pc,
            gem5_pc);

    DPRINTF(Diff, "Inst [sn:%llu] @ %#lx in GEM5 is %s\n", seq,
            diffInfo.pc->instAddr(),
            diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
    auto machInst = dynamic_cast<RiscvISA::RiscvStaticInst &>(*diffInfo.inst).machInst;
    DPRINTF(Diff, "MachInst: %#lx\n", machInst);


    if (enableRVV) {
        if (diffInfo.inst->isVector()) {
            readDutRegs(tid, diffAllStates->gem5RegFile);
            uint64_t* nemu_val = (uint64_t*)&(diffAllStates->referenceRegFile.vr[0]);
            uint64_t* gem5_val = (uint64_t*)&(diffAllStates->gem5RegFile.vr[0]);
            uint8_t* nemu_byte = (uint8_t*)&(diffAllStates->referenceRegFile.vr[0]);
            uint8_t* gem5_byte = (uint8_t*)&(diffAllStates->gem5RegFile.vr[0]);
            const uint64_t vtype = diffAllStates->referenceRegFile.vtype;
            const uint64_t vl = diffAllStates->referenceRegFile.vl;
            const bool tail_agnostic = bits(vtype, 6);
            const uint32_t sew_bytes = 1 << bits(vtype, 5, 3);
            const uint32_t regs_per_group = RiscvISA::vtype_regs_per_group(vtype);
            const uint32_t elems_per_reg = RiscvISA::VLENB / sew_bytes;
            const uint32_t vlmax = RiscvISA::vtype_VLMAX(vtype);
            auto is_tail_agnostic_byte = [&](int byte_idx) {
                if (!tail_agnostic || vl >= vlmax)
                    return false;
                const int reg_idx = byte_idx / RiscvISA::VLENB;
                const int byte_in_reg = byte_idx % RiscvISA::VLENB;
                auto reg_is_in_group = [&](const RegId &reg) {
                    if (!reg.isVecReg())
                        return false;
                    const int vec_reg = reg.index();
                    const int group_base = vec_reg & ~(regs_per_group - 1);
                    if (reg_idx < group_base ||
                        reg_idx >= group_base + regs_per_group)
                        return false;
                    const uint32_t elem_idx =
                        (reg_idx - group_base) * elems_per_reg +
                        byte_in_reg / sew_bytes;
                    return elem_idx >= vl;
                };
                for (int dest_idx = 0; dest_idx < diffInfo.inst->numDestRegs();
                     dest_idx++) {
                    if (reg_is_in_group(diffInfo.inst->destRegIdx(dest_idx)))
                        return true;
                }
                for (int src_idx = 0; src_idx < diffInfo.inst->numSrcRegs();
                     src_idx++) {
                    if (reg_is_in_group(diffInfo.inst->srcRegIdx(src_idx)))
                        return true;
                }
                return false;
            };
            bool maybe_error = false;
            int error_idx = 0;
            for (int i = 0; i < RiscvISA::VLENB * 32; i++) {
                if (nemu_byte[i] != gem5_byte[i] &&
                    is_tail_agnostic_byte(i))
                    continue;
                if (nemu_byte[i] != gem5_byte[i]) {
                    maybe_error = true;
                    error_idx = (i / RiscvISA::VLENB) *
                                RiscvISA::NumVecElemPerVecReg;
                    break;
                }
            }

            if (maybe_error) {
                std::string gem5_val_, nemu_val_;
                for (int j=RiscvISA::NumVecElemPerVecReg-1; j>=0; j--) {
                    gem5_val_ += csprintf("%016lx", gem5_val[j + error_idx]);
                    if (j != 0) {
                        gem5_val_+="_";
                    }
                }
                for (int j=RiscvISA::NumVecElemPerVecReg-1; j>=0; j--) {
                    nemu_val_ += csprintf("%016lx", nemu_val[j + error_idx]);
                    if (j != 0) {
                        nemu_val_ += "_";
                    }
                }
                warn("May be diff at v%d\n Ref  value: %s\n GEM5 value: %s\n",
                    (error_idx>>1), nemu_val_, gem5_val_);
                diff_at = ValueDiff;
            }
        }

        // vtype
        uint64_t gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VTYPE, tid);
        diffAllStates->gem5RegFile.vtype = gem5_val;
        uint64_t ref_val = diffAllStates->referenceRegFile.vtype;
        // Ignore the high bit for compatibility with older GCBH NEMU.
        if (gem5_val % (1ULL<<63) != ref_val % (1ULL<<63)) {
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "vtype", ref_val, gem5_val);
            if (!diff_at) {
                diff_at = ValueDiff;
            }
        }

        // vstart now do not diff
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTART, tid);
        diffAllStates->gem5RegFile.vstart = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vstart;

        // vxsat
        diffAllStates->gem5RegFile.vxsat = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VXSAT, tid);
        // vxrm
        diffAllStates->gem5RegFile.vxrm = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VXRM, tid);
        // vcsr
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VCSR, tid);
        diffAllStates->gem5RegFile.vcsr = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vcsr;
        if (gem5_val != ref_val) {
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "vcsr", ref_val, gem5_val);
            if (!diff_at) {
                diff_at = ValueDiff;
            }
        }

        // vl
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VL, tid);
        diffAllStates->gem5RegFile.vl = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vl;
        if (gem5_val != ref_val) {
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "vl", ref_val, gem5_val);
            if (!diff_at) {
                diff_at = ValueDiff;
            }
        }
    }

    // always check some CSR regs
    {
        // mstatus
        auto gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_STATUS, tid);
        diffAllStates->gem5RegFile.mstatus = gem5_val;
        // readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, 0);
        auto ref_val = diffAllStates->referenceRegFile.mstatus;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mstatus, diffAllStates->gem5RegFile.mstatus, seq, "mstatus",
                           diff_at);
            diffMsg <<
                csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                         "mstatus", ref_val, gem5_val);
        }
        //stval
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_STVAL, tid);
        diffAllStates->gem5RegFile.stval = gem5_val;
        ref_val = diffAllStates->referenceRegFile.stval;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::stval, diffAllStates->gem5RegFile.stval, seq, "stval",
                           diff_at);
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n", "stval",
                    ref_val, gem5_val);
        }

        // mtval
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_MTVAL, tid);
        diffAllStates->gem5RegFile.mtval = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mtval;
        DPRINTF(Diff, "stvmtvalal:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
        if (gem5_val != ref_val) {
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n", "mtval",
                    ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::mtval] = 1;
            diffAllStates->gem5RegFile.mtval = gem5_val;
            if (!diff_at)
                diff_at = ValueDiff;
        }
            //DIFFTEST_STVDIFFTEST_STVAL

        // mode
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_PRV, tid);
        diffAllStates->gem5RegFile.mode = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mode;
        DPRINTF(Diff, "priv:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
        if (gem5_val != ref_val) {
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n", "priv",
                    ref_val, gem5_val);
            // diffInfo.errorCsrsValue[CsrRegIndex::stval] = 1;
            if (!diff_at)
                diff_at = ValueDiff;
        }

        // mcause
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_MCAUSE, tid);
        diffAllStates->gem5RegFile.mcause = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mcause;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mcause, diffAllStates->gem5RegFile.mcause, seq, "mcause",
                           diff_at);
            diffMsg <<
                csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "mcause", ref_val, gem5_val);
        }

        // scause
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_SCAUSE, tid);
        diffAllStates->gem5RegFile.scause = gem5_val;
        ref_val = diffAllStates->referenceRegFile.scause;
        DPRINTF(Diff, "scause:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
        if (gem5_val != ref_val) {
            diffMsg <<
                csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "scause", ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::scause] = 1;
            diffAllStates->gem5RegFile.scause = gem5_val;
            if (!diff_at)
                diff_at = ValueDiff;
        }
        // satp
        gem5_val =
            readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_SATP, tid);
        diffAllStates->gem5RegFile.satp = gem5_val;
        ref_val = diffAllStates->referenceRegFile.satp;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::satp, diffAllStates->gem5RegFile.satp, seq, "satp",
                           diff_at);
            diffMsg << csprintf("CPU%i Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    cpuId(), "satp", ref_val, gem5_val);
        }

        // mie
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_IE, tid);
        diffAllStates->gem5RegFile.mie = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mie;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mie, diffAllStates->gem5RegFile.mie, seq, "mie", diff_at);
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n", "mie",
                    ref_val, gem5_val);
        }
        // mip
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_IP, tid);
        diffAllStates->gem5RegFile.mip = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mip;
        const RegVal mip_diff_mask = RiscvISA::NEMU_MIP_MASK;
        if ((gem5_val & mip_diff_mask) != (ref_val & mip_diff_mask)) {
            warn("mip:\tGEM5: %#lx,\tREF: %#lx,\tMASK: %#lx\n",
                 gem5_val, ref_val, mip_diff_mask);
            diffMsg <<
                csprintf("%s at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    gem5_val == ref_val ? "match" : "diff", "mip", ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::mip] = 1;
            diffAllStates->gem5RegFile.mip = gem5_val;
        }
        //mepc
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MEPC, tid);
        diffAllStates->gem5RegFile.mepc = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mepc;
        if (gem5_val != ref_val) {
            warn("Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "mepc", ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::mepc] = 1;
            diffAllStates->gem5RegFile.mepc = gem5_val;
        }
    }

    if (enableRVHDIFF){
        // h difftest
        // mtval2
        // no_sync
        bool enable_csrnemu = false;
        if ((diffInfo.instFault == NoFault) && enableSkipCSR) {
            enable_csrnemu = true;
        }
        auto gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL2, tid);
        diffAllStates->gem5RegFile.mtval2 = gem5_val;
        auto ref_val = diffAllStates->referenceRegFile.mtval2;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_MTVAL2, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mtval2, diffAllStates->gem5RegFile.mtval2, seq,
                               "mtval2", diff_at);
            }
        }
        //mtinst
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTINST, tid);
        diffAllStates->gem5RegFile.mtinst = gem5_val;
        ref_val = diffAllStates->referenceRegFile.mtinst;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_MTINST, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mtinst, diffAllStates->gem5RegFile.mtinst, seq,
                               "mtinst", diff_at);
            }
        }
        //hstatus
        //no_sync
        gem5_val = readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_HSTATUS, tid);
        diffAllStates->gem5RegFile.hstatus = gem5_val;
        ref_val = diffAllStates->referenceRegFile.hstatus;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HSTATUS, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hstatus, diffAllStates->gem5RegFile.hstatus, seq,
                               "hstatus", diff_at);
            }
        }
        //hideleg
        gem5_val = readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_HIDELEG, tid);
        diffAllStates->gem5RegFile.hideleg = gem5_val;
        ref_val = diffAllStates->referenceRegFile.hideleg;
        if ((gem5_val != ref_val)) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HIDELEG, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hideleg, diffAllStates->gem5RegFile.hideleg, seq,
                               "hideleg", diff_at);
            }
        }
        // hedeleg
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HEDELEG, tid);
        diffAllStates->gem5RegFile.hedeleg = gem5_val;
        ref_val = diffAllStates->referenceRegFile.hedeleg;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HEDELEG, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hedeleg, diffAllStates->gem5RegFile.hedeleg, seq,
                               "hedeleg", diff_at);
            }
        }
        //hcounteren
        gem5_val = readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HCOUNTEREN, tid);
        diffAllStates->gem5RegFile.hcounteren = gem5_val;
        ref_val = diffAllStates->referenceRegFile.hcounteren;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HCOUNTEREN, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hcounteren, diffAllStates->gem5RegFile.hcounteren, seq,
                               "hcounteren", diff_at);
            }
        }

        //htval
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTVAL, tid);
        diffAllStates->gem5RegFile.htval = gem5_val;
        ref_val = diffAllStates->referenceRegFile.htval;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HTVAL, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::htval, diffAllStates->gem5RegFile.htval, seq, "htval",
                               diff_at);
            }
        }

        // htinst
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTINST, tid);
        diffAllStates->gem5RegFile.htinst = gem5_val;
        ref_val = diffAllStates->referenceRegFile.htinst;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HTINST, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::htinst, diffAllStates->gem5RegFile.htinst, seq,
                               "htinst", diff_at);
            }
        }
        // hgatp
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HGATP, tid);
        diffAllStates->gem5RegFile.hgatp = gem5_val;
        ref_val = diffAllStates->referenceRegFile.hgatp;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HGATP, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hgatp, diffAllStates->gem5RegFile.hgatp, seq, "hgatp",
                               diff_at);
            }
        }
        // vsstatus
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, tid);
        diffAllStates->gem5RegFile.vsstatus = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vsstatus;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsstatus, diffAllStates->gem5RegFile.vsstatus, seq,
                               "vsstatus", diff_at);
            }
        }
        // vstvec
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVEC, tid);
        diffAllStates->gem5RegFile.vstvec = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vstvec;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSTVEC, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vstvec, diffAllStates->gem5RegFile.vstvec, seq,
                               "vstvec", diff_at);
            }
        }
        // vsepc
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSEPC, tid);
        diffAllStates->gem5RegFile.vsepc = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vsepc;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSEPC, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsepc, diffAllStates->gem5RegFile.vsepc, seq, "vsepc",
                               diff_at);
            }
        }
        // vscause
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSCAUSE, tid);
        diffAllStates->gem5RegFile.vscause = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vscause;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSCAUSE, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vscause, diffAllStates->gem5RegFile.vscause, seq,
                               "vscause", diff_at);
            }
        }
        // vstval
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVAL, tid);
        diffAllStates->gem5RegFile.vstval = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vstval;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSTVAL, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vstval, diffAllStates->gem5RegFile.vstval, seq,
                               "vstval", diff_at);
            }
        }

        // vsatp
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSATP, tid);
        diffAllStates->gem5RegFile.vsatp = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vsatp;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSATP, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsatp, diffAllStates->gem5RegFile.vsatp, seq, "vsatp",
                               diff_at);
            }
        }

        // vsscratch
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSSCRATCH, tid);
        diffAllStates->gem5RegFile.vsscratch = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vsscratch;
        if (gem5_val != ref_val) {
            if (enable_csrnemu) {
                setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSCRATCH, ref_val, tid);
            } else {
                csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsscratch, diffAllStates->gem5RegFile.vsscratch, seq,
                               "vsscratch", diff_at);
            }
        }
        // cpu.v diff
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VIRMODE, tid);
        diffAllStates->gem5RegFile.v = gem5_val;
        ref_val = diffAllStates->referenceRegFile.v;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::v, diffAllStates->gem5RegFile.v, seq, "v", diff_at);
        }
    }



        if (diff_at != NoneDiff) {
            DPRINTF(Diff, "Inst [sn:%llu] @ \033[31m%#lx\033[0m in GEM5 is \033[31m%s\033[0m\n", seq,
                    diffInfo.pc->instAddr(),
                    diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
        }


    for (int dest_idx = 0; dest_idx < diffInfo.inst->numDestRegs(); dest_idx++) {

        const auto &dest = diffInfo.inst->destRegIdx(dest_idx);
        auto dest_tag = dest.index() + dest.isFloatReg() * 32;

        if ((dest.isFloatReg() || dest.isIntReg()) && !dest.isZeroReg()) {
            auto gem5_val = diffInfo.scalarResults[dest_idx];
            auto nemu_val = diffAllStates->referenceRegFile[dest_tag];
            DPRINTF(Diff, "At %s Ref value: %#lx, GEM5 value: %#lx\n",
                    reg_name[dest_tag], nemu_val, gem5_val);

            if (gem5_val != nemu_val) {
                if (diffInfo.inst->isMemRef()) {
                    diffMsg << csprintf("%s addr: %#lx, size: %u\n",
                                        diffInfo.inst->isAtomic() ? "AMO"
                                        : diffInfo.inst->isLoad() ? "Load"
                                                                  : "Store",
                                        diffInfo.physEffAddr, diffInfo.effSize);
                }

                if (system->multiContextDifftest() &&
                    (diffInfo.inst->isLoad() || diffInfo.inst->isAtomic()) &&
                    _goldenMemManager->inPmem(diffInfo.physEffAddr)) {
                    DPRINTF(Diff,
                            "Difference on %s instr found in multicore mode, "
                            "check in golden memory\n",
                            diffInfo.inst->isLoad() ? "load" : "amo");
                    uint8_t current_golden_data[16] = {};
                    panic_if(diffInfo.effSize > sizeof(current_golden_data),
                             "Unexpected large mem diff size: %u\n",
                             diffInfo.effSize);
                    _goldenMemManager->readGoldenMem(diffInfo.physEffAddr,
                                                     current_golden_data,
                                                     diffInfo.effSize);
                    uint8_t *golden_ptr = current_golden_data;
                    uint8_t *exec_golden_ptr = diffInfo.goldenValue;
                    const RecentCommittedStore *matched_recent_store = nullptr;
                    if (diffInfo.inst->isLoad()) {
                        const auto &recent_history =
                            recentCommittedStores.at(tid);
                        for (auto it = recent_history.rbegin();
                             it != recent_history.rend(); ++it) {
                            if (!it->valid ||
                                it->addr != diffInfo.physEffAddr ||
                                it->size != diffInfo.effSize ||
                                it->seq >= seq ||
                                (seq - it->seq) > 256) {
                                continue;
                            }
                            if (memcmp(it->data, &gem5_val,
                                       diffInfo.effSize) == 0) {
                                matched_recent_store = &(*it);
                                break;
                            }
                        }
                    }
                    auto sync_reg = [&]() {
                        diffAllStates->referenceRegFile[dest_tag] = gem5_val;
                        diffAllStates->proxy->copyRegsToRef(
                            diffAllStates->referenceRegFile);
                    };

                    // Sync both memory and register when the value is already
                    // globally visible in golden memory.
                    auto sync_mem_reg = [&](const uint8_t *mem_src) {
                        diffAllStates->proxy->memcpy(diffInfo.physEffAddr,
                                                     const_cast<uint8_t *>(mem_src),
                                                     diffInfo.effSize,
                                                     DIFFTEST_TO_REF);
                        sync_reg();
                    };

                    if (diffInfo.inst->isLoad() &&
                               memcmp(golden_ptr, &gem5_val,
                                      diffInfo.effSize) == 0) {
                        DPRINTF(Diff,
                                "Load content matched in golden memory. "
                                "Sync from golden to ref\n");
                        sync_mem_reg(golden_ptr);
                        continue;
                    } else if (diffInfo.inst->isLoad() && exec_golden_ptr &&
                               memcmp(exec_golden_ptr, &gem5_val,
                                      diffInfo.effSize) == 0) {
                        DPRINTF(Diff,
                                "Load content matched the execution-time "
                                "golden snapshot. Sync from the recorded "
                                "snapshot to ref\n");
                        sync_mem_reg(exec_golden_ptr);
                        continue;
                    } else if (matched_recent_store) {
                        DPRINTF(Diff,
                                "Load content matched recent committed store "
                                "[sn:%llu] at addr %#lx. Syncing ref from the "
                                "store snapshot for this hart.\n",
                                matched_recent_store->seq,
                                diffInfo.physEffAddr);
                        sync_mem_reg(matched_recent_store->data);
                        continue;
                    } else if (diffInfo.inst->isAtomic()) {
                        DPRINTF(Diff, "Golden mem old value: %#lx, GEM5 old value: %#lx\n", diffInfo.amoOldGoldenValue,
                                gem5_val);
                        DPRINTF(Diff, "New golden value: %#lx\n", *(uint64_t *)golden_ptr);
                        if (memcmp(&diffInfo.amoOldGoldenValue, &gem5_val,
                                   diffInfo.effSize) == 0) {
                            DPRINTF(Diff, "Atomic encountered, old value matched. Sync from golden to ref\n");
                            sync_mem_reg(golden_ptr);
                            continue;
                        }
                    } else if (diffInfo.inst->isLoad()) {
                        DPRINTF(Diff,
                                "Unresolved shared-memory load mismatch at "
                                "addr=%#lx gem5=%#lx current_golden=%#lx "
                                "exec_snapshot=%#lx; falling back to normal "
                                "difftest reporting.\n",
                                diffInfo.physEffAddr, gem5_val,
                                *(uint64_t *)golden_ptr,
                                exec_golden_ptr ?
                                    *(uint64_t *)exec_golden_ptr : 0);
                    }
                }

                if (dest.isFloatReg() && (gem5_val ^ nemu_val) == ((0xffffffffULL) << 32)) {
                    DPRINTF(Diff,
                            "Difference might be caused by box,"
                            " ignore it\n");
                } else {
                    bool skipCSR = false;
                    for (auto iter : skipCSRs) {
                        if ((machInst & 0xfff00073) == iter) {
                            skipCSR = true;
                            DPRINTF(Diff, "This is an csr instruction, skip!\n");
                            diffAllStates->referenceRegFile[dest_tag] = gem5_val;
                            diffAllStates->proxy->copyRegsToRef(
                                diffAllStates->referenceRegFile);
                            break;
                        }
                    }
                    if (!diff_at && !skipCSR) {
                        diffMsg << csprintf("Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
                        diffMsg << csprintf(
                            "Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, "
                            "GEM5 value: \033[31m%#lx\033[0m\n",
                            reg_name[dest_tag], nemu_val, gem5_val);
                        diffInfo.errorRegsValue[dest_tag] = 1;
                        if (dest_tag < 32)
                            diffAllStates->gem5RegFile.gpr[dest_tag]._64 = gem5_val;
                        else if (dest_tag >= 32 && dest_tag < 64)
                            diffAllStates->gem5RegFile.fpr[dest_tag - 32]._64 = gem5_val;

                        diffAllStates->gem5RegFile.pc = gem5_pc;

                        diff_at = ValueDiff;
                    }
                }
            }
        }
    }
    if (diff_at) {
        diffMsg << csprintf("In CPU%d: NEMU PC: %#10lx, GEM5 PC: %#10lx, inst: %s\n", cpuId(),
        nemu_pc, gem5_pc,
        diffInfo.inst->disassemble(diffInfo.pc->instAddr()).c_str());
    }
    return std::make_pair(diff_at, npc_match);
}

void
BaseCPU::clearDiffMismatch(ThreadID tid, InstSeqNum seq) {
    diffMsg.str(std::string());
    memset(diffInfo.errorRegsValue, 0, sizeof(diffInfo.errorRegsValue));
    memset(diffInfo.errorCsrsValue, 0, sizeof(diffInfo.errorCsrsValue));
    diffInfo.errorPcValue = 0;
}

void
BaseCPU::reportDiffMismatch(ThreadID tid, InstSeqNum seq)
{
    auto diffAllStates = this->diffAllStates[tid];
    warn("%s", diffMsg.str());
    diffAllStates->proxy->isa_reg_display();
    displayGem5Regs(tid);
    warn("start dump last %lu committed msg\n", diffInfo.lastCommittedMsg.size());
    while (diffInfo.lastCommittedMsg.size()) {
        auto &inst = diffInfo.lastCommittedMsg.front();
        warn("V %s\n", inst->genDisassembly());
        diffInfo.lastCommittedMsg.pop();
    }
}

void
BaseCPU::ensure_difftest_reference(ThreadID tid, Addr event_pc)
{
    auto diffAllStates = this->diffAllStates[tid];
    if (diffAllStates->referenceInitialized) {
        return;
    }
    fatal_if(!diffAllStates->initialStateCaptured,
             "Difftest initial state was not captured for tid %d", tid);
    auto &initial_state = diffAllStates->initialDutState;

    fatal_if(initial_state.pc != event_pc,
             "Difftest initial state PC %#lx does not match first "
             "architectural event PC %#lx for tid %d",
             initial_state.pc, event_pc, tid);

    diffAllStates->diff.nemu_this_pc = initial_state.pc;
    if (noHypeMode) {
        auto start = pmemStart + pmemSize * difftestHartId(tid);
        diffAllStates->proxy->memcpy(
            0x80000000u, start, pmemSize, DIFFTEST_TO_REF);
    } else if (enableMemDedup) {
        assert(diffAllStates->proxy->ref_get_backed_memory);
        if (system->multiContextDifftest()) {
            assert(goldenMemPtr);
        }
        diffAllStates->proxy->ref_get_backed_memory(
            system->createCopyOnWriteBranch(), pmemSize);
    } else {
        diffAllStates->proxy->memcpy_init(
            0x80000000u, pmemStart, pmemSize, DIFFTEST_TO_REF);
    }
    diffAllStates->proxy->copyRegsToRef(initial_state);
    // Read the canonical REF view after NEMU applies CSR writeback.
    diffAllStates->proxy->copyRegsFromRef(
        diffAllStates->referenceRegFile);
    diffAllStates->referenceInitialized = true;
    DPRINTF(Diff, "Initialized REF for tid %d at event PC %#lx\n",
            tid, event_pc);
}

void
BaseCPU::difftestStep(ThreadID tid, InstSeqNum seq)
{
    DPRINTF(DumpCommit, "[sn:%llu] %#lx, %s\n",
            seq, diffInfo.pc->instAddr(), diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
    DPRINTF(Diff, "DiffTest step on inst pc: %#lx: %s\n",
            diffInfo.pc->instAddr(),
            diffInfo.inst->disassemble(diffInfo.pc->instAddr()));

    bool is_fence = diffInfo.inst->isReadBarrier() || diffInfo.inst->isWriteBarrier();
    bool fence_should_diff = is_fence && !diffInfo.inst->isMicroop();
    bool lr_should_diff = diffInfo.inst->isLoadReserved();
    bool amo_should_diff = diffInfo.inst->isAtomic() && diffInfo.inst->numDestRegs() > 0;
    bool is_sc = diffInfo.inst->isStoreConditional() && diffInfo.inst->isDelayedCommit();
    bool other_should_diff = !diffInfo.inst->isAtomic() && !is_fence && !is_sc &&
                             (!diffInfo.inst->isMicroop() || diffInfo.inst->isLastMicroop());

    const bool should_diff = fence_should_diff || amo_should_diff || is_sc ||
                             other_should_diff || lr_should_diff;

    if (enableDifftest && should_diff) {
        ensure_difftest_reference(tid, diffInfo.pc->instAddr());
        auto [diff_at, npc_match] = diffWithNEMU(tid, seq);
        if (diff_at != NoneDiff) {
            if (npc_match && diff_at == PCDiff) {
                // warn("Found PC mismatch, Let NEMU run one more
                // instruction\n");
                std::tie(diff_at, npc_match) = diffWithNEMU(tid, 0);
                if (diff_at != NoneDiff) {
                    reportDiffMismatch(tid, seq);
                    panic("Difftest failed again!\n");

                } else {
                    clearDiffMismatch(tid, seq);
                    DPRINTF(Diff,
                            "Difftest matched again, "
                            "NEMU seems to commit the failed mem instruction\n");
                }
            } else {
                reportDiffMismatch(tid, seq);
                panic("Difftest failed!\n");
            }
        } else {
            clearDiffMismatch(tid, seq);
        }
    }
    committedInstNum++;
    if (dumpCommitFlag && committedInstNum >= dumpStartNum) {
        committedInsts.push_back(
            std::make_pair(diffInfo.pc->instAddr(), diffInfo.inst->disassemble(diffInfo.pc->instAddr()).c_str()));
    }
}

void
BaseCPU::displayGem5Regs(ThreadID tid)
{
    auto diffAllStates = this->diffAllStates[tid];
    readDutRegs(tid, diffAllStates->gem5RegFile);
    std::string str;
    //reg
    for (size_t i = 0; i < 32; i++)
    {
        if ( diffInfo.errorRegsValue[i] )
            str += csprintf("\033[31m%04s : %16lx \033[0m",reg_name[i]  ,diffAllStates->gem5RegFile.gpr[i]._64);
        else
            str += csprintf("%04s : %16lx ",reg_name[i]  ,diffAllStates->gem5RegFile.gpr[i]._64);

        if (i%4 == 3) str += csprintf("\n");
    }
    warn("gem5-rRegsDisplay : \n%s",str) ;
    str.clear();

    //fp
    for (size_t i = 0; i < 32; i++)
    {
        if ( diffInfo.errorRegsValue[i+32] )
            str += csprintf("\033[31m%04s : %16lx \033[0m",reg_name[i+32]  ,diffAllStates->gem5RegFile.fpr[i]._64);
        else
            str += csprintf("%04s : %16lx ",reg_name[i+32]  ,diffAllStates->gem5RegFile.fpr[i]._64);

        if (i%4 == 3) str += csprintf("\n");
    }
    warn("gem5-fRegsDisplay : \n%s",str);
    str.clear();

    //csr
    str += csprintf("pc : %16lx      ",diffAllStates->gem5RegFile.pc);
    if (diffInfo.errorCsrsValue[CsrRegIndex::mstatus])
        str += csprintf("\033[31mmstatus : %16lx\033[0m", diffAllStates->gem5RegFile.mstatus);
    else
        str += csprintf("mstatus : %16lx", diffAllStates->gem5RegFile.mstatus);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mcause])
        str += csprintf(" \033[31mmcause : %16lx\033[0m ", diffAllStates->gem5RegFile.mcause);
    else
        str += csprintf(" mcause : %16lx ", diffAllStates->gem5RegFile.mcause);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mepc])
        str += csprintf("\033[31mmepc    : %16lx\033[0m\n", diffAllStates->gem5RegFile.mepc);
    else
        str += csprintf("mepc    : %16lx\n", diffAllStates->gem5RegFile.mepc);

    str += csprintf("\t\t\t   sstatus : %16lx scause : %16lx",
        diffAllStates->gem5RegFile.sstatus,diffAllStates->gem5RegFile.scause);
    str += csprintf(" sepc    : %16lx\n", diffAllStates->gem5RegFile.sepc);

    if (diffInfo.errorCsrsValue[CsrRegIndex::satp])
        str += csprintf("\033[31msatp    : %16lx\033[0m\n", diffAllStates->gem5RegFile.satp);
    else
        str += csprintf("satp    : %16lx\n", diffAllStates->gem5RegFile.satp);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mip])
        str += csprintf("\033[31mmip     : %16lx\033[0m", diffAllStates->gem5RegFile.mip);
    else
        str += csprintf("mip     : %16lx", diffAllStates->gem5RegFile.mip);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mie])
        str += csprintf(" \033[31mmie\033[0m     : %16lx ", diffAllStates->gem5RegFile.mie);
    else {
        str += csprintf(" mie     : %16lx ", diffAllStates->gem5RegFile.mie);
    }


    str += csprintf("mscratch: %16lx sscratch: %16lx\n",
        diffAllStates->gem5RegFile.mtval, diffAllStates->gem5RegFile.stval);
    str += csprintf("mideleg : %16lx medeleg : %16lx\n",
                diffAllStates->gem5RegFile.mideleg, diffAllStates->gem5RegFile.medeleg);

    str += csprintf("mtval   : %16lx stval   : %16lx ",
        diffAllStates->gem5RegFile.mtval, diffAllStates->gem5RegFile.stval);
    str += csprintf("mtvec   : %16lx stvec   : %16lx\n",
        diffAllStates->gem5RegFile.mtvec,diffAllStates->gem5RegFile.stvec);
    str += csprintf("privilege mode : %x\n", diffAllStates->gem5RegFile.mode);
    warn("gem5-CsrDisplay : \n%s",str) ;
    str.clear();

    //vector
    for (size_t i = 0; i < 32; i++)
    {
        str += csprintf("v%02d : ", i);
        for (int j=RiscvISA::NumVecElemPerVecReg-1; j>=0; j--) {
            str +=csprintf("%016lx", diffAllStates->gem5RegFile.vr[i]._64[j]);
            if (j != 0) {
                str+="_";
            }
            else {
                str+="\t";
            }
        }
        if (i%2 == 1)
            str +=csprintf("\n");
    }
    str += csprintf("vtype   : %16lx vstart   : %16lx  ",
                diffAllStates->gem5RegFile.vtype,diffAllStates->gem5RegFile.vstart);
    str += csprintf("vxsat   : %16lx\n", diffAllStates->gem5RegFile.vxsat);
    str += csprintf("vxrm    : %16lx vl       : %16lx  ",
                diffAllStates->gem5RegFile.vxrm,diffAllStates->gem5RegFile.vl);
    str += csprintf("vcsr    : %16lx\n", diffAllStates->gem5RegFile.vcsr);
    warn("gem5-VectorDisplay : \n%s\n\n", str);
    str.clear();

}

void
BaseCPU::difftestRaiseIntr(uint64_t no, ThreadID tid)
{
    auto diffAllStates = this->diffAllStates[tid];
    ensure_difftest_reference(tid, threadContexts[tid]->pcState().instAddr());
    diffAllStates->diff.will_handle_intr = true;
    diffAllStates->proxy->raise_intr(no);
}

void
BaseCPU::clearGuideExecInfo()
{
    for (auto &diffAllStates : this->diffAllStates) {
        diffAllStates->diff.guide.force_raise_exception = false;
        diffAllStates->diff.guide.force_set_jump_target = false;
    }
}

void
BaseCPU::enableDiffPrint()
{
    for (auto &diffAllStates : this->diffAllStates) {
        diffAllStates->diff.dynamic_config.debug_difftest = true;
        diffAllStates->proxy->update_config(&diffAllStates->diff.dynamic_config);
    }
}

void BaseCPU::setSCSuccess(bool success, paddr_t addr, ThreadID tid)
{
    auto diffAllStates = this->diffAllStates[tid];
    diffAllStates->diff.sync.lrscValid = success;
    diffAllStates->diff.sync.lrscAddr = addr; // used for spike diff
}

void
BaseCPU::setExceptionGuideExecInfo(uint64_t exception_num, uint64_t mtval, uint64_t stval, bool force_set_jump_target,
                                   uint64_t jump_target, ThreadID tid)
{
    auto diffAllStates = this->diffAllStates[tid];

    assert(diffAllStates->referenceInitialized);
    auto &gd = diffAllStates->diff.guide;
    gd.force_raise_exception = true;
    gd.exception_num = exception_num;
    gd.mtval = mtval;
    gd.stval = stval;
    gd.mtval2 = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL2, tid);
    gd.htval = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTVAL, tid);
    gd.vstval = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVAL, tid);
    gd.force_set_jump_target = force_set_jump_target;
    gd.jump_target = jump_target;

    diffAllStates->proxy->guided_exec(&(diffAllStates->diff.guide));

    diffAllStates->proxy->copyRegsFromRef(
        diffAllStates->referenceRegFile);
    diffAllStates->diff.nemu_this_pc =
        diffAllStates->referenceRegFile.pc;
    DPRINTF(Diff, "After guided exec on NEMU, new PC: %#lx\n", diffAllStates->diff.nemu_this_pc);
}

void
BaseCPU::checkL1DRefill(Addr paddr, const uint8_t* refill_data, size_t size) {
    assert(size == 64);
    if (system->multiContextDifftest()) {
        uint8_t *golden_ptr = (uint8_t *)_goldenMemManager->guestToHost(paddr);
        if (memcmp(golden_ptr, refill_data, size)) {
            panic("Refill data diff with Golden addr %#lx with size %d\n", paddr, size);
        }
    }
}
} // namespace gem5

/*
 * Copyright (c) 2016 RISC-V Foundation
 * Copyright (c) 2016 The University of Virginia
 * Copyright (c) 2018 TU Dresden
 * Copyright (c) 2020 Barkhausen Institut
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

#include "arch/riscv/faults.hh"

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/Faults.hh"
#include "sim/debug.hh"
#include "sim/full_system.hh"
#include "sim/workload.hh"

namespace gem5
{

namespace RiscvISA
{

void
RiscvFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    panic("Fault %s encountered at pc %s.", name(), tc->pcState());
}

void
RiscvFault::invoke(ThreadContext *tc, const StaticInstPtr &inst)
{
    auto pc_state = tc->pcState().as<PCState>();

    DPRINTFS(Faults, tc->getCpuPtr(), "Fault (%s) at PC: %s\n",
             name(), pc_state);

    if (FullSystem) {
        PrivilegeMode pp = (PrivilegeMode)tc->readMiscReg(MISCREG_PRV);
        PrivilegeMode prv = PRV_M;
        PrivilegeMode prvh = PRV_M;
        STATUS status = tc->readMiscReg(MISCREG_STATUS);
        HSTATUS hstatus = tc->readMiscReg(MISCREG_HSTATUS);
        VSSTATUS vsstatus = tc->readMiscReg(MISCREG_VSSTATUS);
        auto vmode = tc->readMiscReg(MISCREG_VIRMODE);
        bool hs_mode_pc = false;
        if ((_code == ECALL_SUPER)  && vmode){
            _code = ECALL_VS;
        }

        // According to riscv-privileged-v1.11, if a NMI occurs at the middle
        // of a M-mode trap handler, the state (epc/cause) will be overwritten
        // and is not necessary recoverable. There's nothing we can do here so
        // we'll just warn our user that the CPU state might be broken.
        warn_if(isNonMaskableInterrupt() && pp == PRV_M && status.mie == 0,
                "NMI overwriting M-mode trap handler state");

        // Set fault handler privilege mode
        if (isNonMaskableInterrupt()) {
            prv = PRV_M;
        } else if (isInterrupt()) {
            if (pp != PRV_M &&
                bits(tc->readMiscReg(MISCREG_MIDELEG), _code) != 0) {
                prv = PRV_S;
            }
            if (pp == PRV_U &&
                bits(tc->readMiscReg(MISCREG_SIDELEG), _code) != 0) {
                prv = PRV_U;
            }
        } else {
            if (pp != PRV_M &&
                bits(tc->readMiscReg(MISCREG_MEDELEG), _code) != 0) {
                prv = PRV_S;
            }
            if (pp == PRV_U &&
                bits(tc->readMiscReg(MISCREG_SEDELEG), _code) != 0) {
                prv = PRV_U;
            }
        }

        if (isInterrupt()) {
            if (pp != PRV_M && (tc->readMiscReg(MISCREG_VIRMODE) == 1) &&
                bits(tc->readMiscReg(MISCREG_HIDELEG), _code) != 0) {
                prvh = PRV_HS;
            }
        } else {
            if (pp != PRV_M && (tc->readMiscReg(MISCREG_VIRMODE) == 1) &&
                bits(tc->readMiscReg(MISCREG_HEDELEG), _code) != 0) {
                prvh = PRV_HS;
            }
        }

        // Set fault registers and status
        MiscRegIndex cause, epc, tvec, tval;
        int v = status.mprv ? status.mpv : tc->readMiscReg(MISCREG_VIRMODE);
        switch (prv) {
          case PRV_U:
            cause = MISCREG_UCAUSE;
            epc = MISCREG_UEPC;
            tvec = MISCREG_UTVEC;
            tval = MISCREG_UTVAL;

            status.upie = status.uie;
            status.uie = 0;
            break;
          case PRV_S:
              if (prvh == PRV_HS) {
                  cause = MISCREG_SCAUSE;
                  epc = MISCREG_SEPC;
                  tvec = MISCREG_STVEC;
                  //tval = MISCREG_STVAL;
                  tval = MISCREG_VSTVAL;

                  if (hstatus.vsxl == 1) {
                      assert(0);

                  } else {
                      vsstatus.spp = pp;
                      vsstatus.spie = vsstatus.sie;
                      vsstatus.sie = 0;
                  }

                  tc->setMiscReg(MISCREG_VIRMODE, 1);
                  if (isInterrupt())
                      tc->setMiscReg(MISCREG_VSCAUSE, _code | (1L << 63));
                  else
                      tc->setMiscReg(MISCREG_VSCAUSE, _code);
                  tc->setMiscReg(MISCREG_VSEPC, tc->pcState().instAddr());
                  hs_mode_pc = true;
              } else {
                  cause = MISCREG_SCAUSE;
                  epc = MISCREG_SEPC;
                  tvec = MISCREG_STVEC;
                  tval = MISCREG_STVAL;

                  status.spp = pp;
                  status.spie = status.sie;
                  status.sie = 0;
                  hstatus.gva =
                      (!isInterrupt()) && (_code == INST_G_PAGE || _code == LOAD_G_PAGE || _code == STORE_G_PAGE ||
                                           ((v) && ((0 <= _code && _code <= 7 && _code != 2) || _code == INST_PAGE ||
                                                    _code == LOAD_PAGE || _code == STORE_PAGE)));
                  hstatus.spv = v;
                  if (v) {
                      hstatus.spvp = pp;
                  }
                  if ((_code == INST_PAGE || _code == LOAD_PAGE || _code == STORE_PAGE ||
                       _code == LOAD_ADDR_MISALIGNED || _code == STORE_ADDR_MISALIGNED || _code == INST_ACCESS ||
                       _code == LOAD_ACCESS || _code == STORE_ACCESS) ||
                      ((_code != INST_G_PAGE) && (_code != LOAD_G_PAGE) && (_code != STORE_G_PAGE))) {
                      tc->setMiscReg(MISCREG_HTVAL, 0);
                  }

                  tc->setMiscReg(MISCREG_VIRMODE, 0);
              }
            break;
          case PRV_M:
            cause = MISCREG_MCAUSE;
            epc = MISCREG_MEPC;
            tvec = isNonMaskableInterrupt() ? MISCREG_NMIVEC : MISCREG_MTVEC;
            tval = MISCREG_MTVAL;

            status.mpp = pp;
            status.mpie = status.mie;
            status.mie = 0;
            status.gva =
                (!isInterrupt()) && (_code == INST_G_PAGE || _code == LOAD_G_PAGE || _code == STORE_G_PAGE ||
                                     ((v) && ((0 <= _code && _code <= 7 && _code != 2) || _code == INST_PAGE ||
                                              _code == LOAD_PAGE || _code == STORE_PAGE)));
            status.mpv = v;
            tc->setMiscReg(MISCREG_VIRMODE, 0);
            break;
          default:
            panic("Unknown privilege mode %d.", prv);
            break;
        }

        // Set fault cause, privilege, and return PC
        // Interrupt is indicated on the MSB of cause (bit 63 in RV64)
        uint64_t _cause = _code;
        if (isInterrupt()) {
           _cause |= (1L << 63);
        }

        if (!hs_mode_pc) {
            tc->setMiscReg(cause, _cause);
            tc->setMiscReg(epc, tc->pcState().instAddr());
        }

        if (_cause == INST_ILLEGAL)
            tc->setMiscReg(tval, 0);
        else
            tc->setMiscReg(tval, trap_value());
        if (_code == INST_G_PAGE || _code == LOAD_G_PAGE || _code == STORE_G_PAGE) {
            if (prv == PRV_S && (g_trap_value() != 0)) {
                tc->setMiscReg(MISCREG_HTVAL, g_trap_value() >> 2);
            } else if (g_trap_value() != 0) {
                tc->setMiscReg(MISCREG_MTVAL2, g_trap_value() >> 2);
            }
        }

        tc->setMiscReg(MISCREG_PRV, prv);
        tc->setMiscReg(MISCREG_STATUS, status);
        tc->setMiscReg(MISCREG_HSTATUS, hstatus);
        tc->setMiscRegNoEffect(MISCREG_VSSTATUS, vsstatus);
        // Temporarily mask NMI while we're in NMI handler. Otherweise, the
        // checkNonMaskableInterrupt will always return true and we'll be
        // stucked in an infinite loop.
        if (isNonMaskableInterrupt()) {
            tc->setMiscReg(MISCREG_NMIE, 0);
        }

        // Set PC to fault handler address
        Addr addr = 0;
        auto addr_tvec = tc->readMiscReg(tvec);
        if (!hs_mode_pc) {
            addr = mbits(tc->readMiscReg(tvec), 63, 2);
        } else {
            addr = mbits(tc->readMiscReg(MISCREG_VSTVEC), 63, 2);
            addr_tvec = tc->readMiscReg(MISCREG_VSTVEC);
        }

        if (isInterrupt() && bits(addr_tvec, 1, 0) == 1)
            addr += 4 * _code;
        pc_state.set(addr);
        tc->pcState(pc_state);
    } else {
        inst->advancePC(pc_state);
        tc->pcState(pc_state);
        invokeSE(tc, inst);
    }
}

void
Reset::invoke(ThreadContext *tc, const StaticInstPtr &inst)
{
    tc->setMiscReg(MISCREG_PRV, PRV_M);
    STATUS status = tc->readMiscReg(MISCREG_STATUS);
    status.mie = 0;
    status.mprv = 0;
    tc->setMiscReg(MISCREG_STATUS, status);
    tc->setMiscReg(MISCREG_MCAUSE, 0);

    // Advance the PC to the implementation-defined reset vector
    auto workload = dynamic_cast<Workload *>(tc->getSystemPtr()->workload);
    PCState pc(workload->getEntry());
    tc->pcState(pc);
}

void
UnknownInstFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    auto *rsi = static_cast<RiscvStaticInst *>(inst.get());
    panic("Unknown instruction 0x%08x at pc %s", rsi->machInst,
        tc->pcState());
}

void
IllegalInstFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    auto *rsi = static_cast<RiscvStaticInst *>(inst.get());
    panic("Illegal instruction 0x%08x at pc %s: %s", rsi->machInst,
        tc->pcState(), reason.c_str());
}

void
UnimplementedFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    panic("Unimplemented instruction %s at pc %s", instName, tc->pcState());
}

void
IllegalFrmFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    panic("Illegal floating-point rounding mode 0x%x at pc %s.",
            frm, tc->pcState());
}

void
BreakpointFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    schedRelBreak(0);
}

void
SyscallFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    tc->getSystemPtr()->workload->syscall(tc);
}

void
HVFault::invokeSE(ThreadContext *tc, const StaticInstPtr &inst)
{
    panic("HVFault at pc %s", tc->pcState());
}

} // namespace RiscvISA
} // namespace gem5

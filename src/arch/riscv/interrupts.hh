/*
 * Copyright (c) 2011 Google
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

#ifndef __ARCH_RISCV_INTERRUPT_HH__
#define __ARCH_RISCV_INTERRUPT_HH__

#include <bitset>
#include <memory>

#include "arch/generic/interrupts.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/logging.hh"
#include "cpu/thread_context.hh"
#include "debug/Interrupt.hh"
#include "params/RiscvInterrupts.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class BaseCPU;
class ThreadContext;

namespace RiscvISA {

/*
 * This is based on version 1.10 of the RISC-V privileged ISA reference,
 * chapter 3.1.14.
 */
class Interrupts : public BaseInterrupts
{
  private:
    std::bitset<NumInterruptTypes> ip;
    std::bitset<NumInterruptTypes> ie;

  public:
    using Params = RiscvInterruptsParams;

    Interrupts(const Params &p) : BaseInterrupts(p), ip(0), ie(0) {}

    std::bitset<NumInterruptTypes>
    globalMask() const
    {
        INTERRUPT mask = 0;
        STATUS status = tc->readMiscReg(MISCREG_STATUS);
        INTERRUPT mideleg = tc->readMiscReg(MISCREG_MIDELEG);
        INTERRUPT sideleg = tc->readMiscReg(MISCREG_SIDELEG);
        PrivilegeMode prv = (PrivilegeMode)tc->readMiscReg(MISCREG_PRV);
        switch (prv) {
            case PRV_U:
                mask.mei = (!sideleg.mei) | (sideleg.mei & status.uie);
                mask.mti = (!sideleg.mti) | (sideleg.mti & status.uie);
                mask.msi = (!sideleg.msi) | (sideleg.msi & status.uie);
                mask.sei = (!sideleg.sei) | (sideleg.sei & status.uie);
                mask.sti = (!sideleg.sti) | (sideleg.sti & status.uie);
                mask.ssi = (!sideleg.ssi) | (sideleg.ssi & status.uie);
                if (status.uie)
                    mask.uei = mask.uti = mask.usi = 1;
                break;
            case PRV_S:
                mask.mei = (!mideleg.mei) | (mideleg.mei & status.sie);
                mask.mti = (!mideleg.mti) | (mideleg.mti & status.sie);
                mask.msi = (!mideleg.msi) | (mideleg.msi & status.sie);
                if (status.sie)
                    mask.sei = mask.sti = mask.ssi = 1;
                mask.uei = mask.uti = mask.usi = 0;
                break;
            case PRV_M:
                if (status.mie)
                     mask.mei = mask.mti = mask.msi = 1;
                mask.sei = mask.sti = mask.ssi = 0;
                mask.uei = mask.uti = mask.usi = 0;
                break;
            default:
                panic("Unknown privilege mode %d.", prv);
                break;
        }

        return std::bitset<NumInterruptTypes>(mask);
    }

    bool
    checkNonMaskableInterrupt() const
    {
        return tc->readMiscReg(MISCREG_NMIP) & tc->readMiscReg(MISCREG_NMIE);
    }

    bool checkInterrupt(int num) const { return ip[num] && ie[num]; }
    bool checkInterrupts() const
    {
        return checkNonMaskableInterrupt() || (ip & ie & globalMask()).any();
    }

    Fault
    getInterrupt()
    {
        assert(checkInterrupts());
        if (checkNonMaskableInterrupt())
            return std::make_shared<NonMaskableInterruptFault>();
        std::bitset<NumInterruptTypes> mask = globalMask();
        const std::vector<int> interrupt_order {
            INT_EXT_MACHINE, INT_TIMER_MACHINE, INT_SOFTWARE_MACHINE,
            INT_EXT_SUPER, INT_TIMER_SUPER, INT_SOFTWARE_SUPER,
            INT_EXT_USER, INT_TIMER_USER, INT_SOFTWARE_USER
        };
        for (const int &id : interrupt_order)
            if (checkInterrupt(id) && mask[id])
                return std::make_shared<InterruptFault>(id);
        return NoFault;
    }

    int getInterruptNO()
    {
        assert(checkInterrupts());
        std::bitset<NumInterruptTypes> mask = globalMask();
        for (int c = 0; c < NumInterruptTypes; c++)
            if (checkInterrupt(c) && mask[c])
                return c;
        return -1;
    }

    void updateIntrInfo() {}

    void
    post(int int_num, int index)
    {
        DPRINTF(Interrupt, "Interrupt %d:%d posted\n", int_num, index);
        ip[int_num] = true;
    }

    void
    clear(int int_num, int index)
    {
        DPRINTF(Interrupt, "Interrupt %d:%d cleared\n", int_num, index);
        ip[int_num] = false;
    }

    void postNMI() { tc->setMiscReg(MISCREG_NMIP, 1); }
    void clearNMI() { tc->setMiscReg(MISCREG_NMIP, 0); }

    void
    clearAll()
    {
        DPRINTF(Interrupt, "All interrupts cleared\n");
        ip = 0;
        clearNMI();
    }

    uint64_t readIP() const { return (uint64_t)ip.to_ulong(); }
    uint64_t readIE() const { return (uint64_t)ie.to_ulong(); }
    void setIP(const uint64_t& val) { ip = val; }
    void setIE(const uint64_t& val) { ie = val; }

    void
    serialize(CheckpointOut &cp) const
    {
        unsigned long ip_ulong = ip.to_ulong();
        unsigned long ie_ulong = ie.to_ulong();
        SERIALIZE_SCALAR(ip_ulong);
        SERIALIZE_SCALAR(ie_ulong);
    }

    void
    unserialize(CheckpointIn &cp)
    {
        unsigned long ip_ulong;
        unsigned long ie_ulong;
        UNSERIALIZE_SCALAR(ip_ulong);
        ip = ip_ulong;
        UNSERIALIZE_SCALAR(ie_ulong);
        ie = ie_ulong;
    }
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INTERRUPT_HH__

/*
 * Copyright (c) 2015 RISC-V Foundation
 * Copyright (c) 2017 The University of Virginia
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

#ifndef __ARCH_RISCV_STANDARD_INST_HH__
#define __ARCH_RISCV_STANDARD_INST_HH__

#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

/**
 * Base class for operations that work only on registers
 */
class RegOp : public RiscvStaticInst
{
  protected:
    using RiscvStaticInst::RiscvStaticInst;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

/**
 * Base class for Zimop operations, whose encoded sources are not dependencies.
 */
class MopOp : public RiscvStaticInst
{
  protected:
    using RiscvStaticInst::RiscvStaticInst;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

/**
 * Base class for operations with immediates (I is the type of immediate)
 */
template<typename I>
class ImmOp : public RiscvStaticInst
{
  protected:
    I imm;

    ImmOp(const char *mnem, ExtMachInst _extMachInst, OpClass __opClass)
        : RiscvStaticInst(mnem, _extMachInst, __opClass), imm(0)
    {}

    int64_t getImm() const override {
        return imm;
    }

  public:
    bool isLldpImmediate() const override { return true; }
};

/**
 * Base class for system operations
 */
class SystemOp : public RiscvStaticInst
{
  protected:
    using RiscvStaticInst::RiscvStaticInst;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

enum class CSRAccessClass { Write, BlockBackward, WaitForward, OutOfOrder };

inline CSRAccessClass
classifyCSRAccess(ExtMachInst machInst)
{
    switch (machInst.funct3) {
      case 0x2: case 0x3: // CSRRS/CSRRC
        if (machInst.rs1 != 0)
            return CSRAccessClass::Write;
        break;
      case 0x6: case 0x7: // CSRRSI/CSRRCI
        if (machInst.csrimm != 0)
            return CSRAccessClass::Write;
        break;
      default: // CSRRW/CSRRWI always write, even with a zero source.
        return CSRAccessClass::Write;
    }

    switch (machInst.funct12) {
      case CSR_MIREG: case CSR_SIREG: case CSR_VSIREG:
      case CSR_MTOPI: case CSR_STOPI: case CSR_VSTOPI:
      case CSR_MTOPEI: case CSR_STOPEI: case CSR_VSTOPEI:
      case CSR_MIP: case CSR_SIP: case CSR_VSIP:
      case CSR_HIP: case CSR_HVIP: case CSR_MVIP:
        return CSRAccessClass::BlockBackward;
      case CSR_FFLAGS: case CSR_FCSR:
      case CSR_VXSAT: case CSR_VCSR: case CSR_VSTART:
      case CSR_SSTATUS: case CSR_VSSTATUS: case CSR_MSTATUS:
      case CSR_HSTATUS: case CSR_MNSTATUS: case CSR_DCSR:
      case CSR_VTYPE:
        return CSRAccessClass::WaitForward;
      default:
        return CSRAccessClass::OutOfOrder;
    }
}

/**
 * Base class for CSR operations
 */
class CSROp : public RiscvStaticInst
{
  protected:
    uint64_t csr;
    uint64_t uimm;

    bool valid = false;
    RegIndex midx = 0;
    std::string csrName;
    uint64_t maskVal = 0;

    /// Constructor
    CSROp(const char *mnem, ExtMachInst _extMachInst, OpClass __opClass)
        : RiscvStaticInst(mnem, _extMachInst, __opClass),
            csr(_extMachInst.funct12), uimm(_extMachInst.csrimm)
    {
        auto csr_data_it = CSRData.find(csr);
        if (csr_data_it == CSRData.end()) {
            valid = false;
        } else {
            valid = true;
            midx = csr_data_it->second.physIndex;
            csrName = csr_data_it->second.name;
            auto mask_it = CSRMasks.find(csr);
            if (mask_it == CSRMasks.end())
                maskVal = mask(64);
            else {
                maskVal = mask_it->second;
            }
        }
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_STANDARD_INST_HH__

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

#include "arch/riscv/insts/standard.hh"

#include <sstream>
#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

std::string
RegOp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        registerName(srcRegIdx(0));
    if (_numSrcRegs >= 2)
        ss << ", " << registerName(srcRegIdx(1));
    if (_numSrcRegs >= 3)
        ss << ", " << registerName(srcRegIdx(2));
    return ss.str();
}

std::string
MopOp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    const bool two_sources = bits(machInst, 25);
    unsigned number = bits(machInst, 30) << 2 | bits(machInst, 27, 26);
    if (!two_sources)
        number = number << 2 | bits(machInst, 21, 20);

    // Print encoded registers without adding them to the operand list.
    std::stringstream ss;
    ss << (two_sources ? "mop.rr." : "mop.r.") << number << ' '
       << registerName(RegId(IntRegClass, machInst.rd)) << ", "
       << registerName(RegId(IntRegClass, machInst.rs1));
    if (two_sources)
        ss << ", " << registerName(RegId(IntRegClass, machInst.rs2));
    return ss.str();
}

CSROp::Ordering
CSROp::ordering() const
{
    // rs1 and zimm share the same five encoded bits.
    switch (machInst.funct3) {
      case 0x2: // CSRRS
      case 0x3: // CSRRC
      case 0x6: // CSRRSI
      case 0x7: // CSRRCI
        if (uimm != 0)
            return Ordering::Serialized;
        break;
      default: // CSRRW and CSRRWI always write.
        return Ordering::Serialized;
    }

    switch (csr) {
      case CSR_MIREG: case CSR_SIREG: case CSR_VSIREG:
      case CSR_MTOPI: case CSR_STOPI: case CSR_VSTOPI:
      case CSR_MTOPEI: case CSR_STOPEI: case CSR_VSTOPEI:
      case CSR_MIP: case CSR_SIP: case CSR_VSIP:
      case CSR_HIP: case CSR_HVIP: case CSR_MVIP:
      // Keep vector length reads on their existing serialized path.
      case CSR_VL: case CSR_VLENB:
        return Ordering::Serialized;
      case CSR_FFLAGS: case CSR_FCSR:
      case CSR_VXSAT: case CSR_VCSR: case CSR_VSTART:
      case CSR_SSTATUS: case CSR_VSSTATUS: case CSR_MSTATUS:
      case CSR_HSTATUS: case CSR_MNSTATUS: case CSR_DCSR: case CSR_VTYPE:
        return Ordering::WaitForOlder;
      default:
        return Ordering::Unordered;
    }
}

std::string
CSROp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    auto data = CSRData.find(csr);
    if (data != CSRData.end())
        ss << data->second.name;
    else
        ss << "?? (" << std::hex << "0x" << csr << std::dec << ")";
    if (_numSrcRegs > 0)
        ss << ", " << registerName(srcRegIdx(0));
    else
        ss << uimm;
    return ss.str();
}

std::string
SystemOp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    if (strcmp(mnemonic, "fence_vma") == 0) {
        std::stringstream ss;
        ss << mnemonic << ' ' << registerName(srcRegIdx(0)) << ", " <<
            registerName(srcRegIdx(1));
        return ss.str();
    }

    return mnemonic;
}

} // namespace RiscvISA
} // namespace gem5

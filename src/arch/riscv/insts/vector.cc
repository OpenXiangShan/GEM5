/*
 * Copyright (c) 2022 PLCT Lab
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

#include "arch/riscv/insts/vector.hh"

#include <sstream>
#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/utility.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

std::string
VConfOp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (bit31 && bit30 == 0) {
        ss << registerName(srcRegIdx(0)) << ", " << registerName(srcRegIdx(1));
    } else if (bit31 && bit30) {
        ss << uimm << ", " << zimm;
    } else {
        ss << registerName(srcRegIdx(0)) << ", " << zimm;
    }
    return ss.str();
}

std::string VectorArithMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    return ss.str();
}

std::string VectorArithMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    if (machInst.vm == 0) {
        ss << ", v0.t";
    }
    return ss.str();
}

std::string VleMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VlWholeMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VseMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VsWholeMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VleMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VlWholeMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VseMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VsWholeMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

Fault
VldMvMicroInst::execute(ExecContext *xc, Trace::InstRecord *traceData) const
{
    vreg_t tmp_d0 = xc->getWritableVecRegOperand(this, 0);
    auto Vd = tmp_d0.as<uint8_t>();

    for (size_t i = 0; i < this->srcRegs; i++) {
        vreg_t tmp_s = xc->readVecRegOperand(this, i);
        auto s = tmp_s.as<uint8_t>();
        memcpy(Vd + i * CachelineSizeByte, s, CachelineSizeByte);
    }

    xc->setVecRegOperand(this, 0, tmp_d0);

    return NoFault;
}

std::string VldMvMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));
    for (int i = 0; i < NumVecMemInternalRegs; i++) {
        ss << ", " << registerName(srcRegIdx(i));
    }
    return ss.str();
}

Fault
VstMvMicroInst::execute(ExecContext *xc, Trace::InstRecord *traceData) const
{
    vreg_t tmp_s0 = xc->readVecRegOperand(this, 0);
    auto Vs = tmp_s0.as<uint8_t>();

    for (int i = 0; i < this->dstRegs; i++) {
        vreg_t tmp_d = xc->getWritableVecRegOperand(this, i);
        auto d = tmp_d.as<uint8_t>();
        memcpy(d, Vs + i * CachelineSizeByte, CachelineSizeByte);
        xc->setVecRegOperand(this, i, tmp_d);
    }

    return NoFault;
}

std::string VstMvMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ';
    for (int i = 0; i < NumVecMemInternalRegs; i++) {
        ss << registerName(destRegIdx(i)) << ", ";
    }
    ss << registerName(srcRegIdx(0));
    return ss.str();
}

} // namespace RiscvISA
} // namespace gem5

/*
 * Copyright (c) 2015 RISC-V Foundation
 * Copyright (c) 2017 The University of Virginia
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

#ifndef __ARCH_RISCV_INST_MEM_HH__
#define __ARCH_RISCV_INST_MEM_HH__

#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

class MemInst : public RiscvStaticInst
{
  protected:
    int64_t offset;
    Request::Flags memAccessFlags;

    MemInst(const char *mnem, ExtMachInst _extMachInst, OpClass __opClass)
        : RiscvStaticInst(mnem, _extMachInst, __opClass), offset(0)
    {}

    int64_t getImm() const override { return offset; }
  public:
    Request::Flags getMemAccessFlags() const { return memAccessFlags; }
};

class Load : public MemInst
{
  protected:
    using MemInst::MemInst;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class Store : public MemInst
{
  protected:
    using MemInst::MemInst;

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class StoreData : public RiscvStaticInst
{
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[0];

  public:
    int memsize = 0;

    StoreData(StaticInstPtr inst) : RiscvStaticInst("store_data", 0, OpClass::StoreData)
    {
        setRegIdxArrays(reinterpret_cast<RegIdArrayPtr>(&std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
                        reinterpret_cast<RegIdArrayPtr>(&std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        setSrcRegIdx(_numSrcRegs++, inst->srcRegIdx(1));
        flags[IsInteger] = true;
        assert(inst->operWid() > 7);
        memsize = inst->operWid() / 8;
    }

    Fault execute(ExecContext *, Trace::InstRecord *) const override;

    Fault initiateAcc(ExecContext *, Trace::InstRecord *) const override
    {
        panic("StoreData::initiateAcc() not implemented");
    }
    Fault completeAcc(PacketPtr, ExecContext *, Trace::InstRecord *) const override
    {
        panic("StoreData::completeAcc() not implemented");
    }

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const override { return "std"; }
};


} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_INST_MEM_HH__

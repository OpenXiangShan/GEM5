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

#include "arch/riscv/insts/mem.hh"

#include <sstream>
#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/utility.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/lsq_unit.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

std::string
Load::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string
Store::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

Fault
StoreData::execute(ExecContext *xc, Trace::InstRecord *) const
{
    auto inst = dynamic_cast<o3::DynInst *>(xc);
    auto data = inst->getRegOperand(inst->staticInst.get(), 0);

    if (inst->sqIt->instruction()->getFault() == NoFault) {
        // if instruction has already faulted, then skip executing std
        if (inst->sqIt->instruction()->memData)
            memcpy(inst->sqIt->instruction()->memData, &data, memsize);
        memcpy(inst->sqIt->data(), &data, memsize);
        inst->sqIt->setStatus(o3::SplitStoreStatus::DataReady);
        inst->sqIt->setStatus(o3::SplitStoreStatus::StdPipeFinish);
    }

    return NoFault;
}

} // namespace RiscvISA
} // namespace gem5

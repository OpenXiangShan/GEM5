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

float
getVflmul(uint32_t vlmul_encoding) {
  int vlmul = int8_t(vlmul_encoding << 5) >> 5;
  float vflmul = vlmul >= 0 ? 1 << vlmul : 1.0 / (1 << -vlmul);
  return vflmul;
}

uint32_t
getVlmax(VTYPE vtype, uint32_t vlen) {
  uint32_t sew = getSew(vtype.vsew);
  uint32_t vlmax = (vlen/sew) * getVflmul(vtype.vlmul);
  return vlmax;
}

std::string
VConfOp::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (bit31 && bit30 == 0) {
        ss << registerName(srcRegIdx(0)) << ", " << registerName(srcRegIdx(1));
    } else if (bit31 && bit30) {
        ss << uimm << ", " << generateZimmDisassembly();
    } else {
        ss << registerName(srcRegIdx(0)) << ", " << generateZimmDisassembly();
    }
    return ss.str();
}

std::string
VConfOp::generateZimmDisassembly() const
{
    std::stringstream s;

    // VSETIVLI uses ZIMM10 and VSETVLI uses ZIMM11
    uint64_t zimm = (bit31 && bit30) ? zimm10 : zimm11;

    bool frac_lmul = bits(zimm, 2);
    int sew = 1 << (bits(zimm, 5, 3) + 3);
    int lmul = bits(zimm, 1, 0);
    auto vta = bits(zimm, 6) == 1 ? "ta" : "tu";
    auto vma = bits(zimm, 7) == 1 ? "ma" : "mu";
    s << "e" << sew;
    if (frac_lmul) {
        std::string lmul_str = "";
        switch(lmul){
        case 3:
            lmul_str = "f2";
            break;
        case 2:
            lmul_str = "f4";
            break;
        case 1:
            lmul_str = "f8";
            break;
        default:
            panic("Unsupport fractional LMUL");
        }
        s << ", m" << lmul_str;
    } else {
        s << ", m" << (1 << lmul);
    }
    s << ", " << vta << ", " << vma;
    return s.str();
}

std::string
VectorNonSplitInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << registerName(srcRegIdx(0));
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorArithMicroInst::generateDisassembly(Addr pc,
        const Loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (machInst.funct3 == 0x3) {
        // OPIVI
      ss  << registerName(srcRegIdx(0)) << ", " << machInst.vecimm;
    } else {
      ss  << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorArithMacroInst::generateDisassembly(Addr pc,
        const Loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (machInst.funct3 == 0x3) {
        // OPIVI
      ss  << registerName(srcRegIdx(0)) << ", " << machInst.vecimm;
    } else {
      ss  << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorVMUNARY0MicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorVMUNARY0MacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorSlideMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) <<  ", ";
    if (machInst.funct3 == 0x3) {
      ss  << registerName(srcRegIdx(0)) << ", " << machInst.vecimm;
    } else {
      ss  << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VectorSlideMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
    if (machInst.funct3 == 0x3) {
      ss  << registerName(srcRegIdx(0)) << ", " << machInst.vecimm;
    } else {
      ss  << registerName(srcRegIdx(1)) << ", " << registerName(srcRegIdx(0));
    }
    if (machInst.vm == 0) ss << ", v0.t";
    return ss.str();
}

std::string VleMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << VLENB * microIdx << '(' << registerName(srcRegIdx(0)) << ')' << ", "
       << registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlWholeMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
       << VLENB * microIdx << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VseMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", "
       << VLENB * microIdx  << '(' << registerName(srcRegIdx(0)) << ')';
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsWholeMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", "
       << VLENB * microIdx << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string VleMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')';
    if (!machInst.vm) ss << ", v0.t";
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
    if (!machInst.vm) ss << ", v0.t";
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

std::string VlStrideMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')' <<
        ", " << registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlStrideMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')' <<
        ", "<< registerName(srcRegIdx(1));
    if (microIdx != 0 || machInst.vtype8.vma == 0 || machInst.vtype8.vta == 0)
        ss << ", " << registerName(srcRegIdx(2));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsStrideMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(2)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')' <<
        ", " << registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsStrideMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(2)) << ", " <<
        '(' << registerName(srcRegIdx(0)) << ')' <<
        ", "<< registerName(srcRegIdx(1));
    if (microIdx != 0 || machInst.vtype8.vma == 0 || machInst.vtype8.vta == 0)
        ss << ", " << registerName(srcRegIdx(2));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlIndexMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << '(' << registerName(srcRegIdx(0)) << "),"
        << registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VlIndexMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' '
        << registerName(destRegIdx(0)) << "[" << uint16_t(vdElemIdx) << "], "
        << '(' << registerName(srcRegIdx(0)) << "), "
        << registerName(srcRegIdx(1)) << "[" << uint16_t(vs2ElemIdx) << "]";
    if (microIdx != 0 || machInst.vtype8.vma == 0 || machInst.vtype8.vta == 0)
        ss << ", " << registerName(srcRegIdx(2));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsIndexMacroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(srcRegIdx(2)) << ", "
        << '(' << registerName(srcRegIdx(0)) << "),"
        << registerName(srcRegIdx(1));
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string VsIndexMicroInst::generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' '
        << registerName(srcRegIdx(2)) << "[" << uint16_t(vs3ElemIdx) << "], "
        << '(' << registerName(srcRegIdx(0)) << "), "
        << registerName(srcRegIdx(1)) << "[" << uint16_t(vs2ElemIdx) << "]";
    if (!machInst.vm) ss << ", v0.t";
    return ss.str();
}

std::string
VMvWholeMacroInst::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        registerName(srcRegIdx(1));
    return ss.str();
}

std::string
VMvWholeMicroInst::generateDisassembly(Addr pc,
    const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        registerName(srcRegIdx(1));
    return ss.str();
}

Vle8ff_vMicro::Vle8ff_vMicro(ExtMachInst _machInst, uint8_t _microVl, uint8_t _microIdx)
    : VleMicroInst("vle8ff_v_micro", _machInst, VectorUnitStrideLoadOp, _microVl,
        _microIdx)
{
    
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
            ;
    _numSrcRegs = 0;
    _numDestRegs = 0;
    setDestRegIdx(_numDestRegs++, vecRegClass[_machInst.vd + _microIdx]);
    _numTypedDestRegs[VecRegClass]++;
    setDestRegIdx(_numDestRegs++, vecRegClass[VecMemInternalReg0 + _microIdx]);
    _numTypedDestRegs[VecRegClass]++;
    setSrcRegIdx(_numSrcRegs++, intRegClass[_machInst.rs1]);
    setSrcRegIdx(_numSrcRegs++, vecRegClass[_machInst.vd + _microIdx]);
    if (!_machInst.vm) {
        setSrcRegIdx(_numSrcRegs++, vecRegClass[0]);
    }
}

Fault
Vle8ff_vMicro::execute(ExecContext *xc, Trace::InstRecord * traceData) const
{
    printf("Vle8ff_vMicro::execute begin\n");
    Addr EA;
    uint64_t Rs1 = 0;
    vreg_t Mem = {};
    auto &tmp_d0 = *(vreg_t *)xc->getWritableRegOperand(this, 0);
    auto &fof_d0 = *(vreg_t *)xc->getWritableRegOperand(this, 1);
    auto Vd = tmp_d0.as<uint8_t>();
    auto Fof = fof_d0.as<uint64_t>();
    Rs1 = xc->getRegOperand(this, 0);
    vreg_t tmp_s1; 
    xc->getRegOperand(this, 1, &tmp_s1);
    auto Vs2 = tmp_s1.as<uint8_t>();
    
    auto base_addr = Rs1;
    EA = Rs1 + VLENB * microIdx; ;

    vreg_t tmp_v0;
    uint8_t *v0;
    if(!machInst.vm) {
        xc->getRegOperand(this, _numSrcRegs - 1, &tmp_v0);
        v0 = tmp_v0.as<uint8_t>();
    }

    uint32_t mem_size = width_EEW(machInst.width) / 8 * this->microVl;
    const std::vector<bool> byte_enable(mem_size, true);

    Fault fault = xc->readMem(EA, Mem.as<uint8_t>(), mem_size, memAccessFlags,
                            byte_enable);
    uint64_t fault_elem_idx = mem_size / sizeof(uint8_t);

    if (fault != NoFault) {
        auto addr_fault = dynamic_cast<AddressFault*>(fault.get());
        assert(addr_fault != nullptr);
        if (addr_fault) {
            auto fault_addr = addr_fault->trap_value();
            assert(fault_addr >= EA);
            fault_elem_idx = (fault_addr - base_addr) / sizeof(uint8_t);
        }
    }

    if (fault_elem_idx == 0)
        return fault;

    Fof[0] = fault_elem_idx; // set fault elem idx in VecMemInternalReg0 + idx
    const size_t micro_vlmax = vtype_VLMAX(machInst.vtype8, true);
    const size_t micro_elems = VLEN / width_EEW(machInst.width);
    size_t ei;
    for (size_t i = 0; i < std::min(micro_elems, fault_elem_idx); i++) {
        ei = i + micro_vlmax * microIdx;
        if ((machInst.vm || elem_mask(v0, ei)) &&
            i < this->microVl) {
            Vd[i] = Mem.as<uint8_t>()[i];
        } else {
            Vd[i] = Vs2[i];
        }
    }

    xc->setRegOperand(this, 1, &fof_d0);
    xc->setRegOperand(this, 0, &tmp_d0);
    if (traceData) {
        traceData->setData(tmp_d0);
    }

    printf("Vle8ff_vMicro::execute end\n");
    return fault;
}

Fault
Vle8ff_vMicro::initiateAcc(ExecContext *xc, Trace::InstRecord *traceData) const
{
    Addr EA;
    uint64_t Rs1 = 0;
    auto &fof_d0 = *(vreg_t *)xc->getWritableRegOperand(this, 1);
    auto Fof = fof_d0.as<uint64_t>();
    Rs1 = xc->getRegOperand(this, 0);
    
    auto base_addr = Rs1;
    EA = Rs1 + VLENB * microIdx; ;

    uint32_t mem_size = width_EEW(machInst.width) / 8 * this->microVl;
    const std::vector<bool> byte_enable(mem_size, true);
    Fault fault = xc->initiateMemRead(EA, mem_size, memAccessFlags, byte_enable);
    uint64_t fault_elem_idx = mem_size / sizeof(uint8_t);

    if (fault != NoFault) {
        auto addr_fault = dynamic_cast<AddressFault*>(fault.get());
        assert(addr_fault != nullptr);
        if (addr_fault) {
            auto fault_addr = addr_fault->trap_value();
            assert(fault_addr >= EA);
            fault_elem_idx = (fault_addr - base_addr) / sizeof(uint8_t);
        }
    }

    if (fault_elem_idx == 0)
        return fault;

    Fof[0] = fault_elem_idx; // set fault elem idx in VecMemInternalReg0 + idx
    xc->setRegOperand(this, 1, &fof_d0);

    printf("Vle8ff_vMicro::initiateAcc end\n");
    return fault;
}

Fault
Vle8ff_vMicro::completeAcc(PacketPtr pkt, ExecContext *xc,
                            Trace::InstRecord *traceData) const
{
    vreg_t Mem = {};
    auto &tmp_d0 = *(vreg_t *)xc->getWritableRegOperand(this, 0);
    auto Vd = tmp_d0.as<uint8_t>();
    vreg_t tmp_s1; 
    xc->getRegOperand(this, 1, &tmp_s1);
    auto Vs2 = tmp_s1.as<uint8_t>();
    
    vreg_t tmp_v0;
    uint8_t *v0;
    if(!machInst.vm) {
        xc->getRegOperand(this, _numSrcRegs - 1, &tmp_v0);
        v0 = tmp_v0.as<uint8_t>();
    }

    uint32_t mem_size = width_EEW(machInst.width) / 8 * this->microVl;
    const std::vector<bool> byte_enable(mem_size, true);

    if (!machInst.vm) {
        xc->getRegOperand(this, _numSrcRegs - 1, &tmp_v0);
        v0 = tmp_v0.as<uint8_t>();
    }

    memcpy(Mem.as<uint8_t>(), pkt->getPtr<uint8_t>(), pkt->getSize());


    const size_t micro_vlmax = vtype_VLMAX(machInst.vtype8, true);
    const size_t micro_elems = VLEN / width_EEW(machInst.width);
    size_t ei;
    for (size_t i = 0; i < micro_elems; i++) {
        ei = i + micro_vlmax * microIdx;
        if ((machInst.vm || elem_mask(v0, ei)) &&
            i < this->microVl) {
            Vd[i] = Mem.as<uint8_t>()[i];
        } else {
            Vd[i] = Vs2[i];
        }
    }

    xc->setRegOperand(this, 0, &tmp_d0);
    if (traceData) {
        traceData->setData(tmp_d0);
    }

    printf("Vle8ff_vMicro::completeAcc end\n");
    return NoFault;
}

VleffEndMicroInst::VleffEndMicroInst(ExtMachInst extMachInst, uint8_t _numSrcs)
    : VectorMicroInst("VleffEnd", extMachInst,
    VectorIntegerArithOp, 0, 0)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
    _numSrcRegs = 0;
    _numDestRegs = 0;
    for (uint8_t i = 0; i < _numSrcs; i++) {
        setSrcRegIdx(_numSrcRegs++, vecRegClass[VecMemInternalReg0 + i]);
    }
    this->numSrcs = _numSrcs;
    printf("VleffEndMicroInst numSrc: %hhu, numDestRegs: %hhu\n", this->numSrcs, _numDestRegs);

    flags[IsNonSpeculative] = true;
    flags[IsSerializeAfter] = true;
}

Fault
VleffEndMicroInst::execute(ExecContext* xc, Trace::InstRecord* traceData) const
{
    printf("VleffEndMicroInst::execute begin\n");
    vreg_t cnt[8];
    for (uint8_t i = 0; i < this->numSrcs; i++) {
        xc->getRegOperand(this, i, cnt + i);
    }

    printf("VleffEndMicroInst::execute getRegOperand done\n");

    // [[maybe_unused]]uint64_t vl = *(uint64_t*)xc->getWritableRegOperand(this, 0);
    printf("VleffEndMicroInst::execute getWritableRegOperand done\n");
    
    uint64_t new_vl = 0;
    for (uint8_t i = 0; i < this->numSrcs; i++) {
        new_vl += cnt[i].as<uint64_t>()[0];
    }
    printf("VleffEndMicroInst::execute new_vl sum done\n");

    // xc->setRegOperand(this, 0, new_vl);
    xc->setMiscReg(MISCREG_VL, new_vl);

    printf("VleffEndMicroInst::execute setRegOperand done\n");

    if (traceData)
        traceData->setData(new_vl);
    printf("VleffEndMicroInst::execute end\n");
    return NoFault;
}

std::string
VleffEndMicroInst::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0));
    return ss.str();
}

Vle8ff_v::Vle8ff_v(ExtMachInst _machInst)
    : VleMacroInst("vle8ff_v", _machInst, VectorUnitStrideLoadOp)
{
    setRegIdxArrays(
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
            ;
    
    setDestRegIdx(_numDestRegs++, vecRegClass[_machInst.vd]);
    _numTypedDestRegs[vecRegClass.type()]++;
    setSrcRegIdx(_numSrcRegs++, ((_machInst.rs1) == 0) ? RegId() : intRegClass[_machInst.rs1]);
    setSrcRegIdx(_numSrcRegs++, vecRegClass[_machInst.vs2]);
    flags[IsInteger] = true;
    flags[IsLoad] = true;
    flags[IsVector] = true;;

    const int32_t micro_vlmax = VLEN / width_EEW(_machInst.width);
    const uint32_t num_microops = ceil((float) this->vl / (micro_vlmax));
    int32_t remaining_vl = this->vl;
    int32_t micro_vl = std::min(remaining_vl, micro_vlmax);
    StaticInstPtr microop;

    if (micro_vl == 0) {
        microop = new VectorNopMicroInst(_machInst);
        this->microops.push_back(microop);
    } else {
        for (int i = 0; i < num_microops && micro_vl > 0; ++i) {
            microop = new Vle8ff_vMicro(_machInst, micro_vl, i);
            microop->setDelayedCommit();
            microop->setFlag(IsLoad);
            this->microops.push_back(microop);
            micro_vl = std::min(remaining_vl -= micro_vlmax, micro_vlmax);
        }
        microop = new VleffEndMicroInst(_machInst, num_microops);
        this->microops.push_back(microop);
    }
    this->microops.front()->setFirstMicroop();
    this->microops.back()->setLastMicroop();

}

} // namespace RiscvISA
} // namespace gem5

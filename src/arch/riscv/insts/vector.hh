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

#ifndef __ARCH_RISCV_INSTS_VECTOR_HH__
#define __ARCH_RISCV_INSTS_VECTOR_HH__

#include <string>

#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/utility.hh"
#include "base/bitfield.hh"
#include "cpu/exec_context.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

float
getVflmul(uint32_t vlmul_encoding);

inline uint32_t getSew(uint32_t vsew) {
    assert(vsew <= 3);
    return (8 << vsew);
}

uint32_t
getVlmax(VTYPE vtype, uint32_t vlen);

/**
 * Base class for Vector Config operations
 */
class VConfOp : public RiscvStaticInst
{
  protected:
    uint64_t bit30;
    uint64_t bit31;
    uint64_t zimm10;
    uint64_t zimm11;
    uint64_t uimm;
    VConfOp(const char *mnem, ExtMachInst _extMachInst, OpClass __opClass)
        : RiscvStaticInst(mnem, _extMachInst, __opClass),
          bit30(_extMachInst.bit30), bit31(_extMachInst.bit31),
          zimm10(_extMachInst.zimm_vsetivli),
          zimm11(_extMachInst.zimm_vsetvli),
          uimm(_extMachInst.uimm_vsetivli)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;

    std::string generateZimmDisassembly() const;
};

inline uint8_t checked_vtype(bool vill, uint8_t vtype) {
    panic_if(vill, "vill has been set");
    const uint8_t vsew = bits(vtype, 5, 3);
    panic_if(vsew >= 0b100, "vsew: %#x not supported", vsew);
    const uint8_t vlmul = bits(vtype, 2, 0);
    panic_if(vlmul == 0b100, "vlmul: %#x not supported", vlmul);
    return vtype;
}

class VectorNonSplitInst : public RiscvStaticInst
{
  protected:
    uint32_t vl;
    uint8_t vtype;
    VectorNonSplitInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : RiscvStaticInst(mnem, _machInst, __opClass),
        vl(_machInst.vl),
        vtype(checked_vtype(_machInst.vill, _machInst.vtype8))
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorMacroInst : public RiscvMacroInst
{
  protected:
    uint32_t vl;
    uint8_t vtype;
    VectorMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : RiscvMacroInst(mnem, _machInst, __opClass),
        vl(_machInst.vl),
        vtype(checked_vtype(_machInst.vill, _machInst.vtype8))
    {
        this->flags[IsVector] = true;
    }
};

class VectorMicroInst : public RiscvMicroInst
{
protected:
    uint8_t microVl;
    uint8_t microIdx;
    uint8_t vtype;
    VectorMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                    uint8_t _microVl, uint8_t _microIdx)
        : RiscvMicroInst(mnem, _machInst, __opClass),
        microVl(_microVl),
        microIdx(_microIdx),
        vtype(_machInst.vtype8)
    {
        this->flags[IsVector] = true;
    }
};

class VectorNopMicroInst : public RiscvMicroInst
{
public:
    VectorNopMicroInst(ExtMachInst _machInst)
        : RiscvMicroInst("vnop", _machInst, No_OpClass)
    {}

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData)
        const override
    {
        return NoFault;
    }

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab)
      const override
    {
        std::stringstream ss;
        ss << mnemonic;
        return ss.str();
    }
};

class VectorArithMicroInst : public VectorMicroInst
{
protected:
    VectorArithMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microVl,
                         uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorArithMacroInst : public VectorMacroInst
{
  protected:
    VectorArithMacroInst(const char* mnem, ExtMachInst _machInst,
                         OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorVMUNARY0MicroInst : public VectorMicroInst
{
protected:
    VectorVMUNARY0MicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microVl,
                         uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorVMUNARY0MacroInst : public VectorMacroInst
{
  protected:
    VectorVMUNARY0MacroInst(const char* mnem, ExtMachInst _machInst,
                         OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorSlideMacroInst : public VectorMacroInst
{
  protected:
    VectorSlideMacroInst(const char* mnem, ExtMachInst _machInst,
                         OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorSlideMicroInst : public VectorMicroInst
{
  protected:
    uint8_t vdIdx;
    uint8_t vs2Idx;
    VectorSlideMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microVl,
                         uint8_t _microIdx, uint8_t _vdIdx, uint8_t _vs2Idx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
        , vdIdx(_vdIdx), vs2Idx(_vs2Idx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorMemMicroInst : public VectorMicroInst
{
  protected:
    uint32_t offset; // Used to calculate EA.
    Request::Flags memAccessFlags;

    VectorMemMicroInst(const char* mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint8_t _microVl, uint8_t _microIdx,
                       uint32_t _offset)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
        , offset(_offset)
        , memAccessFlags(0)
    {}
};

class VectorMemMacroInst : public VectorMacroInst
{
  protected:
    VectorMemMacroInst(const char* mnem, ExtMachInst _machInst,
                       OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {}
};

class VleMacroInst : public VectorMemMacroInst
{
  protected:
    VleMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VseMacroInst : public VectorMemMacroInst
{
  protected:
    VseMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VleMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VleMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint8_t _microVl, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {
        this->flags[IsLoad] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VseMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VseMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint8_t _microVl, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {
        this->flags[IsStore] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VleffMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VleffMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint8_t _microVl, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {
        this->flags[IsLoad] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VleffEndMicroInst : public VectorMicroInst
{
private:
    RegId srcRegIdxArr[8];   // vle tmp target, used to keep RAW sequence
    RegId destRegIdxArr[1];  // vstart
    uint8_t numSrcs;
public:
    VleffEndMicroInst(ExtMachInst extMachInst, uint8_t _numSrcs);

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData) const override;

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlWholeMacroInst : public VectorMemMacroInst
{
  protected:
    VlWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass)
      : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
      Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlWholeMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VlWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint8_t _microVl, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {}

    std::string generateDisassembly(
      Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsWholeMacroInst : public VectorMemMacroInst
{
  protected:
    VsWholeMacroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsWholeMicroInst : public VectorMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VsWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint8_t _microVl, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microIdx, _microIdx)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlStrideMacroInst : public VectorMemMacroInst
{
  protected:
    VlStrideMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlStrideMicroInst : public VectorMemMicroInst
{
  protected:
  uint8_t regIdx;
    VlStrideMicroInst(const char *mnem, ExtMachInst _machInst,
                      OpClass __opClass, uint8_t _regIdx,
                      uint8_t _microIdx, uint8_t _microVl)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microVl,
                             _microIdx, 0)
        , regIdx(_regIdx)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsStrideMacroInst : public VectorMemMacroInst
{
  protected:
    VsStrideMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsStrideMicroInst : public VectorMemMicroInst
{
  protected:
  uint8_t regIdx;
    VsStrideMicroInst(const char *mnem, ExtMachInst _machInst,
                      OpClass __opClass, uint8_t _regIdx,
                      uint8_t _microIdx, uint8_t _microVl)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microVl,
                             _microIdx, 0)
        , regIdx(_regIdx)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlIndexMacroInst : public VectorMemMacroInst
{
  protected:
    VlIndexMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VlIndexMicroInst : public VectorMemMicroInst
{
  protected:
    uint8_t vdRegIdx;
    uint8_t vdElemIdx;
    uint8_t vs2RegIdx;
    uint8_t vs2ElemIdx;
    VlIndexMicroInst(const char *mnem, ExtMachInst _machInst,
                    OpClass __opClass, uint8_t _vdRegIdx, uint8_t _vdElemIdx,
                    uint8_t _vs2RegIdx, uint8_t _vs2ElemIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, 1,
                             0, 0)
        , vdRegIdx(_vdRegIdx), vdElemIdx(_vdElemIdx)
        , vs2RegIdx(_vs2RegIdx), vs2ElemIdx(_vs2ElemIdx)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsIndexMacroInst : public VectorMemMacroInst
{
  protected:
    VsIndexMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : VectorMemMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VsIndexMicroInst : public VectorMemMicroInst
{
  protected:
    uint8_t vs3RegIdx;
    uint8_t vs3ElemIdx;
    uint8_t vs2RegIdx;
    uint8_t vs2ElemIdx;
    VsIndexMicroInst(const char *mnem, ExtMachInst _machInst,
                    OpClass __opClass, uint8_t _vs3RegIdx, uint8_t _vs3ElemIdx,
                    uint8_t _vs2RegIdx, uint8_t _vs2ElemIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, 1, 0, 0)
        , vs3RegIdx(_vs3RegIdx), vs3ElemIdx(_vs3ElemIdx)
        , vs2RegIdx(_vs2RegIdx), vs2ElemIdx(_vs2ElemIdx)
    {}

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VMvWholeMacroInst : public VectorArithMacroInst
{
  protected:
    VMvWholeMacroInst(const char* mnem, ExtMachInst _machInst,
                         OpClass __opClass)
        : VectorArithMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VMvWholeMicroInst : public VectorArithMicroInst
{
  protected:
    VMvWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microVl,
                         uint8_t _microIdx)
        : VectorArithMicroInst(mnem, _machInst, __opClass, _microVl, _microIdx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

template<typename ElemType>
class VMaskMergeMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[NumVecInternalRegs];
    RegId destRegIdxArr[1];

  public:
    VMaskMergeMicroInst(ExtMachInst extMachInst, uint8_t _dstReg,
        uint8_t _numSrcs)
        : VectorArithMicroInst("vmask_mv_micro", extMachInst,
          VectorIntegerArithOp, 0, 0)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        _numSrcRegs = 0;
        _numDestRegs = 0;

        setDestRegIdx(_numDestRegs++, vecRegClass[_dstReg]);
        _numTypedDestRegs[VecRegClass]++;
        for (uint8_t i=0; i<_numSrcs; i++) {
            setSrcRegIdx(_numSrcRegs++, vecRegClass[VecMemInternalReg0 + i]);
        }
    }

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData)
            const override {
        vreg_t tmp_d0 = *(vreg_t *)xc->getWritableRegOperand(this, 0);
        auto Vd = tmp_d0.as<uint8_t>();
        constexpr uint8_t elems_per_vreg = VLENB / sizeof(ElemType);
        size_t bit_cnt = elems_per_vreg;
        vreg_t tmp_s;
        xc->getRegOperand(this, 0, &tmp_s);
        auto s = tmp_s.as<uint8_t>();
        // cp the first result and tail
        memcpy(Vd, s, VLENB);
        for (uint8_t i = 1; i < this->_numSrcRegs; i++) {
            xc->getRegOperand(this, i, &tmp_s);
            s = tmp_s.as<uint8_t>();
            if constexpr (elems_per_vreg < 8) {
                constexpr uint8_t m = (1 << elems_per_vreg) - 1;
                const uint8_t mask = m << (i * elems_per_vreg % 8);
                // clr & ext bits
                Vd[bit_cnt/8] ^= Vd[bit_cnt/8] & mask;
                Vd[bit_cnt/8] |= s[bit_cnt/8] & mask;
                bit_cnt += elems_per_vreg;
            } else {
                constexpr uint8_t byte_offset = elems_per_vreg / 8;
                memcpy(Vd + i * byte_offset, s + i * byte_offset, byte_offset);
            }
        }
        xc->setRegOperand(this, 0, &tmp_d0);
        if (traceData)
            traceData->setData(tmp_d0);
        return NoFault;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override {
        std::stringstream ss;
        ss << mnemonic << ' ' << registerName(destRegIdx(0));
        for (uint8_t i = 0; i < this->_numSrcRegs; i++) {
            ss << ", " << registerName(srcRegIdx(i));
        }
        ss << ", offset:" << VLENB / sizeof(ElemType);
        return ss.str();
    }
};

class VxsatMicroInst : public VectorArithMicroInst
{
  private:
    bool* vxsat;
  public:
    VxsatMicroInst(bool* Vxsat, ExtMachInst extMachInst)
        : VectorArithMicroInst("vxsat_micro", extMachInst,
          VectorIntegerArithOp, 0, 0)
    {
        vxsat = Vxsat;
    }
    Fault execute(ExecContext* xc, Trace::InstRecord* traceData)
    const override
    {
        xc->setMiscReg(MISCREG_VXSAT,*vxsat);
        auto vcsr = xc->readMiscReg(MISCREG_VCSR);
        xc->setMiscReg(MISCREG_VCSR, ((vcsr&~1)|*vxsat));
        return NoFault;
    }
    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab)
      const override
    {
        std::stringstream ss;
        ss << mnemonic << ' ' << "VXSAT" << ", " << (*vxsat ? "0x1" : "0x0");
        return ss.str();
    }
};

class VCompressPopcMicroInst : public VectorArithMicroInst
{
private:
    RegId srcRegIdxArr[1];  // vm
    RegId destRegIdxArr[1]; // vcnt

  public:
    VCompressPopcMicroInst(ExtMachInst extMachInst)
        : VectorArithMicroInst("VPopCount", extMachInst,
          VectorIntegerArithOp, 0, 0)
    {
        setRegIdxArrays(reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
        reinterpret_cast<RegIdArrayPtr>(
            &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setDestRegIdx(_numDestRegs++, vecRegClass[VecMemInternalReg0]);
        _numTypedDestRegs[VecRegClass]++;
        setSrcRegIdx(_numSrcRegs++, vecRegClass[extMachInst.vs1]);
    }

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData) const override
    {

        const int8_t vlmul = vtype_vlmul(vtype);
        const size_t sew =  vtype_SEW(vtype);
        const size_t countN = VLEN / sew;
        const uint32_t numVRegs = 1 << std::max<int64_t>(0, vlmul);

        size_t cnt[8] = {0};
        auto popcount_in_byte = [](uint8_t* addr, uint8_t msb, uint8_t lsb)
             -> int {
            return popCount(mask(msb, lsb) & *addr);
        };

        auto popcount_byte = [](uint8_t* l_addr, uint8_t* r_addr) -> int {
            size_t res = 0;
            while (l_addr < r_addr) {
                res += popCount(*l_addr);
                l_addr++;
            }
            return res;
        };

        vreg_t vs;
        xc->getRegOperand(this, 0, &vs);
        vreg_t vd = *(vreg_t *)xc->getWritableRegOperand(this, 0);

        for (int i = 0; i < std::max<int8_t>(1, numVRegs); i++) {
            uint8_t* base_addr = vs.as<uint8_t>() + i*countN/8;
            if (countN < 8) {
                cnt[i] = popcount_in_byte(base_addr, i * countN % 8,
                    (i + 1)*countN % 8);
            } else {
                cnt[i] = popcount_byte(base_addr, base_addr + countN / 8);
            }
        }

        for (int i = 0; i < std::max<int8_t>(1, numVRegs); i++) {
            vd.as<uint8_t>()[i] = cnt[i];
        }

        xc->setRegOperand(this, 0, &vd);
        if (traceData)
            traceData->setData(vd);
        return NoFault;
    }

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab)
        const override
    {
        std::stringstream ss;
        ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << registerName(srcRegIdx(0)) << ", ";
        return ss.str();
    }

};

template<typename Type>
class VCompressMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[4];  // vs, vcnt, vm, old_vd
    RegId destRegIdxArr[1]; // vd
    uint8_t vsIdx;
    uint8_t vdIdx;
  public:
    VCompressMicroInst(ExtMachInst extMachInst, uint8_t microVl,
        uint8_t microIdx, uint8_t _vsIdx, uint8_t _vdIdx)
        : VectorArithMicroInst("Vcompress_micro", extMachInst,
          VectorIntegerArithOp, microVl, microIdx)
        , vsIdx(_vsIdx), vdIdx(_vdIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        _numSrcRegs = 0;
        _numDestRegs = 0;
        setDestRegIdx(_numDestRegs++, vecRegClass[extMachInst.vd + _vdIdx]);
        _numTypedDestRegs[VecRegClass]++;
        // vs
        setSrcRegIdx(_numSrcRegs++, vecRegClass[extMachInst.vs2 + _vsIdx]);
        // vcnt
        setSrcRegIdx(_numSrcRegs++, vecRegClass[VecMemInternalReg0]);
        // vm
        setSrcRegIdx(_numSrcRegs++, vecRegClass[extMachInst.vs1]);
        // old_vd
        setSrcRegIdx(_numSrcRegs++, vecRegClass[extMachInst.vd + _vdIdx]);
    }

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData) const override
    {
        const int sew = vtype_SEW(vtype);
        const int uvlmax = VLEN / sew;

        vreg_t vs, vcnt, vm, old_vd;
        xc->getRegOperand(this, 0, &vs);
        xc->getRegOperand(this, 1, &vcnt);
        xc->getRegOperand(this, 2, &vm);
        xc->getRegOperand(this, 3, &old_vd);

        vreg_t vd = *(vreg_t *)xc->getWritableRegOperand(this, 0);
        memcpy(vd.as<uint8_t>(), old_vd.as<uint8_t>(), VLENB);

        vreg_t vtmp;

        auto vcnt_get_elem = [&](int idx) -> size_t {
            return vcnt.as<uint8_t>()[idx];
        };

        int num_vs_elem_moved = 0;
        int num_vd_elem_moved = vdIdx * uvlmax;
        for (int i = 0; i < vsIdx; i++) {
            num_vs_elem_moved += vcnt_get_elem(i);
        }

        int vtmpIdx = 0;
        for (int i = 0; i < microVl; i++) {
            if (elem_mask(vm.as<uint8_t>(), i + vsIdx * uvlmax)) {
                vtmp.as<Type>()[vtmpIdx++] = vs.as<Type>()[i];
            }
        }
        int vsElemIdxBase = std::max(0, num_vd_elem_moved - num_vs_elem_moved);
        int vdElemIdxBase = std::max(0, num_vs_elem_moved - num_vd_elem_moved);

        for (; vsElemIdxBase < vtmpIdx && vdElemIdxBase < microVl;) {
            vd.as<Type>()[vdElemIdxBase++] = vtmp.as<Type>()[vsElemIdxBase++];
        }
        xc->setRegOperand(this, 0, &vd);
        if (traceData)
            traceData->setData(vd);
        return NoFault;
    }

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab)
        const override
    {
        std::stringstream ss;
        ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << registerName(srcRegIdx(0)) << ", "
        << registerName(srcRegIdx(1)) << ", "
        << registerName(srcRegIdx(2)) << ", "
        << registerName(srcRegIdx(3)) << ", ";
        return ss.str();
    }
};

template<typename Type>
class Vcompress_vm : public VectorArithMacroInst
{
  private:
    RegId srcRegIdxArr[2];  // vs, vm
    RegId destRegIdxArr[1]; // vd
  public:
    Vcompress_vm(ExtMachInst _machInst)
        : VectorArithMacroInst("vcompress_vm", _machInst, VectorIntegerArithOp)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setDestRegIdx(_numDestRegs++, vecRegClass[_machInst.vd]);
        _numTypedDestRegs[VecRegClass]++;
        setSrcRegIdx(_numSrcRegs++, vecRegClass[_machInst.vs1]);
        setSrcRegIdx(_numSrcRegs++, vecRegClass[_machInst.vs2]);

        const uint32_t num_microops = vtype_regs_per_group(vtype);
        int32_t tmp_vl = this->vl;
        const int32_t micro_vlmax = vtype_VLMAX(_machInst.vtype8, true);
        int32_t micro_vl = std::min(tmp_vl, micro_vlmax);

        StaticInstPtr microop;
        microop = new VCompressPopcMicroInst(_machInst);
        this->microops.push_back(microop);

        int8_t microIdx = 0;
        for (int i = 0; i < num_microops && micro_vl > 0; ++i) {
            for (int j = 0; j <= i; ++j) {
                microop = new VCompressMicroInst<Type>(
                    _machInst, micro_vl, microIdx++, i, j);
                microop->setDelayedCommit();
                this->microops.push_back(microop);
            }
            micro_vl = std::min(tmp_vl -= micro_vlmax, micro_vlmax);
        }
        this->microops.front()->setFirstMicroop();
        this->microops.back()->setLastMicroop();
    }

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab)
        const override
    {
        std::stringstream ss;
        ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
        << registerName(srcRegIdx(1)) << ", "
        << registerName(srcRegIdx(0));
        return ss.str();
    }
};

} // namespace RiscvISA
} // namespace gem5


#endif // __ARCH_RISCV_INSTS_VECTOR_HH__

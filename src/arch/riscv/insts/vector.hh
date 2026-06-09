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

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

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

#define RVV_AGNOSTIC 0  // agnostic, if NEMU CONFIG_RVV_AGNOSTIC is set, set 1
struct VectorMicroInfo
{
    int32_t rs = ~0;
    int32_t re = ~0;
    int32_t rb = ~0;

    int32_t microVd = ~0;
    int32_t microVs1 = ~0;
    int32_t microVs2 = ~0;
    int32_t microVs3 = ~0;

    int32_t fn = ~0; // segment idx
    uint32_t offset = ~0; // vload/store baseAddr offset
};



/**
 * Base class for Vector Config operations
 */
class VConfOp : public RiscvStaticInst
{
  public:
    bool vtypeIsImm = false;
    uint8_t earlyVtype = -1;
  protected:
    int vlsrcIdx = -1;
    int vtypesrcIdx = -1;
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
    {
        // vsetvl instructions modify vector state (vl, vtype)
        // They should set mstatus.vs to dirty
        flags[IsVector] = true;
    }

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
  public:
    int oldDstIdx = -1;
    int vmsrcIdx = -1;
    int vlsrcIdx = -1;
    int vtypesrcIdx = -1;
  protected:
    const int microIdx = 0; // just for convenience
    const bool vm;
    const uint8_t vsew;
    const int8_t vlmul;
    const uint32_t sew;
    const float vflmul;
    VectorNonSplitInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : RiscvStaticInst(mnem, _machInst, __opClass),
        vm(_machInst.vm),
        vsew(_machInst.vtype8.vsew),
        vlmul(vtype_vlmul(_machInst.vtype8)),
        sew( (8 << vsew) ),
        vflmul( vlmul < 0 ? (1.0 / (1 << (-vlmul))) : (1 << vlmul) )
    {
        this->flags[IsVector] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorMacroInst : public RiscvMacroInst
{
  protected:
    const bool vm;
    const uint8_t vsew;
    const int8_t vlmul;
    const uint32_t sew;
    const float vflmul;
    VectorMacroInst(const char* mnem, ExtMachInst _machInst,
                   OpClass __opClass)
        : RiscvMacroInst(mnem, _machInst, __opClass),
        vm(_machInst.vm),
        vsew(_machInst.vtype8.vsew),
        vlmul(vtype_vlmul(_machInst.vtype8)),
        sew( (8 << vsew) ),
        vflmul( vlmul < 0 ? (1.0 / (1 << (-vlmul))) : (1 << vlmul) )
    {
        this->flags[IsVector] = true;
    }
};

class VectorMicroInst : public RiscvMicroInst
{
public:
    VectorMicroInfo vmi;
    int oldDstIdx = -1;
    int vmsrcIdx = -1;
    int vlsrcIdx = -1;
    int vtypesrcIdx = -1;
    int vstartsrcIdx = -1;
    int vregsrcIdx = -1;
    int vregsrcNum = 0;
    int vs1srcIdx = -1;
    int vs2srcIdx = -1;
    int vs3srcIdx = -1;
    uint8_t getMicroIdx() const { return microIdx; }
protected:
    const uint8_t microIdx;
    const bool vm;
    const uint8_t vsew;
    const int8_t vlmul;
    const uint32_t sew;
    const float vflmul;
    VectorMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                    uint8_t _microIdx)
        : RiscvMicroInst(mnem, _machInst, __opClass),
        microIdx(_microIdx),
        vm(_machInst.vm),
        vsew(_machInst.vtype8.vsew),
        vlmul(vtype_vlmul(_machInst.vtype8)),
        sew( (8 << vsew) ),
        vflmul( vlmul < 0 ? (1.0 / (1 << (-vlmul))) : (1 << vlmul) )
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
                         OpClass __opClass, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microIdx)
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
                         OpClass __opClass, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microIdx)
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
    VectorSlideMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microIdx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorMemMicroInst : public VectorMicroInst
{
  protected:
    uint8_t veew;
    uint32_t eew;
    Request::Flags memAccessFlags;

    VectorMemMicroInst(const char* mnem, ExtMachInst _machInst,
                       OpClass __opClass, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microIdx),
          veew(_machInst.width),
          eew(width_EEW(veew)),
          memAccessFlags(0)
    {}
};

class VectorMemMacroInst : public VectorMacroInst
{
  protected:
    uint8_t veew;
    uint32_t eew;
    VectorMemMacroInst(const char* mnem, ExtMachInst _machInst,
                       OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass),
          veew(_machInst.width),
          eew(width_EEW(veew))
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

class VleMicroInst : public VectorMemMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VleMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
    {
        this->flags[IsLoad] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VseMicroInst : public VectorMemMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VseMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                 uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
    {
        this->flags[IsStore] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VleffMicroInst : public VectorMemMicroInst
{
  protected:
    Request::Flags memAccessFlags;

    VleffMicroInst(const char *mnem, ExtMachInst _machInst, OpClass __opClass,
                   uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
    {
        this->flags[IsLoad] = true;
    }

    std::string generateDisassembly(
        Addr pc, const loader::SymbolTable *symtab) const override;
};

class VleffEndMicroInst : public VectorMicroInst
{
private:
    // VFOF counters from vecbuf + renamed VL dependency.
    RegId srcRegIdxArr[NumVecBufRegs + 1];
    RegId destRegIdxArr[1];
    uint8_t numSrcs;
    uint8_t fofSrcBase;
    bool packedFaultSlots;
    uint8_t packedFaultCount;
public:
    VleffEndMicroInst(ExtMachInst extMachInst, uint8_t _numSrcs,
                      uint8_t _fofSrcBase = 0,
                      bool _packedFaultSlots = false,
                      uint8_t _packedFaultCount = 0);

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

class VlWholeMicroInst : public VectorMemMicroInst
{
  protected:

    VlWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
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

class VsWholeMicroInst : public VectorMemMicroInst
{
  protected:

    VsWholeMicroInst(const char *mnem, ExtMachInst _machInst,
                     OpClass __opClass, uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
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

    VlStrideMicroInst(const char *mnem, ExtMachInst _machInst,
                      OpClass __opClass, uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
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

    VsStrideMicroInst(const char *mnem, ExtMachInst _machInst,
                      OpClass __opClass, uint8_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
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

    VlIndexMicroInst(const char *mnem, ExtMachInst _machInst,
                    OpClass __opClass, uint32_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
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

    VsIndexMicroInst(const char *mnem, ExtMachInst _machInst,
                    OpClass __opClass, uint32_t _microIdx)
        : VectorMemMicroInst(mnem, _machInst, __opClass, _microIdx)
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
                         OpClass __opClass, uint8_t _microIdx)
        : VectorArithMicroInst(mnem, _machInst, __opClass, _microIdx)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

template<typename ElemType>
class VregMergeMicroInst : public VectorArithMicroInst
{
  private:
    static constexpr size_t MAX_VTEMP_REGS = NumVecBufRegs;
    static constexpr size_t MAX_EXTRA_REGS = 6;  // oldDst(1) + vl(1) + vtype(1) + vstart(1) + vm(2)
    static constexpr size_t MAX_SRC_REGS = MAX_VTEMP_REGS + MAX_EXTRA_REGS;
    static constexpr size_t MAX_DEST_REGS = 1;

    RegId srcRegIdxArr[MAX_SRC_REGS];
    RegId destRegIdxArr[MAX_DEST_REGS];
    bool maskMerge = false;
    bool maskUseVm = true;

  public:
    VregMergeMicroInst(ExtMachInst extMachInst, uint8_t _numSrcs,
        VectorMicroInfo& _vmi, bool _maskMerge = false,
        uint8_t _srcStart = 0, bool _maskUseVm = true);

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData)
            const override;

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

/**
 * BitMaskMergeMicroInst: merge one VecBuf mask payload into architectural vd
 * at bit granularity (used by vlm load->vbuf->merge flow).
 */
class BitMaskMergeMicroInst : public VectorArithMicroInst
{
  private:
    static constexpr size_t MAX_SRC_REGS = 7;  // vbuf + oldDst + vl + vtype + optional vstart + optional vm
    static constexpr size_t MAX_DEST_REGS = 1;
    RegId srcRegIdxArr[MAX_SRC_REGS];
    RegId destRegIdxArr[MAX_DEST_REGS];
    bool maskUseVm;
    bool useVstart;
    bool roundUpVlToByte;

  public:
    BitMaskMergeMicroInst(ExtMachInst extMachInst,
                          VectorMicroInfo& _vmi,
                          uint8_t _srcStart = 0,
                          bool _maskUseVm = false,
                          bool _useVstart = false,
                          bool _roundUpVlToByte = false);

    Fault execute(ExecContext* xc, Trace::InstRecord* traceData) const override;

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VxsatMicroInst : public VectorArithMicroInst
{
  private:
    bool* vxsat;
  public:
    VxsatMicroInst(bool* Vxsat, ExtMachInst extMachInst)
        : VectorArithMicroInst("vxsat_micro", extMachInst,
          VectorArithOp, 0)
    {
        vxsat = Vxsat;
    }
    Fault execute(ExecContext* xc, Trace::InstRecord* traceData)
    const override
    {
        if (*vxsat) {
            xc->setMiscReg(MISCREG_VXSAT,*vxsat);
            auto vcsr = xc->readMiscReg(MISCREG_VCSR);
            xc->setMiscReg(MISCREG_VCSR, ((vcsr&~1)|*vxsat));
            // reset sat
            *vxsat = false;
        }
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


class VBufInsertMicroInst : public RiscvMicroInst
{
  private:
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];
    uint32_t offset;

  public:
    VBufInsertMicroInst(ExtMachInst machInst, RegIndex vbuf_idx,
                        RegIndex src_vec_idx, uint32_t _offset,
                        int expected_writes = 1);

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData)
            const override;

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VBufExtractMicroInst : public RiscvMicroInst
{
  private:
    RegId srcRegIdxArr[3];
    RegId destRegIdxArr[1];
    uint32_t offset;
    uint32_t bankIdx;
    bool mergeOldDest;

  public:
    VBufExtractMicroInst(ExtMachInst machInst, RegIndex vbuf_idx,
                         RegIndex dst_vec_idx, uint32_t _offset,
                         bool merge_old_dest = false);

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData)
            const override;

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

template<typename ElemType, typename AccType = ElemType, bool WidenSrc = false>
class VReducePartialMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<ElemType>,
                  "VReducePartialMicroInst requires integral ElemType");
    static_assert(std::is_integral_v<AccType>,
                  "VReducePartialMicroInst requires integral AccType");
    static constexpr size_t MAX_SRC = 4; // vs2 + vl + optional v0lo/v0hi
    static constexpr size_t MAX_DST = 1; // vbuf slot
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t slot;
    uint8_t totalSlots;

    static AccType
    identity(uint8_t funct6)
    {
        using AS = std::make_signed_t<AccType>;
        using AU = std::make_unsigned_t<AccType>;
        switch (funct6) {
          case 0x01: // and
            return static_cast<AccType>(~static_cast<AU>(0));
          case 0x02: // or
          case 0x03: // xor
          case 0x00: // sum
          case 0x30: // vwredsumu
          case 0x31: // vwredsum
            return static_cast<AccType>(0);
          case 0x04: // minu
            return static_cast<AccType>(std::numeric_limits<AU>::max());
          case 0x05: // min
            return static_cast<AccType>(std::numeric_limits<AS>::max());
          case 0x06: // maxu
            return static_cast<AccType>(0);
          case 0x07: // max
            return static_cast<AccType>(std::numeric_limits<AS>::lowest());
          default:
            return static_cast<AccType>(0);
        }
    }

    static AccType
    foldElem(AccType acc, ElemType rhs, uint8_t funct6)
    {
        using AS = std::make_signed_t<AccType>;
        using AU = std::make_unsigned_t<AccType>;
        using ES = std::make_signed_t<ElemType>;
        using EU = std::make_unsigned_t<ElemType>;
        switch (funct6) {
          case 0x00: // vredsum
            return static_cast<AccType>(acc + static_cast<AccType>(rhs));
          case 0x01: { // vredand
            AU a = static_cast<AU>(acc);
            AU b = static_cast<AU>(static_cast<AccType>(rhs));
            return static_cast<AccType>(a & b);
          }
          case 0x02: { // vredor
            AU a = static_cast<AU>(acc);
            AU b = static_cast<AU>(static_cast<AccType>(rhs));
            return static_cast<AccType>(a | b);
          }
          case 0x03: { // vredxor
            AU a = static_cast<AU>(acc);
            AU b = static_cast<AU>(static_cast<AccType>(rhs));
            return static_cast<AccType>(a ^ b);
          }
          case 0x04: { // vredminu
            AU a = static_cast<AU>(acc);
            AU b = static_cast<AU>(static_cast<AccType>(static_cast<EU>(rhs)));
            return static_cast<AccType>(std::min(a, b));
          }
          case 0x05: { // vredmin
            AS a = static_cast<AS>(acc);
            AS b = static_cast<AS>(static_cast<AccType>(static_cast<ES>(rhs)));
            return static_cast<AccType>(std::min(a, b));
          }
          case 0x06: { // vredmaxu
            AU a = static_cast<AU>(acc);
            AU b = static_cast<AU>(static_cast<AccType>(static_cast<EU>(rhs)));
            return static_cast<AccType>(std::max(a, b));
          }
          case 0x07: { // vredmax
            AS a = static_cast<AS>(acc);
            AS b = static_cast<AS>(static_cast<AccType>(static_cast<ES>(rhs)));
            return static_cast<AccType>(std::max(a, b));
          }
          case 0x30: // vwredsumu
            return static_cast<AccType>(
                static_cast<std::make_unsigned_t<AccType>>(acc) +
                static_cast<std::make_unsigned_t<AccType>>(
                    static_cast<std::make_unsigned_t<ElemType>>(rhs)));
          case 0x31: // vwredsum
            return static_cast<AccType>(
                static_cast<std::make_signed_t<AccType>>(acc) +
                static_cast<std::make_signed_t<AccType>>(
                    static_cast<std::make_signed_t<ElemType>>(rhs)));
          default:
            return static_cast<AccType>(acc + static_cast<AccType>(rhs));
        }
    }

    static AccType
    foldAcc(AccType lhs, AccType rhs, uint8_t funct6)
    {
        using AS = std::make_signed_t<AccType>;
        using AU = std::make_unsigned_t<AccType>;
        switch (funct6) {
          case 0x00:
          case 0x30:
          case 0x31:
            return static_cast<AccType>(lhs + rhs);
          case 0x01:
            return static_cast<AccType>(static_cast<AU>(lhs) &
                                        static_cast<AU>(rhs));
          case 0x02:
            return static_cast<AccType>(static_cast<AU>(lhs) |
                                        static_cast<AU>(rhs));
          case 0x03:
            return static_cast<AccType>(static_cast<AU>(lhs) ^
                                        static_cast<AU>(rhs));
          case 0x04:
            return static_cast<AccType>(
                std::min(static_cast<AU>(lhs), static_cast<AU>(rhs)));
          case 0x05:
            return static_cast<AccType>(
                std::min(static_cast<AS>(lhs), static_cast<AS>(rhs)));
          case 0x06:
            return static_cast<AccType>(
                std::max(static_cast<AU>(lhs), static_cast<AU>(rhs)));
          case 0x07:
            return static_cast<AccType>(
                std::max(static_cast<AS>(lhs), static_cast<AS>(rhs)));
          default:
            return static_cast<AccType>(lhs + rhs);
        }
    }

    static RegIndex
    reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

  public:
    VReducePartialMicroInst(ExtMachInst machInst, uint8_t _slot,
                            uint8_t _totalSlots, OpClass opClass)
        : VectorArithMicroInst("vred_partial_vbuf", machInst, opClass, _slot),
          slot(_slot),
          totalSlots(_totalSlots)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;

        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, slot)));
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        vlsrcIdx = 1;
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }

        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, slot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }

        vreg_t vs2_reg;
        xc->getRegOperand(this, 0, &vs2_reg);
        const auto vs2 = vs2_reg.as<ElemType>();
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t src_slice_offset =
            (lmul < 0) ? (micro_vlmax * (slot % frac_div)) : 0;

        AccType acc = identity(machInst.vfunct6);
        for (uint32_t i = 0; i < micro_vlmax; i++) {
            const uint32_t ei = i + micro_vlmax * slot;
            if (ei >= rVl) {
                break;
            }
            if (vm || elem_mask(v0, ei)) {
                acc = foldElem(acc, vs2[i + src_slice_offset], machInst.vfunct6);
            }
        }

        VecBufRegContainer out{};
        out.as<AccType>()[0] = acc;
        xc->setRegOperand(this, 0, &out);
        if (traceData) {
            traceData->setData(out);
        }
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0))
           << ", " << registerName(srcRegIdx(0))
           << ", slot=" << static_cast<unsigned>(slot)
           << "/" << static_cast<unsigned>(totalSlots);
        return ss.str();
    }
};

template<typename AccType>
class VReduceTreeMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<AccType>,
                  "VReduceTreeMicroInst requires integral AccType");
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];
    uint8_t dstSlot;
    uint8_t srcSlot;

    static AccType
    foldAcc(AccType lhs, AccType rhs, uint8_t funct6)
    {
        using AS = std::make_signed_t<AccType>;
        using AU = std::make_unsigned_t<AccType>;
        switch (funct6) {
          case 0x00:
          case 0x30:
          case 0x31:
            return static_cast<AccType>(lhs + rhs);
          case 0x01:
            return static_cast<AccType>(static_cast<AU>(lhs) &
                                        static_cast<AU>(rhs));
          case 0x02:
            return static_cast<AccType>(static_cast<AU>(lhs) |
                                        static_cast<AU>(rhs));
          case 0x03:
            return static_cast<AccType>(static_cast<AU>(lhs) ^
                                        static_cast<AU>(rhs));
          case 0x04:
            return static_cast<AccType>(
                std::min(static_cast<AU>(lhs), static_cast<AU>(rhs)));
          case 0x05:
            return static_cast<AccType>(
                std::min(static_cast<AS>(lhs), static_cast<AS>(rhs)));
          case 0x06:
            return static_cast<AccType>(
                std::max(static_cast<AU>(lhs), static_cast<AU>(rhs)));
          case 0x07:
            return static_cast<AccType>(
                std::max(static_cast<AS>(lhs), static_cast<AS>(rhs)));
          default:
            return static_cast<AccType>(lhs + rhs);
        }
    }

  public:
    VReduceTreeMicroInst(ExtMachInst machInst, uint8_t _dstSlot,
                         uint8_t _srcSlot, OpClass opClass,
                         uint8_t _microIdx = 0)
        : VectorArithMicroInst("vred_tree_vbuf", machInst, opClass, _microIdx),
          dstSlot(_dstSlot),
          srcSlot(_srcSlot)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, dstSlot));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, srcSlot));
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        VecBufRegContainer lhs_buf, rhs_buf;
        xc->getRegOperand(this, 0, &lhs_buf);
        xc->getRegOperand(this, 1, &rhs_buf);
        const AccType lhs = lhs_buf.as<AccType>()[0];
        const AccType rhs = rhs_buf.as<AccType>()[0];
        lhs_buf.as<AccType>()[0] = foldAcc(lhs, rhs, machInst.vfunct6);
        xc->setRegOperand(this, 0, &lhs_buf);
        if (traceData) {
            traceData->setData(lhs_buf);
        }
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0))
           << ", " << registerName(srcRegIdx(0))
           << ", " << registerName(srcRegIdx(1));
        return ss.str();
    }
};

template<typename AccType>
class VReduceFinalMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<AccType>,
                  "VReduceFinalMicroInst requires integral AccType");
    RegId srcRegIdxArr[4]; // vs1 + partial(vbuf) + vl + old vd
    RegId destRegIdxArr[1];

    static AccType
    foldAcc(AccType lhs, AccType rhs, uint8_t funct6)
    {
        using AS = std::make_signed_t<AccType>;
        using AU = std::make_unsigned_t<AccType>;
        switch (funct6) {
          case 0x00:
          case 0x30:
          case 0x31:
            return static_cast<AccType>(lhs + rhs);
          case 0x01:
            return static_cast<AccType>(static_cast<AU>(lhs) &
                                        static_cast<AU>(rhs));
          case 0x02:
            return static_cast<AccType>(static_cast<AU>(lhs) |
                                        static_cast<AU>(rhs));
          case 0x03:
            return static_cast<AccType>(static_cast<AU>(lhs) ^
                                        static_cast<AU>(rhs));
          case 0x04:
            return static_cast<AccType>(
                std::min(static_cast<AU>(lhs), static_cast<AU>(rhs)));
          case 0x05:
            return static_cast<AccType>(
                std::min(static_cast<AS>(lhs), static_cast<AS>(rhs)));
          case 0x06:
            return static_cast<AccType>(
                std::max(static_cast<AU>(lhs), static_cast<AU>(rhs)));
          case 0x07:
            return static_cast<AccType>(
                std::max(static_cast<AS>(lhs), static_cast<AS>(rhs)));
          default:
            return static_cast<AccType>(lhs + rhs);
        }
    }

  public:
    VReduceFinalMicroInst(ExtMachInst machInst, uint8_t partialSlot,
                          OpClass opClass, uint8_t _microIdx = 0)
        : VectorArithMicroInst("vred_final_vd", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vs1 * VregBanks));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, partialSlot));
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        oldDstIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        setDestRegIdx(_numDestRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        _numTypedDestRegs[VecRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }

        vreg_t vs1_reg;
        xc->getRegOperand(this, 0, &vs1_reg);
        const AccType seed = vs1_reg.as<AccType>()[0];

        VecBufRegContainer partial_buf;
        xc->getRegOperand(this, 1, &partial_buf);
        const AccType partial = partial_buf.as<AccType>()[0];
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        vreg_t old_vd;
        xc->getRegOperand(this, oldDstIdx, &old_vd);
        auto *dst = static_cast<VecRegContainer *>(
            xc->getWritableRegOperand(this, 0));
        memcpy(dst->as<uint8_t>(), old_vd.as<uint8_t>(), DPLENB);
        if (rVl > 0) {
            dst->as<AccType>()[0] = foldAcc(seed, partial, machInst.vfunct6);
        }
        xc->setRegOperand(this, 0, dst);
        if (traceData) {
            traceData->setData(*dst);
        }
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0))
           << ", " << registerName(srcRegIdx(0))
           << ", " << registerName(srcRegIdx(1));
        return ss.str();
    }
};

// ============================================================
// Integer parallel reduce micro-ops (FP-like ordering):
//   inter-register first (Leaf), then intra-lane tree (Tree), then Final
// ============================================================

// VReduceIntLeafMicroInst: pairs two source register banks lane-wise,
// produces a multi-lane VecBuf (one ElemType value per lane).
// Used for non-widening parallel integer reductions.
template<typename ElemType>
class VReduceIntLeafMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<ElemType>,
                  "VReduceIntLeafMicroInst requires integral ElemType");
    static constexpr size_t MAX_SRC = 5; // vs2a + vs2b + vl + optional v0lo/v0hi
    static constexpr size_t MAX_DST = 1;
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t srcSlotA;
    uint8_t srcSlotB;
    bool hasSecond;
    int vs2aSrcIdx = -1;
    int vs2bSrcIdx = -1;

    static RegIndex
    reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

    static ElemType
    identity(uint8_t funct6)
    {
        using ES = std::make_signed_t<ElemType>;
        using EU = std::make_unsigned_t<ElemType>;
        switch (funct6) {
          case 0x01: return static_cast<ElemType>(~static_cast<EU>(0));  // and
          case 0x02: case 0x03: case 0x00:                               // or/xor/sum
            return static_cast<ElemType>(0);
          case 0x04: return static_cast<ElemType>(std::numeric_limits<EU>::max()); // minu
          case 0x05: return static_cast<ElemType>(std::numeric_limits<ES>::max()); // min
          case 0x06: return static_cast<ElemType>(0);                              // maxu
          case 0x07: return static_cast<ElemType>(std::numeric_limits<ES>::lowest()); // max
          default:   return static_cast<ElemType>(0);
        }
    }

    static ElemType
    foldAcc(ElemType lhs, ElemType rhs, uint8_t funct6)
    {
        using ES = std::make_signed_t<ElemType>;
        using EU = std::make_unsigned_t<ElemType>;
        switch (funct6) {
          case 0x00: return static_cast<ElemType>(lhs + rhs);
          case 0x01: return static_cast<ElemType>(
              static_cast<EU>(lhs) & static_cast<EU>(rhs));
          case 0x02: return static_cast<ElemType>(
              static_cast<EU>(lhs) | static_cast<EU>(rhs));
          case 0x03: return static_cast<ElemType>(
              static_cast<EU>(lhs) ^ static_cast<EU>(rhs));
          case 0x04: return static_cast<ElemType>(
              std::min(static_cast<EU>(lhs), static_cast<EU>(rhs)));
          case 0x05: return static_cast<ElemType>(
              std::min(static_cast<ES>(lhs), static_cast<ES>(rhs)));
          case 0x06: return static_cast<ElemType>(
              std::max(static_cast<EU>(lhs), static_cast<EU>(rhs)));
          case 0x07: return static_cast<ElemType>(
              std::max(static_cast<ES>(lhs), static_cast<ES>(rhs)));
          default:   return static_cast<ElemType>(lhs + rhs);
        }
    }

  public:
    VReduceIntLeafMicroInst(ExtMachInst machInst, uint8_t dstSlot,
                            uint8_t _srcSlotA, uint8_t _srcSlotB,
                            bool _hasSecond, OpClass opClass,
                            uint8_t _microIdx = 0)
        : VectorArithMicroInst("vred_int_leaf_vbuf", machInst, opClass, _microIdx),
          srcSlotA(_srcSlotA),
          srcSlotB(_srcSlotB),
          hasSecond(_hasSecond)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        vs2aSrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, srcSlotA)));
        if (hasSecond) {
            vs2bSrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, srcSlotB)));
        }
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill)
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);

        vreg_t vs2_a_reg;
        xc->getRegOperand(this, vs2aSrcIdx, &vs2_a_reg);
        const ElemType *vs2a = vs2_a_reg.as<ElemType>();

        vreg_t vs2_b_reg;
        const ElemType *vs2b = nullptr;
        if (hasSecond) {
            xc->getRegOperand(this, vs2bSrcIdx, &vs2_b_reg);
            vs2b = vs2_b_reg.as<ElemType>();
        }

        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);
        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t offset_a = (lmul < 0) ? (micro_vlmax * (srcSlotA % frac_div)) : 0;
        const uint32_t offset_b = (lmul < 0) ? (micro_vlmax * (srcSlotB % frac_div)) : 0;

        const uint32_t lanes_per_slot = DPLENB / sizeof(ElemType);
        const ElemType id = identity(machInst.vfunct6);

        VecBufRegContainer out{};
        ElemType *out_lanes = out.as<ElemType>();
        for (uint32_t i = 0; i < lanes_per_slot; ++i) {
            out_lanes[i] = id;
        }

        for (uint32_t i = 0; i < micro_vlmax && i < lanes_per_slot; ++i) {
            ElemType acc = id;
            const uint32_t ei_a = i + micro_vlmax * srcSlotA;
            if (ei_a < rVl && (vm || elem_mask(v0, ei_a)))
                acc = foldAcc(acc, vs2a[i + offset_a], machInst.vfunct6);
            if (hasSecond) {
                const uint32_t ei_b = i + micro_vlmax * srcSlotB;
                if (ei_b < rVl && (vm || elem_mask(v0, ei_b)))
                    acc = foldAcc(acc, vs2b[i + offset_b], machInst.vfunct6);
            }
            out_lanes[i] = acc;
        }

        xc->setRegOperand(this, 0, &out);
        if (traceData) traceData->setData(out);
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0))
           << ", slot_a=" << static_cast<unsigned>(srcSlotA)
           << ", slot_b=" << static_cast<unsigned>(srcSlotB);
        return ss.str();
    }
};

// VReduceIntParallelTreeMicroInst: lane-wise combination of two multi-lane
// VecBufs for non-widening integer reductions.
template<typename ElemType>
class VReduceIntParallelTreeMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<ElemType>,
                  "VReduceIntParallelTreeMicroInst requires integral ElemType");
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];

    static ElemType
    foldAcc(ElemType lhs, ElemType rhs, uint8_t funct6)
    {
        using ES = std::make_signed_t<ElemType>;
        using EU = std::make_unsigned_t<ElemType>;
        switch (funct6) {
          case 0x00: return static_cast<ElemType>(lhs + rhs);
          case 0x01: return static_cast<ElemType>(
              static_cast<EU>(lhs) & static_cast<EU>(rhs));
          case 0x02: return static_cast<ElemType>(
              static_cast<EU>(lhs) | static_cast<EU>(rhs));
          case 0x03: return static_cast<ElemType>(
              static_cast<EU>(lhs) ^ static_cast<EU>(rhs));
          case 0x04: return static_cast<ElemType>(
              std::min(static_cast<EU>(lhs), static_cast<EU>(rhs)));
          case 0x05: return static_cast<ElemType>(
              std::min(static_cast<ES>(lhs), static_cast<ES>(rhs)));
          case 0x06: return static_cast<ElemType>(
              std::max(static_cast<EU>(lhs), static_cast<EU>(rhs)));
          case 0x07: return static_cast<ElemType>(
              std::max(static_cast<ES>(lhs), static_cast<ES>(rhs)));
          default:   return static_cast<ElemType>(lhs + rhs);
        }
    }

  public:
    VReduceIntParallelTreeMicroInst(ExtMachInst machInst, uint8_t dstSlot,
                                    uint8_t srcSlot, OpClass opClass,
                                    uint8_t _microIdx = 0)
        : VectorArithMicroInst("vred_int_ptree_vbuf", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, dstSlot));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, srcSlot));
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        VecBufRegContainer lhs, rhs;
        xc->getRegOperand(this, 0, &lhs);
        xc->getRegOperand(this, 1, &rhs);
        ElemType *lv = lhs.as<ElemType>();
        const ElemType *rv = rhs.as<ElemType>();
        const uint32_t lane_count = DPLENB / sizeof(ElemType);
        for (uint32_t i = 0; i < lane_count; ++i)
            lv[i] = foldAcc(lv[i], rv[i], machInst.vfunct6);
        xc->setRegOperand(this, 0, &lhs);
        if (traceData) traceData->setData(lhs);
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0))
           << ", " << registerName(srcRegIdx(0))
           << ", " << registerName(srcRegIdx(1));
        return ss.str();
    }
};

// VReduceIntIntraLaneMicroInst: reduces the multi-lane VecBuf[partialSlot]
// to a single scalar via an internal binary-tree and writes it back to
// VecBuf[partialSlot][0].  This is element-wise work within a VecBuf slot,
// so it uses VectorReductionOp.  It is followed by VReduceFinalMicroInst
// (VectorArithOp) which combines the scalar result with vs1[0] and writes vd.
template<typename ElemType>
class VReduceIntIntraLaneMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<ElemType>,
                  "VReduceIntIntraLaneMicroInst requires integral ElemType");
    RegId srcRegIdxArr[1];
    RegId destRegIdxArr[1];

    static ElemType
    foldAcc(ElemType lhs, ElemType rhs, uint8_t funct6)
    {
        using ES = std::make_signed_t<ElemType>;
        using EU = std::make_unsigned_t<ElemType>;
        switch (funct6) {
          case 0x00: return static_cast<ElemType>(lhs + rhs);
          case 0x01: return static_cast<ElemType>(
              static_cast<EU>(lhs) & static_cast<EU>(rhs));
          case 0x02: return static_cast<ElemType>(
              static_cast<EU>(lhs) | static_cast<EU>(rhs));
          case 0x03: return static_cast<ElemType>(
              static_cast<EU>(lhs) ^ static_cast<EU>(rhs));
          case 0x04: return static_cast<ElemType>(
              std::min(static_cast<EU>(lhs), static_cast<EU>(rhs)));
          case 0x05: return static_cast<ElemType>(
              std::min(static_cast<ES>(lhs), static_cast<ES>(rhs)));
          case 0x06: return static_cast<ElemType>(
              std::max(static_cast<EU>(lhs), static_cast<EU>(rhs)));
          case 0x07: return static_cast<ElemType>(
              std::max(static_cast<ES>(lhs), static_cast<ES>(rhs)));
          default:   return static_cast<ElemType>(lhs + rhs);
        }
    }

  public:
    VReduceIntIntraLaneMicroInst(ExtMachInst machInst, uint8_t partialSlot,
                                 OpClass opClass, uint8_t _microIdx = 0)
        : VectorArithMicroInst("vred_int_intra_vbuf", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, partialSlot));
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, partialSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        VecBufRegContainer partial_buf;
        xc->getRegOperand(this, 0, &partial_buf);
        ElemType *lanes = partial_buf.as<ElemType>();

        const uint32_t lane_count = DPLENB / sizeof(ElemType);
        uint32_t active = lane_count;
        while (active > 1) {
            for (uint32_t i = 0; i < active / 2; ++i)
                lanes[i] = foldAcc(lanes[i], lanes[i + active / 2],
                                   machInst.vfunct6);
            active >>= 1;
        }

        xc->setRegOperand(this, 0, &partial_buf);
        if (traceData) traceData->setData(partial_buf);
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0));
        return ss.str();
    }
};

// VReduceIntScalarLeafMicroInst: reads TWO ElemType register banks and
// fully reduces all their elements into a single AccType scalar.
// Used for widening parallel integer reductions (vwredsumu/vwredsum),
// where the output scalar is stored in VecBuf[dstSlot][0].
template<typename ElemType, typename AccType>
class VReduceIntScalarLeafMicroInst : public VectorArithMicroInst
{
  private:
    static_assert(std::is_integral_v<ElemType>,
                  "VReduceIntScalarLeafMicroInst requires integral ElemType");
    static_assert(std::is_integral_v<AccType>,
                  "VReduceIntScalarLeafMicroInst requires integral AccType");
    static constexpr size_t MAX_SRC = 5;
    static constexpr size_t MAX_DST = 1;
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t srcSlotA;
    uint8_t srcSlotB;
    bool hasSecond;
    int vs2aSrcIdx = -1;
    int vs2bSrcIdx = -1;

    static RegIndex
    reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

    static AccType
    identity(uint8_t funct6)
    {
        return static_cast<AccType>(0); // only sum (vwredsumu/vwredsum) uses widening
    }

    static AccType
    foldElem(AccType acc, ElemType rhs, uint8_t funct6)
    {
        using AU = std::make_unsigned_t<AccType>;
        using EU = std::make_unsigned_t<ElemType>;
        if (funct6 == 0x30) { // vwredsumu: zero-extend
            return static_cast<AccType>(
                static_cast<AU>(acc) + static_cast<AU>(static_cast<EU>(rhs)));
        }
        // funct6 == 0x31: vwredsum: sign-extend
        return static_cast<AccType>(
            static_cast<std::make_signed_t<AccType>>(acc) +
            static_cast<std::make_signed_t<AccType>>(
                static_cast<std::make_signed_t<ElemType>>(rhs)));
    }

  public:
    VReduceIntScalarLeafMicroInst(ExtMachInst machInst, uint8_t dstSlot,
                                  uint8_t _srcSlotA, uint8_t _srcSlotB,
                                  bool _hasSecond, OpClass opClass,
                                  uint8_t _microIdx = 0)
        : VectorArithMicroInst("vred_int_sleaf_vbuf", machInst, opClass, _microIdx),
          srcSlotA(_srcSlotA),
          srcSlotB(_srcSlotB),
          hasSecond(_hasSecond)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        vs2aSrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, srcSlotA)));
        if (hasSecond) {
            vs2bSrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, srcSlotB)));
        }
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill)
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);

        vreg_t vs2_a_reg;
        xc->getRegOperand(this, vs2aSrcIdx, &vs2_a_reg);
        const ElemType *vs2a = vs2_a_reg.as<ElemType>();

        vreg_t vs2_b_reg;
        const ElemType *vs2b = nullptr;
        if (hasSecond) {
            xc->getRegOperand(this, vs2bSrcIdx, &vs2_b_reg);
            vs2b = vs2_b_reg.as<ElemType>();
        }

        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);
        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t offset_a = (lmul < 0) ? (micro_vlmax * (srcSlotA % frac_div)) : 0;
        const uint32_t offset_b = (lmul < 0) ? (micro_vlmax * (srcSlotB % frac_div)) : 0;

        AccType acc = identity(machInst.vfunct6);
        for (uint32_t i = 0; i < micro_vlmax; ++i) {
            const uint32_t ei_a = i + micro_vlmax * srcSlotA;
            if (ei_a >= rVl) break;
            if (vm || elem_mask(v0, ei_a))
                acc = foldElem(acc, vs2a[i + offset_a], machInst.vfunct6);
        }
        if (hasSecond) {
            for (uint32_t i = 0; i < micro_vlmax; ++i) {
                const uint32_t ei_b = i + micro_vlmax * srcSlotB;
                if (ei_b >= rVl) break;
                if (vm || elem_mask(v0, ei_b))
                    acc = foldElem(acc, vs2b[i + offset_b], machInst.vfunct6);
            }
        }

        VecBufRegContainer out{};
        out.as<AccType>()[0] = acc;
        xc->setRegOperand(this, 0, &out);
        if (traceData) traceData->setData(out);
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
        const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " " << registerName(destRegIdx(0))
           << ", slot_a=" << static_cast<unsigned>(srcSlotA)
           << ", slot_b=" << static_cast<unsigned>(srcSlotB);
        return ss.str();
    }
};

template<typename ElemType, typename AccType = ElemType, bool WidenSrc = false>
class VParallelReduceIntMacroInst : public VectorArithMacroInst
{
  public:
    VParallelReduceIntMacroInst(const char* mnem, ExtMachInst extMachInst,
                                OpClass opClass)
        : VectorArithMacroInst(mnem, extMachInst, opClass)
    {
        const uint32_t numSlots = calculateActiveBanks(vflmul);
        panic_if(numSlots > NumVecBufRegs,
                 "VParallelReduceIntMacroInst slots=%u exceeds vecbuf=%u",
                 numSlots, NumVecBufRegs);

        StaticInstPtr microop;
        uint8_t micro_idx = 0;
        for (uint8_t s = 0; s < numSlots; ++s) {
            microop = new VReducePartialMicroInst<ElemType, AccType, WidenSrc>(
                extMachInst, s, numSlots, opClass);
            microop->setDelayedCommit();
            this->microops.push_back(microop);
            micro_idx++;
        }

        for (uint8_t stride = 1; stride < numSlots; stride <<= 1) {
            for (uint8_t s = 0; (s + stride) < numSlots; s += (stride << 1)) {
                microop = new VReduceTreeMicroInst<AccType>(
                    extMachInst, s, s + stride, VectorArithOp, micro_idx++);
                microop->setDelayedCommit();
                this->microops.push_back(microop);
            }
        }

        microop = new VReduceFinalMicroInst<AccType>(
            extMachInst, 0, VectorArithOp, micro_idx++);
        microop->setDelayedCommit();
        this->microops.push_back(microop);

        this->microops.front()->setFirstMicroop();
        this->microops.back()->setLastMicroop();
    }

    std::string generateDisassembly(Addr pc,
            const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << " v" << static_cast<uint32_t>(machInst.vd)
           << ", v" << static_cast<uint32_t>(machInst.vs2)
           << ", v" << static_cast<uint32_t>(machInst.vs1);
        if (!machInst.vm) {
            ss << ", v0.t";
        }
        return ss.str();
    }
};

template<typename ElemType, typename AccType = ElemType, bool WidenSrc = false>
class VReduceFPPartialMicroInst : public VectorArithMicroInst
{
  private:
    static constexpr size_t MAX_SRC = 4; // vs2 + vl + optional v0lo/v0hi
    static constexpr size_t MAX_DST = 1;
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t slot;
    uint8_t totalSlots;

    static RegIndex reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

    static decltype(AccType::v)
    fold(decltype(AccType::v) acc, decltype(ElemType::v) rhs)
    {
        if constexpr (WidenSrc) {
            return fadd<AccType>(ftype<AccType>(acc),
                                 f_to_wf<ElemType>(ftype<ElemType>(rhs))).v;
        } else {
            return fadd<AccType>(ftype<AccType>(acc),
                                 ftype<AccType>(static_cast<decltype(AccType::v)>(rhs))).v;
        }
    }

  public:
    VReduceFPPartialMicroInst(ExtMachInst machInst, uint8_t _slot,
                              uint8_t _totalSlots, OpClass opClass)
        : VectorArithMicroInst("vfred_partial_vbuf", machInst, opClass, _slot),
          slot(_slot),
          totalSlots(_totalSlots)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, slot)));
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        vlsrcIdx = 1;
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, slot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;

        vreg_t vs2_reg;
        xc->getRegOperand(this, 0, &vs2_reg);
        const auto vs2 = vs2_reg.as<decltype(ElemType::v)>();
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t src_slice_offset =
            (lmul < 0) ? (micro_vlmax * (slot % frac_div)) : 0;

        using AccBits = decltype(AccType::v);
        const AccBits zero = static_cast<AccBits>(0);
        decltype(AccType::v) acc = ftype<AccType>(zero).v;
        for (uint32_t i = 0; i < micro_vlmax; i++) {
            const uint32_t ei = i + micro_vlmax * slot;
            if (ei >= rVl) {
                break;
            }
            if (vm || elem_mask(v0, ei)) {
                acc = fold(acc, vs2[i + src_slice_offset]);
            }
        }

        VecBufRegContainer out{};
        out.as<decltype(AccType::v)>()[0] = acc;
        xc->setRegOperand(this, 0, &out);
        if (traceData) {
            traceData->setData(out);
        }
        return NoFault;
    }

};

template<typename ElemType>
class VReduceFPParallelPartialMicroInst : public VectorArithMicroInst
{
  private:
    static constexpr size_t MAX_SRC = 4; // vs2 + vl + optional v0lo/v0hi
    static constexpr size_t MAX_DST = 1;
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t slot;

    static RegIndex
    reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

    static constexpr decltype(ElemType::v)
    negZeroBits()
    {
        using Bits = decltype(ElemType::v);
        if constexpr (sizeof(Bits) == sizeof(uint32_t)) {
            return static_cast<Bits>(0x80000000u);
        } else {
            return static_cast<Bits>(0x8000000000000000ull);
        }
    }

    static constexpr decltype(ElemType::v)
    posInfBits()
    {
        using Bits = decltype(ElemType::v);
        if constexpr (sizeof(Bits) == sizeof(uint32_t)) {
            return static_cast<Bits>(0x7f800000u);
        } else {
            return static_cast<Bits>(0x7ff0000000000000ull);
        }
    }

    static constexpr decltype(ElemType::v)
    negInfBits()
    {
        using Bits = decltype(ElemType::v);
        if constexpr (sizeof(Bits) == sizeof(uint32_t)) {
            return static_cast<Bits>(0xff800000u);
        } else {
            return static_cast<Bits>(0xfff0000000000000ull);
        }
    }

    static decltype(ElemType::v)
    reduceIdentity(const ExtMachInst &machInst)
    {
        switch (machInst.vfunct6) {
          case 0x01: // vfredusum.vs
          case 0x03: // vfredosum.vs
            return negZeroBits();
          case 0x05: // vfredmin.vs
            return posInfBits();
          case 0x07: // vfredmax.vs
            return negInfBits();
          default:
            panic("Unsupported FP reduce funct6 %#x in parallel partial",
                  machInst.vfunct6);
        }
    }

  public:
    VReduceFPParallelPartialMicroInst(ExtMachInst machInst, uint8_t _slot,
                                      uint8_t _totalSlots, OpClass opClass)
        : VectorArithMicroInst("vfred_partial_vbuf", machInst, opClass, _slot),
          slot(_slot)
    {
        (void)_totalSlots;
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, slot)));
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        vlsrcIdx = 1;
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, slot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }

        vreg_t vs2_reg;
        xc->getRegOperand(this, 0, &vs2_reg);
        const auto vs2 = vs2_reg.as<decltype(ElemType::v)>();
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t src_slice_offset =
            (lmul < 0) ? (micro_vlmax * (slot % frac_div)) : 0;

        VecBufRegContainer out{};
        auto out_lanes = out.as<decltype(ElemType::v)>();
        const uint32_t lanes_per_slot = DPLEN / (8 * sizeof(decltype(ElemType::v)));
        const auto id = reduceIdentity(machInst);
        for (uint32_t i = 0; i < lanes_per_slot; ++i) {
            out_lanes[i] = id;
        }
        for (uint32_t i = 0; i < micro_vlmax && i < lanes_per_slot; ++i) {
            const uint32_t ei = i + micro_vlmax * slot;
            if (ei >= rVl) {
                break;
            }
            if (vm || elem_mask(v0, ei)) {
                out_lanes[i] = vs2[i + src_slice_offset];
            }
        }

        xc->setRegOperand(this, 0, &out);
        if (traceData) {
            traceData->setData(out);
        }
        return NoFault;
    }
};

template<typename ElemType>
class VReduceFPParallelLeafMicroInst : public VectorArithMicroInst
{
  private:
    static constexpr size_t MAX_SRC = 5; // vs2(a/b) + vl + optional v0(lo/hi)
    static constexpr size_t MAX_DST = 1;
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t srcSlotA;
    uint8_t srcSlotB;
    bool hasSecond;
    int vs2aSrcIdx = -1;
    int vs2bSrcIdx = -1;

    static RegIndex
    reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

    static constexpr decltype(ElemType::v)
    negZeroBits()
    {
        using Bits = decltype(ElemType::v);
        if constexpr (sizeof(Bits) == sizeof(uint32_t)) {
            return static_cast<Bits>(0x80000000u);
        } else {
            return static_cast<Bits>(0x8000000000000000ull);
        }
    }

    static constexpr decltype(ElemType::v)
    posInfBits()
    {
        using Bits = decltype(ElemType::v);
        if constexpr (sizeof(Bits) == sizeof(uint32_t)) {
            return static_cast<Bits>(0x7f800000u);
        } else {
            return static_cast<Bits>(0x7ff0000000000000ull);
        }
    }

    static constexpr decltype(ElemType::v)
    negInfBits()
    {
        using Bits = decltype(ElemType::v);
        if constexpr (sizeof(Bits) == sizeof(uint32_t)) {
            return static_cast<Bits>(0xff800000u);
        } else {
            return static_cast<Bits>(0xfff0000000000000ull);
        }
    }

    static decltype(ElemType::v)
    reduceIdentity(const ExtMachInst &machInst)
    {
        switch (machInst.vfunct6) {
          case 0x01: // vfredusum.vs
          case 0x03: // vfredosum.vs
            return negZeroBits();
          case 0x05: // vfredmin.vs
            return posInfBits();
          case 0x07: // vfredmax.vs
            return negInfBits();
          default:
            panic("Unsupported FP reduce funct6 %#x in parallel leaf",
                  machInst.vfunct6);
        }
    }

    static decltype(ElemType::v)
    reduceStep(const ExtMachInst &machInst,
               decltype(ElemType::v) lhs,
               decltype(ElemType::v) rhs)
    {
        switch (machInst.vfunct6) {
          case 0x01: // vfredusum.vs
          case 0x03: // vfredosum.vs
            return fadd<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          case 0x05: // vfredmin.vs
            return fmin<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          case 0x07: // vfredmax.vs
            return fmax<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          default:
            panic("Unsupported FP reduce funct6 %#x in parallel leaf step",
                  machInst.vfunct6);
        }
    }

  public:
    VReduceFPParallelLeafMicroInst(ExtMachInst machInst, uint8_t dstSlot,
                                   uint8_t _srcSlotA, uint8_t _srcSlotB,
                                   bool _hasSecond, OpClass opClass,
                                   uint8_t _microIdx = 0)
        : VectorArithMicroInst("vfred_leaf_vbuf", machInst, opClass, _microIdx),
          srcSlotA(_srcSlotA),
          srcSlotB(_srcSlotB),
          hasSecond(_hasSecond)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        vs2aSrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, srcSlotA)));
        if (hasSecond) {
            vs2bSrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, reduceSrcReg(machInst, srcSlotB)));
        }
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;

        using LaneBits = decltype(ElemType::v);
        vreg_t vs2_a_reg;
        xc->getRegOperand(this, vs2aSrcIdx, &vs2_a_reg);
        const LaneBits *vs2a = vs2_a_reg.as<LaneBits>();

        vreg_t vs2_b_reg;
        const LaneBits *vs2b = nullptr;
        if (hasSecond) {
            xc->getRegOperand(this, vs2bSrcIdx, &vs2_b_reg);
            vs2b = vs2_b_reg.as<LaneBits>();
        }

        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);
        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t src_slice_offset_a =
            (lmul < 0) ? (micro_vlmax * (srcSlotA % frac_div)) : 0;
        const uint32_t src_slice_offset_b =
            (lmul < 0) ? (micro_vlmax * (srcSlotB % frac_div)) : 0;

        VecBufRegContainer out{};
        auto out_lanes = out.as<decltype(ElemType::v)>();
        const uint32_t lanes_per_slot = DPLEN / (8 * sizeof(decltype(ElemType::v)));
        const auto id = reduceIdentity(machInst);
        for (uint32_t i = 0; i < lanes_per_slot; ++i) {
            out_lanes[i] = id;
        }

        for (uint32_t i = 0; i < micro_vlmax && i < lanes_per_slot; ++i) {
            const uint32_t ei_a = i + micro_vlmax * srcSlotA;
            auto lhs = id;
            if (ei_a < rVl && (vm || elem_mask(v0, ei_a))) {
                lhs = vs2a[i + src_slice_offset_a];
            }

            if (hasSecond) {
                const uint32_t ei_b = i + micro_vlmax * srcSlotB;
                auto rhs = id;
                if (ei_b < rVl && (vm || elem_mask(v0, ei_b))) {
                    rhs = vs2b[i + src_slice_offset_b];
                }
                out_lanes[i] = reduceStep(machInst, lhs, rhs);
            } else {
                out_lanes[i] = lhs;
            }
        }

        xc->setRegOperand(this, 0, &out);
        if (traceData) {
            traceData->setData(out);
        }
        return NoFault;
    }
};

template<typename ElemType>
class VReduceFPSeqStepMicroInst : public VectorArithMicroInst
{
  private:
    static constexpr size_t MAX_SRC = 6; // vs1 + vs2 + carry + vl + v0(lo/hi)
    static constexpr size_t MAX_DST = 1;
    RegId srcRegIdxArr[MAX_SRC];
    RegId destRegIdxArr[MAX_DST];
    uint8_t slot;
    int carrySrcIdx = -1;

    static RegIndex
    reduceSrcReg(const ExtMachInst &machInst, uint8_t slot)
    {
        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        if (lmul < 0) {
            const uint32_t frac_div = 1u << (-lmul);
            const uint32_t arch = slot / (frac_div * VregBanks);
            const uint32_t bank = (slot / frac_div) % VregBanks;
            return (machInst.vs2 + arch) * VregBanks + bank;
        }
        const uint32_t arch = slot / VregBanks;
        const uint32_t bank = slot % VregBanks;
        return (machInst.vs2 + arch) * VregBanks + bank;
    }

    static decltype(ElemType::v)
    reduceStep(const ExtMachInst &machInst,
               decltype(ElemType::v) acc,
               decltype(ElemType::v) rhs)
    {
        switch (machInst.vfunct6) {
          case 0x01: // vfredusum.vs
          case 0x03: // vfredosum.vs
            return fadd<ElemType>(ftype<ElemType>(acc),
                                  ftype<ElemType>(rhs)).v;
          case 0x05: // vfredmin.vs
            return fmin<ElemType>(ftype<ElemType>(acc),
                                  ftype<ElemType>(rhs)).v;
          case 0x07: // vfredmax.vs
            return fmax<ElemType>(ftype<ElemType>(acc),
                                  ftype<ElemType>(rhs)).v;
          default:
            panic("Unsupported FP reduce funct6 %#x in sequential step",
                  machInst.vfunct6);
        }
    }

  public:
    VReduceFPSeqStepMicroInst(ExtMachInst machInst, uint8_t _slot,
                              OpClass opClass)
        : VectorArithMicroInst("vfred_seq_step_vbuf", machInst, opClass, _slot),
          slot(_slot)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        if (slot == 0) {
            vs1srcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++,
                RegId(VecRegClass, machInst.vs1 * VregBanks));
        }
        vs2srcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++,
            RegId(VecRegClass, reduceSrcReg(machInst, slot)));
        if (slot != 0) {
            carrySrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, 0));
        }
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, 0));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;

        vreg_t vs2_reg;
        xc->getRegOperand(this, vs2srcIdx, &vs2_reg);
        const auto vs2 = vs2_reg.as<decltype(ElemType::v)>();

        decltype(ElemType::v) acc;
        if (slot == 0) {
            vreg_t vs1_reg;
            xc->getRegOperand(this, vs1srcIdx, &vs1_reg);
            acc = vs1_reg.as<decltype(ElemType::v)>()[0];
        } else {
            VecBufRegContainer carry;
            xc->getRegOperand(this, carrySrcIdx, &carry);
            acc = carry.as<decltype(ElemType::v)>()[0];
        }

        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);
        uint8_t tmp_v0_storage[VLENB] = {0};
        uint8_t *v0 = nullptr;
        if (!vm) {
            for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                vreg_t v0_bank;
                xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
            }
            v0 = tmp_v0_storage;
        }

        const int8_t lmul = vtype_vlmul(machInst.vtype8);
        const uint32_t micro_vlmax = DPLEN / sew * (vflmul >= 1 ? 1 : vflmul);
        const uint32_t frac_div = (lmul < 0) ? (1u << (-lmul)) : 1u;
        const uint32_t src_slice_offset =
            (lmul < 0) ? (micro_vlmax * (slot % frac_div)) : 0;

        for (uint32_t i = 0; i < micro_vlmax; i++) {
            const uint32_t ei = i + micro_vlmax * slot;
            if (ei >= rVl) {
                break;
            }
            if (vm || elem_mask(v0, ei)) {
                const auto rhs = vs2[i + src_slice_offset];
                acc = reduceStep(machInst, acc, rhs);
            }
        }

        VecBufRegContainer out{};
        out.as<decltype(ElemType::v)>()[0] = acc;
        xc->setRegOperand(this, 0, &out);
        if (traceData) {
            traceData->setData(out);
        }
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
            const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", ";
        if (slot == 0) {
            // First step consumes seed(vs1[0]) and first vs2 slice.
            ss << registerName(srcRegIdx(vs2srcIdx)) << ", "
               << registerName(srcRegIdx(vs1srcIdx));
        } else {
            // Later steps keep ISA operand order: vd, vs2, vs1(carry).
            ss << registerName(srcRegIdx(vs2srcIdx)) << ", "
               << registerName(srcRegIdx(carrySrcIdx));
        }
        if (!machInst.vm) {
            ss << ", v0.t";
        }
        return ss.str();
    }
};

template<typename ElemType>
class VReduceFPSeqFinalMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[3]; // carry(vbuf) + vl + old vd
    RegId destRegIdxArr[1];
  public:
    VReduceFPSeqFinalMicroInst(ExtMachInst machInst, OpClass opClass,
                               uint8_t _microIdx = 0)
        : VectorArithMicroInst("vfred_seq_final_vd", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, 0));
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        oldDstIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        setDestRegIdx(_numDestRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        _numTypedDestRegs[VecRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        VecBufRegContainer carry_buf;
        xc->getRegOperand(this, 0, &carry_buf);
        const auto carry = carry_buf.as<decltype(ElemType::v)>()[0];

        vreg_t old_vd;
        xc->getRegOperand(this, oldDstIdx, &old_vd);
        auto *dst = static_cast<VecRegContainer *>(
            xc->getWritableRegOperand(this, 0));
        memcpy(dst->as<uint8_t>(), old_vd.as<uint8_t>(), DPLENB);
        if (rVl > 0) {
            dst->as<decltype(ElemType::v)>()[0] = carry;
        }
        xc->setRegOperand(this, 0, dst);
        if (traceData) {
            traceData->setData(*dst);
        }
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
            const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        // Final step only commits the reduction carry to vd[0].
        ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", "
           << registerName(srcRegIdx(0));
        return ss.str();
    }
};

template<typename ElemType>
class VReduceFPParallelTreeMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];
    static decltype(ElemType::v)
    reduceStep(const ExtMachInst &machInst,
               decltype(ElemType::v) lhs,
               decltype(ElemType::v) rhs)
    {
        switch (machInst.vfunct6) {
          case 0x01: // vfredusum.vs
          case 0x03: // vfredosum.vs
            return fadd<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          case 0x05: // vfredmin.vs
            return fmin<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          case 0x07: // vfredmax.vs
            return fmax<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          default:
            panic("Unsupported FP reduce funct6 %#x in parallel tree",
                  machInst.vfunct6);
        }
    }
  public:
    VReduceFPParallelTreeMicroInst(ExtMachInst machInst, uint8_t dstSlot,
                                   uint8_t srcSlot, OpClass opClass,
                                   uint8_t _microIdx = 0)
        : VectorArithMicroInst("vfred_tree_vbuf", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, dstSlot));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, srcSlot));
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;

        VecBufRegContainer lhs, rhs;
        xc->getRegOperand(this, 0, &lhs);
        xc->getRegOperand(this, 1, &rhs);
        auto lv = lhs.as<decltype(ElemType::v)>();
        auto rv = rhs.as<decltype(ElemType::v)>();
        const uint32_t lane_count = DPLEN / (8 * sizeof(decltype(ElemType::v)));
        for (uint32_t i = 0; i < lane_count; ++i) {
            lv[i] = reduceStep(machInst, lv[i], rv[i]);
        }
        xc->setRegOperand(this, 0, &lhs);
        if (traceData) {
            traceData->setData(lhs);
        }
        return NoFault;
    }
};

template<typename ElemType>
class VReduceFPParallelFinalMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[6]; // vs1 + partial(vbuf) + vl + old vd + optional v0(lo/hi)
    RegId destRegIdxArr[1];
    static decltype(ElemType::v)
    reduceStep(const ExtMachInst &machInst,
               decltype(ElemType::v) lhs,
               decltype(ElemType::v) rhs)
    {
        switch (machInst.vfunct6) {
          case 0x01: // vfredusum.vs
          case 0x03: // vfredosum.vs
            return fadd<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          case 0x05: // vfredmin.vs
            return fmin<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          case 0x07: // vfredmax.vs
            return fmax<ElemType>(ftype<ElemType>(lhs),
                                  ftype<ElemType>(rhs)).v;
          default:
            panic("Unsupported FP reduce funct6 %#x in parallel final",
                  machInst.vfunct6);
        }
    }
  public:
    VReduceFPParallelFinalMicroInst(ExtMachInst machInst, uint8_t partialSlot,
                                    OpClass opClass, uint8_t _microIdx = 0)
        : VectorArithMicroInst("vfred_final_vd", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vs1 * VregBanks));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, partialSlot));
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        if (!vm) {
            vmsrcIdx = _numSrcRegs;
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 0));
            setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, 1));
        }
        oldDstIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        setDestRegIdx(_numDestRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        _numTypedDestRegs[VecRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;

        vreg_t vs1_reg;
        xc->getRegOperand(this, 0, &vs1_reg);
        const auto seed = vs1_reg.as<decltype(ElemType::v)>()[0];

        VecBufRegContainer partial_buf;
        xc->getRegOperand(this, 1, &partial_buf);
        auto partial_lanes = partial_buf.as<decltype(ElemType::v)>();
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        bool hasActiveElem = false;
        if (rVl > 0) {
            if (vm) {
                hasActiveElem = true;
            } else {
                uint8_t tmp_v0_storage[VLENB] = {0};
                for (uint32_t _b = 0; _b < VregBanks; ++_b) {
                    vreg_t v0_bank;
                    xc->getRegOperand(this, vmsrcIdx + _b, &v0_bank);
                    memcpy(tmp_v0_storage + _b * DPLENB, &v0_bank, DPLENB);
                }
                for (uint32_t i = 0; i < rVl; ++i) {
                    if (elem_mask(tmp_v0_storage, i)) {
                        hasActiveElem = true;
                        break;
                    }
                }
            }
        }

        const uint32_t lane_count = DPLEN / (8 * sizeof(decltype(ElemType::v)));
        uint32_t active = lane_count;
        while (active > 1) {
            for (uint32_t i = 0; i < active / 2; ++i) {
                partial_lanes[i] = reduceStep(
                    machInst, partial_lanes[i], partial_lanes[i + active / 2]);
            }
            active >>= 1;
        }
        const auto partial = partial_lanes[0];

        vreg_t old_vd;
        xc->getRegOperand(this, oldDstIdx, &old_vd);
        auto *dst = static_cast<VecRegContainer *>(
            xc->getWritableRegOperand(this, 0));
        memcpy(dst->as<uint8_t>(), old_vd.as<uint8_t>(), DPLENB);
        if (rVl > 0) {
            dst->as<decltype(ElemType::v)>()[0] = hasActiveElem ?
                reduceStep(machInst, seed, partial) : seed;
        }
        xc->setRegOperand(this, 0, dst);
        if (traceData) {
            traceData->setData(*dst);
        }
        return NoFault;
    }
};

template<typename AccType>
class VReduceFPTreeMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[2];
    RegId destRegIdxArr[1];
  public:
    VReduceFPTreeMicroInst(ExtMachInst machInst, uint8_t dstSlot,
                           uint8_t srcSlot, OpClass opClass,
                           uint8_t _microIdx = 0)
        : VectorArithMicroInst("vfred_tree_vbuf", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, dstSlot));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, srcSlot));
        setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, dstSlot));
        _numTypedDestRegs[VecBufRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;
        VecBufRegContainer lhs, rhs;
        xc->getRegOperand(this, 0, &lhs);
        xc->getRegOperand(this, 1, &rhs);
        auto lv = lhs.as<decltype(AccType::v)>()[0];
        auto rv = rhs.as<decltype(AccType::v)>()[0];
        lhs.as<decltype(AccType::v)>()[0] =
            fadd<AccType>(ftype<AccType>(lv), ftype<AccType>(rv)).v;
        xc->setRegOperand(this, 0, &lhs);
        if (traceData) {
            traceData->setData(lhs);
        }
        return NoFault;
    }
};

template<typename AccType>
class VReduceFPFinalMicroInst : public VectorArithMicroInst
{
  private:
    RegId srcRegIdxArr[4]; // vs1 + partial(vbuf) + vl + old vd
    RegId destRegIdxArr[1];
  public:
    VReduceFPFinalMicroInst(ExtMachInst machInst, uint8_t partialSlot,
                            OpClass opClass, uint8_t _microIdx = 0)
        : VectorArithMicroInst("vfred_final_vd", machInst, opClass, _microIdx)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));
        _numSrcRegs = 0;
        _numDestRegs = 0;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vs1 * VregBanks));
        setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, partialSlot));
        vlsrcIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);
        oldDstIdx = _numSrcRegs;
        setSrcRegIdx(_numSrcRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        setDestRegIdx(_numDestRegs++, RegId(VecRegClass, machInst.vd * VregBanks));
        _numTypedDestRegs[VecRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        if (machInst.vill) {
            return std::make_shared<IllegalInstFault>("VILL is set", machInst);
        }
        uint_fast8_t frm = xc->readMiscReg(MISCREG_FRM);
        if (frm > 4) {
            return std::make_shared<IllegalInstFault>("RM fault", machInst);
        }
        softfloat_roundingMode = frm;

        vreg_t vs1_reg;
        xc->getRegOperand(this, 0, &vs1_reg);
        const auto seed = vs1_reg.as<decltype(AccType::v)>()[0];

        VecBufRegContainer partial_buf;
        xc->getRegOperand(this, 1, &partial_buf);
        const auto partial = partial_buf.as<decltype(AccType::v)>()[0];
        const uint32_t rVl = xc->getRegOperand(this, vlsrcIdx);

        vreg_t old_vd;
        xc->getRegOperand(this, oldDstIdx, &old_vd);
        auto *dst = static_cast<VecRegContainer *>(
            xc->getWritableRegOperand(this, 0));
        memcpy(dst->as<uint8_t>(), old_vd.as<uint8_t>(), DPLENB);
        if (rVl > 0) {
            dst->as<decltype(AccType::v)>()[0] =
                fadd<AccType>(ftype<AccType>(seed), ftype<AccType>(partial)).v;
        }
        xc->setRegOperand(this, 0, dst);
        if (traceData) {
            traceData->setData(*dst);
        }
        return NoFault;
    }
};

template<typename Type>
class VCompressComputeMicroInst : public RiscvMicroInst
{
  private:
    static constexpr uint8_t MaxSlots = 16;
    RegId srcRegIdxArr[MaxSlots + VregBanks + 1];
    RegId destRegIdxArr[MaxSlots + 1];
    uint8_t vsew;
    int8_t vlmul;
    uint32_t sew;
    float vflmul;
    uint8_t numSlots;

  public:
    VCompressComputeMicroInst(ExtMachInst machInst)
        : RiscvMicroInst("vcompress_compute", machInst, VectorPermuteOp),
          vsew(machInst.vtype8.vsew),
          vlmul(vtype_vlmul(machInst.vtype8)),
          sew(8 << vsew),
          vflmul(vlmul < 0 ? (1.0 / (1 << (-vlmul))) : (1 << vlmul)),
          numSlots((vlmul < 0 ? 1 : (1 << vlmul)) * VregBanks)
    {
        flags[IsMicroop] = true;
        flags[IsVector] = true;
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::srcRegIdxArr),
            reinterpret_cast<RegIdArrayPtr>(
                &std::remove_pointer_t<decltype(this)>::destRegIdxArr));

        _numSrcRegs = 0;
        _numDestRegs = 0;

        panic_if(numSlots > MaxSlots,
                 "VCompressComputeMicroInst numSlots=%u exceeds MaxSlots=%u",
                 numSlots, MaxSlots);
        for (uint8_t i = 0; i < numSlots; i++) {
            setSrcRegIdx(_numSrcRegs++, RegId(VecBufRegClass, i));
        }
        for (uint32_t _b = 0; _b < VregBanks; ++_b) {
            setSrcRegIdx(_numSrcRegs++,
                RegId(VecRegClass, machInst.vs1 * VregBanks + _b));
        }
        setSrcRegIdx(_numSrcRegs++, VecRenamedVLReg);

        for (uint8_t i = 0; i < numSlots; i++) {
            setDestRegIdx(_numDestRegs++, RegId(VecBufRegClass, i));
            _numTypedDestRegs[VecBufRegClass]++;
        }
        setDestRegIdx(_numDestRegs++, VecCompressCntReg);
        _numTypedDestRegs[VecRegClass]++;
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData)
            const override
    {
        alignas(16) uint8_t vs2_bytes[VLENB * 8] = {0};
        alignas(16) uint8_t vd_bytes[VLENB * 8] = {0};
        for (uint8_t i = 0; i < numSlots; i++) {
            VecBufRegContainer vbuf_vs2;
            xc->getRegOperand(this, i, &vbuf_vs2);
            memcpy(vs2_bytes + i * DPLENB, vbuf_vs2.as<uint8_t>(), DPLENB);
        }

        uint8_t vm_buf[VLENB];
        for (uint32_t _b = 0; _b < VregBanks; ++_b) {
            vreg_t vm_bank;
            xc->getRegOperand(this, numSlots + _b, &vm_bank);
            memcpy(vm_buf + _b * DPLENB, &vm_bank, DPLENB);
        }
        uint32_t rVl = xc->getRegOperand(this, numSlots + VregBanks);

        auto *vs2_data = reinterpret_cast<Type *>(vs2_bytes);
        auto *vd_data = reinterpret_cast<Type *>(vd_bytes);
        auto *vm_data = vm_buf;

        uint32_t vd_ptr = 0;
        for (uint32_t i = 0; i < rVl; i++) {
            if (elem_mask(vm_data, i)) {
                vd_data[vd_ptr] = vs2_data[i];
                vd_ptr++;
            }
        }

        for (uint8_t i = 0; i < numSlots; i++) {
            VecBufRegContainer vbuf_vd;
            memcpy(vbuf_vd.as<uint8_t>(), vd_bytes + i * DPLENB, DPLENB);
            xc->setRegOperand(this, i, &vbuf_vd);
        }
        vreg_t compress_cnt{};
        compress_cnt.as<uint32_t>()[0] = vd_ptr;
        xc->setRegOperand(this, numSlots, &compress_cnt);
        if (traceData) {
            VecBufRegContainer trace_vd;
            memcpy(trace_vd.as<uint8_t>(), vd_bytes, DPLENB);
            traceData->setData(trace_vd);
        }
        return NoFault;
    }

    std::string generateDisassembly(Addr pc,
            const loader::SymbolTable *symtab) const override
    {
        std::stringstream ss;
        ss << mnemonic << ' '
           << registerName(destRegIdx(0)) << "-..."
           << ", " << registerName(srcRegIdx(0)) << "-..."
           << ", slots=" << static_cast<unsigned>(numSlots);
        return ss.str();
    }
};

template<typename Type>
class Vcompress_vm : public RiscvMacroInst
{
  public:
    Vcompress_vm(ExtMachInst extMachInst)
        : RiscvMacroInst("Vcompress", extMachInst, VectorPermuteOp)
    {
        flags[IsVector] = true;

        const int8_t vlmul_val = vtype_vlmul(extMachInst.vtype8);
        const float vflmul_val = vlmul_val < 0
            ? (1.0 / (1 << (-vlmul_val))) : (1 << vlmul_val);
        const uint32_t regLength = vflmul_val < 1 ? 1 : vflmul_val;

        const RegIndex vbuf_vs2 = 0;
        const RegIndex vbuf_vd = 0;

        StaticInstPtr microop;

        int vs2_insert_idx = 0;
        for (uint32_t i = 0; i < regLength; i++) {
            for (uint32_t b = 0; b < VregBanks; b++) {
                RegIndex src = (extMachInst.vs2 + i) * VregBanks + b;
                uint32_t offset = (i * VregBanks + b) * DPLENB;
                int expected_writes = 1;
                microop = new VBufInsertMicroInst(extMachInst, vbuf_vs2,
                                                  src, offset,
                                                  expected_writes);
                microop->setDelayedCommit();
                this->microops.push_back(microop);
                vs2_insert_idx++;
            }
        }

        microop = new VCompressComputeMicroInst<Type>(extMachInst);
        microop->setDelayedCommit();
        this->microops.push_back(microop);

        for (uint32_t i = 0; i < regLength; i++) {
            for (uint32_t b = 0; b < VregBanks; b++) {
                RegIndex dst = (extMachInst.vd + i) * VregBanks + b;
                uint32_t offset = (i * VregBanks + b) * DPLENB;
                microop = new VBufExtractMicroInst(extMachInst, vbuf_vd,
                                                   dst, offset, true);
                microop->setDelayedCommit();
                this->microops.push_back(microop);
            }
        }

        this->microops.front()->setFirstMicroop();
        this->microops.back()->setLastMicroop();
    }

    std::string generateDisassembly(Addr pc, const loader::SymbolTable *symtab)
        const override
    {
        std::stringstream ss;
        ss << mnemonic << " v" << (uint32_t)machInst.vd
           << ", v" << (uint32_t)machInst.vs2
           << ", v" << (uint32_t)machInst.vs1;
        if (!machInst.vm) {
            ss << ", v0.t";
        }
        return ss.str();
    }
};

class VectorGatherMicroInst : public VectorMicroInst
{
private:
    uint32_t vs2_vregs;  // vs2寄存器数量
    uint8_t vs1_idx;     // vs1寄存器索引
    uint8_t vd_idx;      // vd寄存器索引
protected:
    VectorGatherMicroInst(const char *mnem, ExtMachInst _machInst,
                         OpClass __opClass, uint8_t _microIdx)
        : VectorMicroInst(mnem, _machInst, __opClass, _microIdx)
    {}

public:
    void setGatherParams(uint32_t _vs2_vregs, uint8_t _vs1_idx, uint8_t _vd_idx) {
        vs2_vregs = _vs2_vregs;
        vs1_idx = _vs1_idx;
        vd_idx = _vd_idx;
    }

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

class VectorGatherMacroInst : public VectorMacroInst
{
protected:
    VectorGatherMacroInst(const char* mnem, ExtMachInst _machInst,
                         OpClass __opClass)
        : VectorMacroInst(mnem, _machInst, __opClass)
    {}

    std::string generateDisassembly(
            Addr pc, const loader::SymbolTable *symtab) const override;
};

} // namespace RiscvISA
} // namespace gem5


#endif // __ARCH_RISCV_INSTS_VECTOR_HH__

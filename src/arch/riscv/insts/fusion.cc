#include "fusion.hh"

namespace gem5::RiscvISAInst
{
using namespace RiscvISA;
}

#include <fenv.h>

#include "arch/riscv/generated/decoder.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace RiscvISA
{
// A x1, x2, x3 + B x1, x1, x4
// => A fuseTmp, x2, x3 + B x1, fuseTmp, x4
// => C x1, (x2, x3), (fuseTmp, x4)

Addr FusionInst::getSecondPC() const { return second->getPC(); }

class ChainFusionInst : public FusionInst
{
    // only have one dst
    int firstNumSrcs = 0;
  public:
    ChainFusionInst(const char *name, OpClass op, o3::DynInstPtr first, o3::DynInstPtr second)
        : FusionInst(name, op, first, second)
    {
        panic_if(first->destRegIdx(0) != second->destRegIdx(0),
                 "ChainFusionInst: first and second insts must have the same destination register");
        setDestRegIdx(_numDestRegs++, first->destRegIdx(0)); _numTypedDestRegs[first->destRegIdx(0).classValue()]++;
        for (int i = 0; i < first->numSrcRegs(); ++i) {
            setSrcRegIdx(_numSrcRegs++, first->srcRegIdx(i));
        }
        firstNumSrcs = first->numSrcRegs();

        for (int i = 0; i < second->numSrcRegs(); ++i) {
            if (second->srcRegIdx(i) == first->destRegIdx(0)) {
                // if the second inst's src is the first inst's dst, we use a temporary reg
                setSrcRegIdx(_numSrcRegs++, FuseTmpReg);
            } else {
                // otherwise, just use the second inst's src reg
                setSrcRegIdx(_numSrcRegs++, second->srcRegIdx(i));
            }
        }

        flags = first->staticInst->getFlags() | second->staticInst->getFlags();
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        assert(fused);

        PhysRegIdPtr fuseTmp = nullptr;
        for (int i = 0; i < fused->numSrcRegs(); ++i) {
            if (fused->srcRegIdx(i) == FuseTmpReg) {
                fuseTmp = fused->renamedSrcIdx(i);
                break;
            }
        }
        if (fuseTmp) {
            first->renameDestReg(0, o3::VirtRegId(fuseTmp), o3::VirtRegId());
        } else {
            first->renameDestReg(0, fused->extRenamedDestIdx(0), o3::VirtRegId());
        }

        second->renameDestReg(0, fused->extRenamedDestIdx(0), o3::VirtRegId());

        // rename
        for (int i = 0; i < numSrcRegs(); i++) {
            if (i < firstNumSrcs) {
                // first inst's src
                first->renameSrcReg(i, fused->extRenamedSrcIdx(i));
            } else {
                // second inst's src
                second->renameSrcReg(i - firstNumSrcs, fused->extRenamedSrcIdx(i));
            }
        }

        // execute
        Fault fault = first->execute();
        if (fault != NoFault)
            return fault;
        fault = second->execute();
        return fault;
    }
};


// add x1, x2, x3 + ld x1, offset(x1)
template<int memsize>
class AluLoadFusionInst : public FusionInst
{
    int firstNumSrcs = 0;

    Request::Flags memAccessFlags;
  public:
    AluLoadFusionInst(const char *name, OpClass op, const o3::DynInstPtr& first, const o3::DynInstPtr& second)
        : FusionInst(name, op, first, second)
    {
        panic_if(first->destRegIdx(0) != second->srcRegIdx(1),
                 "AluLoadFusionInst: first's dst must be the same as second's src");
        panic_if(second->destRegIdx(0) != second->srcRegIdx(0),
                 "AluLoadFusionInst: second's dst must be the same as its src");
        assert(second->operWid() / 8 == memsize);
        memAccessFlags = dynamic_cast<MemInst*>(second->staticInst.get())->getMemAccessFlags();
        setDestRegIdx(_numDestRegs++, second->destRegIdx(0)); _numTypedDestRegs[second->destRegIdx(0).classValue()]++;
        for (int i = 0; i < first->numSrcRegs(); ++i) {
            setSrcRegIdx(_numSrcRegs++, first->srcRegIdx(i));
        }
        firstNumSrcs = first->numSrcRegs();
        setSrcRegIdx(_numSrcRegs++, FuseTmpReg);

        flags = second->staticInst->getFlags();
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        panic("Not implemented yet");
    }

    Fault initiateAcc(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        assert(fused);

        first->renameDestReg(0, fused->extRenamedSrcIdx(firstNumSrcs), o3::VirtRegId());
        second->renameDestReg(0, fused->extRenamedDestIdx(0), o3::VirtRegId());

        // rename srcs
        for (int i = 0; i < numSrcRegs(); i++) {
            if (i < firstNumSrcs) {
                // first inst's src
                first->renameSrcReg(i, fused->extRenamedSrcIdx(i));
            } else {
                // second inst's src
                second->renameSrcReg(i - firstNumSrcs, fused->extRenamedSrcIdx(i));
            }
        }

        // calculate the address
        Fault fault = first->execute();
        if (fault != NoFault)
            return fault;
        uint64_t Rp1 = second->getRegOperand(this, 0);
        Addr EA = Rp1 + second->staticInst->getImm();
        return initiateMemReadSize<ExecContext, memsize>(xc, traceData, EA, memAccessFlags);
    }

    Fault completeAcc(PacketPtr pkt, ExecContext *, Trace::InstRecord *) const override
    {
        Fault fault = second->completeAcc(pkt);
        return fault;
    }
};

// ld x1, offset(x2) + ld x3, offset + 8(x2)
template <int memsize>
class SeqLoadFusionInst : public FusionInst
{

    int base_offset = 0;
    int size0 = 0, size1 = 0;

    Request::Flags memAccessFlags;
  public:
    SeqLoadFusionInst(const char *name, OpClass op, const o3::DynInstPtr& first, const o3::DynInstPtr& second)
        : FusionInst(name, op, first, second)
    {
        panic_if(first->srcRegIdx(0) != second->srcRegIdx(0),
                 "SeqLoadFusionInst: first and second insts must have the same source register");
        panic_if(first->staticInst->getImm() + first->operWid() /8 != second->staticInst->getImm(),
                 "SeqLoadFusionInst: second inst's offset must be first inst's offset + load size");
        panic_if(dynamic_cast<MemInst*>(first->staticInst.get())->getMemAccessFlags() !=
                 dynamic_cast<MemInst*>(second->staticInst.get())->getMemAccessFlags(),
                    "SeqLoadFusionInst: first and second insts must have the same memory access flags");
        base_offset = first->staticInst->getImm();
        size0 = first->operWid() / 8;
        size1 = second->operWid() / 8;
        assert(memsize == size0 + size1);
        memAccessFlags = dynamic_cast<MemInst*>(first->staticInst.get())->getMemAccessFlags();

        setDestRegIdx(_numDestRegs++, first->destRegIdx(0)); _numTypedDestRegs[first->destRegIdx(0).classValue()]++;
        setDestRegIdx(_numDestRegs++, second->destRegIdx(0)); _numTypedDestRegs[second->destRegIdx(0).classValue()]++;
        setSrcRegIdx(_numSrcRegs++, first->srcRegIdx(0));

        flags = first->staticInst->getFlags() | second->staticInst->getFlags();
    }

    Fault execute(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        panic("Not implemented yet");
    }

    Fault initiateAcc(ExecContext *xc, Trace::InstRecord *traceData) const override
    {
        // rename
        first->renameDestReg(0, fused->extRenamedDestIdx(0), o3::VirtRegId());
        second->renameDestReg(0, fused->extRenamedDestIdx(1), o3::VirtRegId());
        // no need to rename src

        uint64_t Rp1 = xc->getRegOperand(this, 0);
        Addr EA = Rp1 + base_offset;
        return initiateMemReadSize<ExecContext, memsize>(xc, traceData, EA, memAccessFlags);
    }

    Fault completeAcc(PacketPtr pkt, ExecContext *, Trace::InstRecord *) const override
    {
        Packet tmp(pkt->getPtr<uint8_t>(), size0);
        Fault fault = first->completeAcc(&tmp);
        if (fault != NoFault)
            return fault;
        tmp.setPtr(pkt->getPtr<uint8_t>() + size0, size1);
        fault = second->completeAcc(&tmp);
        return fault;
    }

    bool correctMisalign(Addr addr) const override
    {
        bool align0 = addr % size0 == 0;
        bool align1 = (addr + size0) % size1 == 0;

        return align0 && align1;
    }
};

StaticInstPtr
chainFuseInsts(const char *name, const std::vector<o3::DynInstPtr> &vec,
               std::function<bool(const std::vector<o3::DynInstPtr> &)> checker)
{
    if (!checker(vec)) {
        return nullptr;  // cannot fuse
    }
    return new ChainFusionInst(name, vec[1]->opClass(), vec[0], vec[1]);
}

template<int memsize>
StaticInstPtr
aluLoadFuseInsts(const char *name, const std::vector<o3::DynInstPtr> &vec,
               std::function<bool(const std::vector<o3::DynInstPtr> &)> checker)
{
    if (!checker(vec)) {
        return nullptr;  // cannot fuse
    }
    return new AluLoadFusionInst<memsize>(name, vec[1]->opClass(), vec[0], vec[1]);
}

template<int memsize>
StaticInstPtr
seqLoadFuseInsts(const char *name, const std::vector<o3::DynInstPtr> &vec,
               std::function<bool(const std::vector<o3::DynInstPtr> &)> checker)
{
    if (!checker(vec)) {
        return nullptr;  // cannot fuse
    }
    return new SeqLoadFusionInst<memsize>(name, vec[1]->opClass(), vec[0], vec[1]);
}

const std::unordered_map<std::type_index, std::type_index> deCompressMap = {
    {typeid(RiscvISAInst::C_slli), typeid(RiscvISAInst::Slli)},
    {typeid(RiscvISAInst::C_srli), typeid(RiscvISAInst::Srli)},
    {typeid(RiscvISAInst::C_addi), typeid(RiscvISAInst::Addi)},
    {typeid(RiscvISAInst::C_addiw), typeid(RiscvISAInst::Addiw)},
    {typeid(RiscvISAInst::C_add), typeid(RiscvISAInst::Add)},
    {typeid(RiscvISAInst::C_addw), typeid(RiscvISAInst::Addw)},
    {typeid(RiscvISAInst::C_and), typeid(RiscvISAInst::And)},
    {typeid(RiscvISAInst::C_andi), typeid(RiscvISAInst::Andi)},
    {typeid(RiscvISAInst::C_or), typeid(RiscvISAInst::Or)},
    {typeid(RiscvISAInst::C_xor), typeid(RiscvISAInst::Xor)},
    {typeid(RiscvISAInst::C_zext_h), typeid(RiscvISAInst::Zext_h)},
    {typeid(RiscvISAInst::C_sext_h), typeid(RiscvISAInst::Sext_h)},
    {typeid(RiscvISAInst::C_lui), typeid(RiscvISAInst::Lui)},
    {typeid(RiscvISAInst::C_ld), typeid(RiscvISAInst::Ld)},
    {typeid(RiscvISAInst::C_ldsp), typeid(RiscvISAInst::Ld)},
    {typeid(RiscvISAInst::C_lw), typeid(RiscvISAInst::Lw)},
    {typeid(RiscvISAInst::C_lwsp), typeid(RiscvISAInst::Lw)},
    {typeid(RiscvISAInst::C_lh), typeid(RiscvISAInst::Lh)},
    // unsigned load -> signed load
    {typeid(RiscvISAInst::Lwu), typeid(RiscvISAInst::Lw)},
    {typeid(RiscvISAInst::Lhu), typeid(RiscvISAInst::Lh)},
    {typeid(RiscvISAInst::Lbu), typeid(RiscvISAInst::Lb)},
    {typeid(RiscvISAInst::C_lhu), typeid(RiscvISAInst::Lh)},
    {typeid(RiscvISAInst::C_lbu), typeid(RiscvISAInst::Lb)},
};

#define ImmKey(t, i) FusionKey(typeid(RiscvISAInst::t), i)
#define AnyImmKey(t) FusionKey(typeid(RiscvISAInst::t))
#define ChainCreator(n, ...) [](const std::vector<o3::DynInstPtr>& vec) { return chainFuseInsts(n, std::move(vec), [](const std::vector<o3::DynInstPtr>& vec) { return __VA_ARGS__ ;}); }
#define AluLdCreator(n, s, ...) [](const std::vector<o3::DynInstPtr>& vec) { return aluLoadFuseInsts<s>(n, std::move(vec), [](const std::vector<o3::DynInstPtr>& vec) { return __VA_ARGS__ ;}); }
#define SeqLdCreator(n, s, ...) [](const std::vector<o3::DynInstPtr>& vec) { return seqLoadFuseInsts<s>(n, std::move(vec), [](const std::vector<o3::DynInstPtr>& vec) { return __VA_ARGS__ ;}); }

#define FirstDest0EqualSecond(a, b) (a->destRegIdx(0) == b->destRegIdx(0))
#define Dest0EqualSrc0(x) (x->destRegIdx(0) == x->srcRegIdx(0))
#define Dest0EqualSrc0or1(x) ((x->destRegIdx(0) == x->srcRegIdx(0)) || (x->destRegIdx(0) == x->srcRegIdx(1)))
#define ImmIs(a, i) (a->staticInst->getImm() == (i))
#define SeqLoadCheck() ((vec[0]->srcRegIdx(0) == vec[1]->srcRegIdx(0)) && (vec[0]->destRegIdx(0) != vec[0]->srcRegIdx(0)))

// make sure do not have the same key on different fusion tags
const FusionTag fusionMap = {
    // shift + alu
    {ImmKey(Slli, 32), new FusionTag{
        {ImmKey(Srli, 32), ChainCreator("low32", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {ImmKey(Srli, 31), ChainCreator("sll1zext", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {ImmKey(Srli, 30), ChainCreator("sll2zext", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {ImmKey(Srli, 29), ChainCreator("sll3zext", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {ImmKey(Slli, 48), new FusionTag{
        {ImmKey(Srli, 48), ChainCreator("low16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {ImmKey(Slliw, 16), new FusionTag{
        {ImmKey(Srliw, 16), ChainCreator("low16w", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {ImmKey(Sraiw, 16), ChainCreator("sext16w", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {ImmKey(Slli, 1), new FusionTag{
        {AnyImmKey(Add), ChainCreator("sll1add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Slli, 2), new FusionTag{
        {AnyImmKey(Add), ChainCreator("sll2add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Slli, 3), new FusionTag{
        {AnyImmKey(Add), ChainCreator("sll3add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Slli, 4), new FusionTag{
        {AnyImmKey(Add), ChainCreator("sll4add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Srli, 29), new FusionTag{
        {AnyImmKey(Add), ChainCreator("srl29add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Srli, 30), new FusionTag{
        {AnyImmKey(Add), ChainCreator("srl30add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Srli, 31), new FusionTag{
        {AnyImmKey(Add), ChainCreator("srl31add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Srli, 32), new FusionTag{
        {AnyImmKey(Add), ChainCreator("srl32add", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
    }},
    {ImmKey(Srli, 8), new FusionTag{
        {ImmKey(Andi, 0xff), ChainCreator("byte2", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    // add
    {AnyImmKey(Add), new FusionTag{
        {AnyImmKey(Ld), AluLdCreator("farld", 8, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lw), AluLdCreator("farlw", 4, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lh), AluLdCreator("farlh", 2, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lb), AluLdCreator("farlb", 1, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Addw), new FusionTag{
        {ImmKey(Andi, 255), ChainCreator("addwbyte", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {ImmKey(Andi, 1), ChainCreator("addwbit", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("addwzexth", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Sext_h), ChainCreator("addwsexth", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Addi), new FusionTag{
        {AnyImmKey(Ld), AluLdCreator("farld", 8, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lw), AluLdCreator("farlw", 4, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lh), AluLdCreator("farlh", 2, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lb), AluLdCreator("farlb", 1, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Lui), new FusionTag{
        {AnyImmKey(Addi), ChainCreator("lui32", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Addiw), ChainCreator("luiw32", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Auipc), new FusionTag{
        {AnyImmKey(Ld), AluLdCreator("farld", 8, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lw), AluLdCreator("farlw", 4, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lh), AluLdCreator("farlh", 2, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Lb), AluLdCreator("farlb", 1, FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    // logic
    {AnyImmKey(Andi), new FusionTag{
        {AnyImmKey(Add), ChainCreator("oddadd", ImmIs(vec[0], 1) && FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
        {AnyImmKey(Addw), ChainCreator("oddaddw", ImmIs(vec[0], 1) && FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
        {AnyImmKey(Or), ChainCreator("orh48", ImmIs(vec[0], -256) && FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},
        {AnyImmKey(Mulw), ChainCreator("mulw7", ImmIs(vec[0], 127) && FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0or1(vec[1]) )},

        {ImmKey(Andi, 1), ChainCreator("andi1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("andi16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(And), new FusionTag{
        {ImmKey(Andi, 1), ChainCreator("and1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("and16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Ori), new FusionTag{
        {ImmKey(Andi, 1), ChainCreator("ori1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("ori16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Or), new FusionTag{
        {ImmKey(Andi, 1), ChainCreator("or1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("or16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Xori), new FusionTag{
        {ImmKey(Andi, 1), ChainCreator("xori1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("xori16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Xor), new FusionTag{
        {ImmKey(Andi, 1), ChainCreator("xor1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("xor16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    {AnyImmKey(Orc_b), new FusionTag{
        {ImmKey(Andi, 1), ChainCreator("orcb1", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
        {AnyImmKey(Zext_h), ChainCreator("orcb16", FirstDest0EqualSecond(vec[0], vec[1]) && Dest0EqualSrc0(vec[1]) )},
    }},
    // load
    {AnyImmKey(Ld), new FusionTag{
        {AnyImmKey(Ld), SeqLdCreator("ldld", 16, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 8) )},
        {AnyImmKey(Lw), SeqLdCreator("ldlw", 12, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 8) )},
        {AnyImmKey(Lh), SeqLdCreator("ldlh", 10, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 8) )},
        {AnyImmKey(Lb), SeqLdCreator("ldlb", 9, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 8) )},
    }},
    {AnyImmKey(Lw), new FusionTag{
        {AnyImmKey(Ld), SeqLdCreator("lwld", 12, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 4) )},
        {AnyImmKey(Lw), SeqLdCreator("lwlw", 8, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 4) )},
        {AnyImmKey(Lh), SeqLdCreator("lwlh", 6, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 4) )},
        {AnyImmKey(Lb), SeqLdCreator("lwlb", 5, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 4) )},
    }},
    {AnyImmKey(Lh), new FusionTag{
        {AnyImmKey(Ld), SeqLdCreator("lhld", 10, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 2) )},
        {AnyImmKey(Lw), SeqLdCreator("lhlw", 6, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 2) )},
        {AnyImmKey(Lh), SeqLdCreator("lhlh", 4, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 2) )},
        {AnyImmKey(Lb), SeqLdCreator("lhlb", 3, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 2) )},
    }},
    {AnyImmKey(Lb), new FusionTag{
        {AnyImmKey(Ld), SeqLdCreator("lbld", 9, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 1) )},
        {AnyImmKey(Lw), SeqLdCreator("lblw", 5, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 1) )},
        {AnyImmKey(Lh), SeqLdCreator("lblh", 3, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 1) )},
        {AnyImmKey(Lb), SeqLdCreator("lblb", 2, SeqLoadCheck() && ImmIs(vec[1], vec[0]->staticInst->getImm() + 1) )},
    }},
};


}
}

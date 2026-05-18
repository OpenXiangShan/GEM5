/*
 * Minimal runtime check for matrix sync stub semantics.
 */

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "arch/generic/htm.hh"
#include "arch/riscv/decoder.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/isa.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/regs/matrix.hh"
#include "arch/riscv/regs/misc.hh"
#include "cpu/exec_context.hh"
#include "cpu/op_class.hh"
#include "cpu/thread_context.hh"
#include "params/RiscvDecoder.hh"
#include "params/RiscvISA.hh"
#include "sim/faults.hh"
#include "sim/serialize.hh"

namespace gem5
{

namespace
{

constexpr uint32_t MatrixOpcode = 0x2b;
constexpr uint32_t SystemOpcode = 0x73;
constexpr uint32_t Funct3Zero = 0x0;
constexpr uint32_t Funct3Csrrw = 0x1;
constexpr uint32_t Funct3Csrrs = 0x2;
constexpr uint32_t Funct7Msettilek = 0x09;
constexpr uint32_t Funct7Msettilem = 0x11;
constexpr uint32_t Funct7Msettilen = 0x19;
constexpr uint32_t Funct7Msyncregreset = 0x40;
constexpr uint32_t Funct7Mrelease = 0x48;
constexpr uint32_t Funct7Macquire = 0x50;
constexpr uint32_t Funct7Mfmacc = 0x04;

uint32_t
encodeMatrixSync(uint32_t funct7, uint32_t token_idx, uint32_t rs1 = 0,
                 uint32_t rd = 0)
{
    return (funct7 << 25) |
           ((token_idx & 0x1f) << 20) |
           ((rs1 & 0x1f) << 15) |
           (Funct3Zero << 12) |
           ((rd & 0x1f) << 7) |
           MatrixOpcode;
}

uint32_t
encodeMsettile(uint32_t funct7, uint32_t rs1)
{
    return (funct7 << 25) |
           (0u << 20) |
           ((rs1 & 0x1f) << 15) |
           (Funct3Zero << 12) |
           (0u << 7) |
           MatrixOpcode;
}

uint32_t
encodeMatrixCarrier(uint32_t funct7, uint32_t rs2, uint32_t rs1,
                    uint32_t funct3, uint32_t rd)
{
    return (funct7 << 25) |
           ((rs2 & 0x1f) << 20) |
           ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) |
           ((rd & 0x1f) << 7) |
           MatrixOpcode;
}

uint32_t
encodeCsrReg(uint32_t csr, uint32_t funct3, uint32_t rd, uint32_t rs1)
{
    return ((csr & 0xfff) << 20) |
           ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) |
           ((rd & 0x1f) << 7) |
           SystemOpcode;
}

uint32_t
encodeCsrrwi(uint32_t csr, uint32_t rd, uint32_t uimm)
{
    return ((csr & 0xfff) << 20) |
           ((uimm & 0x1f) << 15) |
           (0x5u << 12) |
           ((rd & 0x1f) << 7) |
           SystemOpcode;
}

uint32_t
encodeCsrrsi(uint32_t csr, uint32_t rd, uint32_t uimm)
{
    return ((csr & 0xfff) << 20) |
           ((uimm & 0x1f) << 15) |
           (0x6u << 12) |
           ((rd & 0x1f) << 7) |
           SystemOpcode;
}

bool
check(bool cond, const std::string &msg)
{
    if (!cond) {
        std::cerr << "matrix_sync_check: " << msg << std::endl;
        return false;
    }
    return true;
}

class TestThreadContext final : public ThreadContext
{
  public:
    explicit TestThreadContext(BaseISA *isa_ptr)
        : _isa(isa_ptr), _pcState(0)
    {}

    void setDecoder(InstDecoder *decoder) { _decoder = decoder; }

    bool schedule(PCEvent *event) override { return false; }
    bool remove(PCEvent *event) override { return false; }
    BaseCPU *getCpuPtr() override { return nullptr; }
    int cpuId() const override { return 0; }
    uint32_t socketId() const override { return 0; }
    int threadId() const override { return 0; }
    void setThreadId(int id) override {}
    ContextID contextId() const override { return 0; }
    void setContextId(ContextID id) override {}
    BaseMMU *getMMUPtr() override { return nullptr; }
    CheckerCPU *getCheckerCpuPtr() override { return nullptr; }
    BaseISA *getIsaPtr() const override { return _isa; }
    InstDecoder *getDecoderPtr() override { return _decoder; }
    System *getSystemPtr() override { return nullptr; }
    Process *getProcessPtr() override { return nullptr; }
    void setProcessPtr(Process *p) override {}
    Status status() const override { return _status; }
    void setStatus(Status new_status) override { _status = new_status; }
    void activate() override { _status = Active; }
    void suspend() override { _status = Suspended; }
    void halt() override { _status = Halted; }
    void takeOverFrom(ThreadContext *old_context) override {}
    void scheduleInstCountEvent(Event *event, Tick count) override {}
    void descheduleInstCountEvent(Event *event) override {}
    Tick getCurrentInstCount() override { return 0; }
    Tick readLastActivate() override { return 0; }
    Tick readLastSuspend() override { return 0; }
    void copyArchRegs(ThreadContext *tc) override {}
    void clearArchRegs() override {}

    const PCStateBase &pcState() const override { return _pcState; }
    void pcState(const PCStateBase &val) override { _pcState.update(val); }
    void pcStateNoRecord(const PCStateBase &val) override { _pcState.update(val); }

    RegVal
    readMiscRegNoEffect(RegIndex misc_reg) const override
    {
        return static_cast<RiscvISA::ISA *>(_isa)->readMiscRegNoEffect(misc_reg);
    }

    RegVal
    readMiscReg(RegIndex misc_reg) override
    {
        return static_cast<RiscvISA::ISA *>(_isa)->readMiscReg(misc_reg);
    }

    void
    setMiscRegNoEffect(RegIndex misc_reg, RegVal val) override
    {
        static_cast<RiscvISA::ISA *>(_isa)->setMiscRegNoEffect(misc_reg, val);
    }

    void
    setMiscReg(RegIndex misc_reg, RegVal val) override
    {
        static_cast<RiscvISA::ISA *>(_isa)->setMiscReg(misc_reg, val);
    }

    RegId flattenRegId(const RegId &reg_id) const override { return reg_id; }
    unsigned readStCondFailures() const override { return 0; }
    void setStCondFailures(unsigned sc_failures) override {}

    void getRegFlat(const RegId &reg, void *val) const override
    {
        std::memcpy(val, &_regVal, sizeof(_regVal));
    }

    void *getWritableRegFlat(const RegId &reg) override { return &_regVal; }

    void setRegFlat(const RegId &reg, const void *val) override
    {
        std::memcpy(&_regVal, val, sizeof(_regVal));
    }

    void htmAbortTransaction(uint64_t htm_uid, HtmFailureFaultCause cause) override {}
    BaseHTMCheckpointPtr &getHtmCheckpointPtr() override { return _htmCheckpoint; }
    void setHtmCheckpointPtr(BaseHTMCheckpointPtr cpt) override
    {
        _htmCheckpoint = std::move(cpt);
    }

  private:
    BaseISA *_isa = nullptr;
    InstDecoder *_decoder = nullptr;
    mutable RegVal _regVal = 0;
    Status _status = Active;
    RiscvISA::PCState _pcState;
    BaseHTMCheckpointPtr _htmCheckpoint;
};

class TestExecContext final : public ExecContext
{
  public:
    explicit TestExecContext(ThreadContext *thread_context)
        : _tc(thread_context)
    {}

    void setSrc0(RegVal val) { _src0 = val; }
    void setSrc1(RegVal val) { _src1 = val; }
    RegVal writtenReg() const { return _writtenReg; }

    RegVal getRegOperand(const StaticInst *si, int idx) override
    {
        const auto &reg = si->srcRegIdx(idx);
        if (reg.is(RMiscRegClass)) {
            return _renamedMisc[reg.index()];
        }
        return idx == 0 ? _src0 : _src1;
    }

    void getRegOperand(const StaticInst *si, int idx, void *val) override
    {
        RegVal src = getRegOperand(si, idx);
        std::memcpy(val, &src, sizeof(src));
    }

    void *getWritableRegOperand(const StaticInst *si, int idx) override
    {
        return &_writtenReg;
    }

    void setRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const auto &reg = si->destRegIdx(idx);
        if (reg.is(RMiscRegClass)) {
            _renamedMisc[reg.index()] = val;
            return;
        }
        _writtenReg = val;
    }

    void setRegOperand(const StaticInst *si, int idx, const void *val) override
    {
        const auto &reg = si->destRegIdx(idx);
        if (reg.is(RMiscRegClass)) {
            std::memcpy(&_renamedMisc[reg.index()], val,
                        sizeof(_renamedMisc[reg.index()]));
            return;
        }
        std::memcpy(&_writtenReg, val, sizeof(_writtenReg));
    }

    RegVal readMiscRegOperand(const StaticInst *si, int idx) override { return 0; }
    void setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override {}
    RegVal readMiscReg(int misc_reg) override { return _tc->readMiscReg(misc_reg); }
    void setMiscReg(int misc_reg, RegVal val) override { _tc->setMiscReg(misc_reg, val); }
    const PCStateBase &pcState() const override { return _tc->pcState(); }
    void pcState(const PCStateBase &val) override { _tc->pcState(val); }
    Fault initiateMemMgmtCmd(Request::Flags flags) override { return NoFault; }
    Fault writeMem(uint8_t *data, unsigned int size, Addr addr, Request::Flags flags,
                   uint64_t *res, const std::vector<bool>& byte_enable) override
    {
        return NoFault;
    }
    void setStCondFailures(unsigned int sc_failures) override {}
    unsigned int readStCondFailures() const override { return 0; }
    ThreadContext *tcBase() const override { return _tc; }
    bool readPredicate() const override { return true; }
    void setPredicate(bool val) override {}
    bool readMemAccPredicate() const override { return true; }
    void setMemAccPredicate(bool val) override {}
    uint64_t newHtmTransactionUid() const override { return 0; }
    uint64_t getHtmTransactionUid() const override { return 0; }
    bool inHtmTransactionalState() const override { return false; }
    uint64_t getHtmTransactionalDepth() const override { return 0; }
    void demapPage(Addr vaddr, uint64_t asn) override {}
    void armMonitor(Addr address) override {}
    bool mwait(PacketPtr pkt) override { return false; }
    void mwaitAtomic(ThreadContext *tc) override {}
    AddressMonitor *getAddrMonitor() override { return nullptr; }

  private:
    ThreadContext *_tc = nullptr;
    RegVal _src0 = 0;
    RegVal _src1 = 0;
    RegVal _writtenReg = 0;
    std::array<RegVal, RiscvISA::rmisc_reg::NumRegs> _renamedMisc = {};
};

StaticInstPtr
decodeInst(RiscvISA::Decoder &decoder, uint32_t raw_inst)
{
    decoder.reset();
    RiscvISA::PCState nextPc(0);
    std::memcpy(decoder.moreBytesPtr(), &raw_inst, sizeof(raw_inst));
    decoder.moreBytes(nextPc, nextPc.instAddr());
    return decoder.decode(nextPc);
}

int
run()
{
    RiscvISAParams isaParams;
    isaParams.name = "matrix_sync_check_isa";
    isaParams.eventq_index = 0;
    auto isa = std::make_unique<RiscvISA::ISA>(isaParams);

    auto tc = std::make_unique<TestThreadContext>(isa.get());
    isa->setThreadContext(tc.get());

    RiscvDecoderParams decoderParams;
    decoderParams.name = "matrix_sync_check_decoder";
    decoderParams.eventq_index = 0;
    decoderParams.isa = isa.get();
    auto decoder = std::make_unique<RiscvISA::Decoder>(decoderParams);
    tc->setDecoder(decoder.get());

    TestExecContext xc(tc.get());

    constexpr uint32_t tokenIdx = 3;
    constexpr RegVal acquireTarget = 2;
    constexpr RegVal baseAddrA = 0x1000;
    constexpr RegVal baseAddrB = 0x2000;
    constexpr RegVal baseAddrC = 0x3000;
    constexpr RegVal strideA = 64;
    constexpr RegVal strideB = 64;
    constexpr RegVal strideC = 128;
    constexpr RegVal tileM = 8;
    constexpr RegVal tileN = 16;
    constexpr RegVal tileK = 24;
    constexpr RegVal csrTileM = 12;
    constexpr RegVal csrTileN = 20;
    constexpr RegVal csrTileK = 28;
    constexpr uint32_t tr0 = 0;
    constexpr uint32_t tr1 = 1;
    constexpr uint32_t acc0 = 4;

    auto mlae8 = decodeInst(*decoder, 0x0400802b);
    auto mlae16 = decodeInst(*decoder, 0x0400812b);
    auto mlbe8 = decodeInst(*decoder, 0x140100ab);
    auto mlbe16 = decodeInst(*decoder, 0x140101ab);
    auto mlce32 = decodeInst(*decoder, 0x4400822b);
    auto msce32 = decodeInst(*decoder, 0x2600822b);
    auto mzero = decodeInst(*decoder, 0x0c00022b);
    auto mmacc_w_b = decodeInst(*decoder, 0x1810022b);
    auto mfmacc_s_h = decodeInst(*decoder,
        encodeMatrixCarrier(Funct7Mfmacc, tr1, 8, 0x0, 20));
    auto msettilem = decodeInst(*decoder, encodeMsettile(Funct7Msettilem, 1));
    auto msettilen = decodeInst(*decoder, encodeMsettile(Funct7Msettilen, 1));
    auto msettilek = decodeInst(*decoder, encodeMsettile(Funct7Msettilek, 1));
    auto csrrwMtilem = decodeInst(*decoder,
        encodeCsrReg(RiscvISA::CSR_MTILEM, Funct3Csrrw, 1, 2));
    auto csrrwMtilen = decodeInst(*decoder,
        encodeCsrReg(RiscvISA::CSR_MTILEN, Funct3Csrrw, 1, 2));
    auto csrrwMtilek = decodeInst(*decoder,
        encodeCsrReg(RiscvISA::CSR_MTILEK, Funct3Csrrw, 1, 2));
    auto csrrwiXmxrm = decodeInst(*decoder, encodeCsrrwi(RiscvISA::CSR_XMXRM, 1, 3));
    auto csrrwiXmfrm = decodeInst(*decoder, encodeCsrrwi(RiscvISA::CSR_XMFRM, 1, 5));
    auto csrrsiXmsaten = decodeInst(*decoder, encodeCsrrsi(RiscvISA::CSR_XMSATEN, 1, 1));
    auto csrrwXmcsr = decodeInst(*decoder,
        encodeCsrReg(RiscvISA::CSR_XMCSR, Funct3Csrrw, 1, 2));
    auto macquire = decodeInst(*decoder, encodeMatrixSync(Funct7Macquire, tokenIdx, 1));
    auto mrelease = decodeInst(*decoder, encodeMatrixSync(Funct7Mrelease, tokenIdx));
    auto msyncregreset = decodeInst(*decoder, encodeMatrixSync(Funct7Msyncregreset, tokenIdx));

    if (!check(static_cast<bool>(mlae8), "mlae8 decode failed") ||
        !check(static_cast<bool>(mlae16), "mlae16 decode failed") ||
        !check(static_cast<bool>(mlbe8), "mlbe8 decode failed") ||
        !check(static_cast<bool>(mlbe16), "mlbe16 decode failed") ||
        !check(static_cast<bool>(mlce32), "mlce32 decode failed") ||
        !check(static_cast<bool>(msce32), "msce32 decode failed") ||
        !check(static_cast<bool>(mzero), "mzero decode failed") ||
        !check(static_cast<bool>(mmacc_w_b), "mmacc_w_b decode failed") ||
        !check(static_cast<bool>(mfmacc_s_h), "mfmacc_s_h decode failed") ||
        !check(static_cast<bool>(msettilem), "msettilem decode failed") ||
        !check(static_cast<bool>(msettilen), "msettilen decode failed") ||
        !check(static_cast<bool>(msettilek), "msettilek decode failed") ||
        !check(static_cast<bool>(csrrwMtilem), "csrrw mtilem decode failed") ||
        !check(static_cast<bool>(csrrwMtilen), "csrrw mtilen decode failed") ||
        !check(static_cast<bool>(csrrwMtilek), "csrrw mtilek decode failed") ||
        !check(static_cast<bool>(csrrwiXmxrm), "csrrwi xmxrm decode failed") ||
        !check(static_cast<bool>(csrrwiXmfrm), "csrrwi xmfrm decode failed") ||
        !check(static_cast<bool>(csrrsiXmsaten), "csrrsi xmsaten decode failed") ||
        !check(static_cast<bool>(csrrwXmcsr), "csrrw xmcsr decode failed") ||
        !check(static_cast<bool>(macquire), "macquire decode failed") ||
        !check(static_cast<bool>(mrelease), "mrelease decode failed") ||
        !check(static_cast<bool>(msyncregreset), "msyncregreset decode failed")) {
        return 1;
    }

    if (!check(mlae8->opClass() == MatrixMemOp, "mlae8 opClass mismatch") ||
        !check(mlae16->opClass() == MatrixMemOp, "mlae16 opClass mismatch") ||
        !check(mlbe8->opClass() == MatrixMemOp, "mlbe8 opClass mismatch") ||
        !check(mlbe16->opClass() == MatrixMemOp, "mlbe16 opClass mismatch") ||
        !check(mlce32->opClass() == MatrixMemOp, "mlce32 opClass mismatch") ||
        !check(msce32->opClass() == MatrixMemOp, "msce32 opClass mismatch") ||
        !check(mzero->opClass() == MatrixArithOp, "mzero opClass mismatch") ||
        !check(mmacc_w_b->opClass() == MatrixMmaOp, "mmacc_w_b opClass mismatch") ||
        !check(mfmacc_s_h->opClass() == MatrixMmaOp, "mfmacc_s_h opClass mismatch") ||
        !check(csrrwiXmxrm->opClass() == No_OpClass,
               "csrrwi xmxrm should use the normal CSR path") ||
        !check(csrrwiXmfrm->opClass() == No_OpClass,
               "csrrwi xmfrm should use the normal CSR path") ||
        !check(csrrsiXmsaten->opClass() == No_OpClass,
               "csrrsi xmsaten should use the normal CSR path") ||
        !check(csrrwXmcsr->opClass() == No_OpClass,
               "csrrw xmcsr should use the normal CSR path") ||
        !check(mrelease->opClass() == MatrixReleaseOp, "mrelease opClass mismatch")) {
        return 1;
    }
    if (!check(mmacc_w_b->numSrcRegs() == 3,
               "mmacc_w_b should only read renamed mtilem/mtilen/mtilek") ||
        !check(mfmacc_s_h->numSrcRegs() == 3,
               "mfmacc_s_h should only read renamed mtilem/mtilen/mtilek")) {
        return 1;
    }
    if (!check(msettilem->isSerializeAfter() && msettilem->isNonSpeculative(),
               "msettilem should be non-speculative and serialize after") ||
        !check(msettilen->isSerializeAfter() && msettilen->isNonSpeculative(),
               "msettilen should be non-speculative and serialize after") ||
        !check(msettilek->isSerializeAfter() && msettilek->isNonSpeculative(),
               "msettilek should be non-speculative and serialize after")) {
        return 1;
    }

    xc.setSrc0(tileM);
    if (!check(msettilem->execute(&xc, nullptr) == NoFault,
               "msettilem should succeed")) {
        return 1;
    }
    xc.setSrc0(tileN);
    if (!check(msettilen->execute(&xc, nullptr) == NoFault,
               "msettilen should succeed")) {
        return 1;
    }
    xc.setSrc0(tileK);
    if (!check(msettilek->execute(&xc, nullptr) == NoFault,
               "msettilek should succeed")) {
        return 1;
    }
    if (!check(csrrwiXmxrm->execute(&xc, nullptr) == NoFault,
               "csrrwi xmxrm should succeed") ||
        !check(csrrwiXmfrm->execute(&xc, nullptr) == NoFault,
               "csrrwi xmfrm should succeed") ||
        !check(csrrsiXmsaten->execute(&xc, nullptr) == NoFault,
               "csrrsi xmsaten should succeed")) {
        return 1;
    }

    xc.setSrc0(baseAddrA);
    xc.setSrc1(strideA);
    if (!check(mlae8->execute(&xc, nullptr) == NoFault,
               "mlae8 should succeed")) {
        return 1;
    }
    if (!check(mlae16->execute(&xc, nullptr) == NoFault,
               "mlae16 should succeed")) {
        return 1;
    }
    const auto &lsuA16 = isa->lastMatrixLsuRequest();
    if (!check(lsuA16.valid && lsuA16.isLoad && lsuA16.isA &&
               !lsuA16.isB && !lsuA16.isAcc && !lsuA16.transpose,
               "mlae16 request flags should match matrix A fp16 load") ||
        !check(lsuA16.widths == RiscvISA::ISA::MatrixSewE16 &&
               lsuA16.elemType == matrix::MatrixElemType::Fp16,
               "mlae16 request width/elem type should be e16/fp16")) {
        return 1;
    }
    xc.setSrc0(baseAddrB);
    xc.setSrc1(strideB);
    if (!check(mlbe8->execute(&xc, nullptr) == NoFault,
               "mlbe8 should succeed")) {
        return 1;
    }
    if (!check(mlbe16->execute(&xc, nullptr) == NoFault,
               "mlbe16 should succeed")) {
        return 1;
    }
    const auto &lsuB16 = isa->lastMatrixLsuRequest();
    if (!check(lsuB16.valid && lsuB16.isLoad && !lsuB16.isA &&
               lsuB16.isB && !lsuB16.isAcc && lsuB16.transpose,
               "mlbe16 request flags should match matrix B fp16 load") ||
        !check(lsuB16.widths == RiscvISA::ISA::MatrixSewE16 &&
               lsuB16.elemType == matrix::MatrixElemType::Fp16,
               "mlbe16 request width/elem type should be e16/fp16")) {
        return 1;
    }
    xc.setSrc0(baseAddrC);
    xc.setSrc1(strideC);
    if (!check(mlce32->execute(&xc, nullptr) == NoFault,
               "mlce32 should succeed") ||
        !check(msce32->execute(&xc, nullptr) == NoFault,
               "msce32 should succeed") ||
        !check(mzero->execute(&xc, nullptr) == NoFault,
               "mzero should succeed")) {
        return 1;
    }

    if (!check(mmacc_w_b->execute(&xc, nullptr) == NoFault,
               "mmacc_w_b should succeed")) {
        return 1;
    }
    const auto &mmaReq = isa->lastMatrixMmaRequest();
    if (!check(mmaReq.valid && !mmaReq.isFp,
               "mmacc_w_b should record an integer mma request") ||
        !check(mmaReq.md == acc0 && mmaReq.ms1 == tr0 && mmaReq.ms2 == tr1,
               "mmacc_w_b register shape should match acc/tr operands") ||
        !check(mmaReq.mtilem == tileM && mmaReq.mtilen == tileN &&
               mmaReq.mtilek == tileK,
               "mmacc_w_b tile shape should match current mtile state") ||
        !check(mmaReq.types1 == 0x4 && mmaReq.types2 == 0x4 &&
               mmaReq.typed == 0x2,
               "mmacc_w_b type encoding should match int8->int32") ||
        !check(mmaReq.lhsElemType == matrix::MatrixElemType::Int8 &&
               mmaReq.rhsElemType == matrix::MatrixElemType::Int8 &&
               mmaReq.dstElemType == matrix::MatrixElemType::Int32,
               "mmacc_w_b elem types should match integer path") ||
        !check(mmaReq.op == 0x0c,
               "mmacc_w_b op metadata should match decoded funct7") ||
        !check(mmaReq.rm == 3 && mmaReq.frm == 5 && mmaReq.sat == 1,
               "mmacc_w_b should use current xmxrm/xmfrm/xmsaten CSR values")) {
        return 1;
    }

    if (!check(mfmacc_s_h->execute(&xc, nullptr) == NoFault,
               "mfmacc_s_h should succeed")) {
        return 1;
    }
    const auto &fpMmaReq = isa->lastMatrixMmaRequest();
    if (!check(fpMmaReq.valid && fpMmaReq.isFp,
               "mfmacc_s_h should record an fp mma request") ||
        !check(fpMmaReq.md == acc0 && fpMmaReq.ms1 == tr0 && fpMmaReq.ms2 == tr1,
               "mfmacc_s_h register shape should match acc/tr operands") ||
        !check(fpMmaReq.mtilem == tileM && fpMmaReq.mtilen == tileN &&
               fpMmaReq.mtilek == tileK,
               "mfmacc_s_h tile shape should match current mtile state") ||
        !check(fpMmaReq.types1 == 0x1 && fpMmaReq.types2 == 0x1 &&
               fpMmaReq.typed == 0x2,
               "mfmacc_s_h type encoding should match fp16->fp32") ||
        !check(fpMmaReq.lhsElemType == matrix::MatrixElemType::Fp16 &&
               fpMmaReq.rhsElemType == matrix::MatrixElemType::Fp16 &&
               fpMmaReq.dstElemType == matrix::MatrixElemType::Int32,
               "mfmacc_s_h elem types should match fp16/fp16->fp32") ||
        !check(fpMmaReq.op == Funct7Mfmacc && fpMmaReq.op != mmaReq.op,
               "mfmacc_s_h op metadata should match decoded funct7") ||
        !check(fpMmaReq.rm == 3 && fpMmaReq.frm == 5 && fpMmaReq.sat == 1,
               "mfmacc_s_h should use current xmxrm/xmfrm/xmsaten CSR values")) {
        return 1;
    }

    xc.setSrc0(csrTileM);
    if (!check(csrrwMtilem->execute(&xc, nullptr) == NoFault,
               "csrrw mtilem should succeed")) {
        return 1;
    }
    xc.setSrc0(csrTileN);
    if (!check(csrrwMtilen->execute(&xc, nullptr) == NoFault,
               "csrrw mtilen should succeed")) {
        return 1;
    }
    xc.setSrc0(csrTileK);
    if (!check(csrrwMtilek->execute(&xc, nullptr) == NoFault,
               "csrrw mtilek should succeed")) {
        return 1;
    }
    if (!check(mmacc_w_b->execute(&xc, nullptr) == NoFault,
               "mmacc_w_b after csrrw tile writes should succeed")) {
        return 1;
    }
    const auto &csrTileMmaReq = isa->lastMatrixMmaRequest();
    if (!check(csrTileMmaReq.mtilem == csrTileM &&
               csrTileMmaReq.mtilen == csrTileN &&
               csrTileMmaReq.mtilek == csrTileK,
               "csrrw mtile writes should update renamed tile carriers")) {
        return 1;
    }

    constexpr RegVal csrXmcsr = (2u) | (6u << 8);
    xc.setSrc0(csrXmcsr);
    if (!check(csrrwXmcsr->execute(&xc, nullptr) == NoFault,
               "csrrw xmcsr should succeed")) {
        return 1;
    }
    if (!check(mmacc_w_b->execute(&xc, nullptr) == NoFault,
               "mmacc_w_b after csrrw xmcsr should succeed")) {
        return 1;
    }
    const auto &csrXmcsrMmaReq = isa->lastMatrixMmaRequest();
    if (!check(csrXmcsrMmaReq.rm == 2 &&
               csrXmcsrMmaReq.frm == 6 &&
               csrXmcsrMmaReq.sat == 0,
               "csrrw xmcsr should update current xmxrm/xmfrm/xmsaten CSR values")) {
        return 1;
    }

    xc.setSrc0(acquireTarget);
    Fault fault = macquire->execute(&xc, nullptr);
    if (!check(fault != NoFault, "macquire should block before any release") ||
        !check(dynamic_cast<ReExec *>(fault.get()) != nullptr,
               "macquire should return ReExec before token is ready")) {
        return 1;
    }

    if (!check(mrelease->execute(&xc, nullptr) == NoFault,
               "mrelease should succeed")) {
        return 1;
    }
    if (!check(msyncregreset->execute(&xc, nullptr) == NoFault,
               "msyncregreset should succeed")) {
        return 1;
    }

    std::cout << "matrix_sync_check: PASS" << std::endl;
    return 0;
}

} // anonymous namespace
} // namespace gem5

int
main()
{
    return gem5::run();
}

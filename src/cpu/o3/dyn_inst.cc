/*
 * Copyright (c) 2010-2011, 2021 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#include "cpu/o3/dyn_inst.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "arch/riscv/insts/mem.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/pcstate.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/intmath.hh"
#include "cpu/o3/trace/TraceInstruction.hh"
#include "debug/DynInst.hh"
#include "debug/IQ.hh"
#include "debug/LSQ.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"

namespace gem5
{

namespace o3
{

namespace
{

constexpr uint8_t MatrixOpcode7 = 0x2b;
constexpr uint8_t SystemOpcode7 = 0x73;

using MatrixCommitBoundary = DynInst::MatrixCommitBoundary;
using MatrixExecPayload = ExecContext::MatrixExecPayload;
using MatrixInstClass = DynInst::MatrixInstClass;
using MatrixRouteKind = DynInst::MatrixRouteKind;
using MatrixStateOperand = DynInst::MatrixStateOperand;

const char *
matrixInstClassNameStr(MatrixInstClass inst_class)
{
    switch (inst_class) {
      case MatrixInstClass::Init:
        return "init";
      case MatrixInstClass::Set:
        return "set";
      case MatrixInstClass::Csr:
        return "csr";
      case MatrixInstClass::Lsu:
        return "lsu";
      case MatrixInstClass::Mma:
        return "mma";
      case MatrixInstClass::Arith:
        return "arith";
      case MatrixInstClass::Sync:
        return "sync";
      case MatrixInstClass::None:
        return "unknown";
    }

    return "unknown";
}

const char *
matrixRouteNameStr(MatrixRouteKind route)
{
    switch (route) {
      case MatrixRouteKind::Init:
        return "init";
      case MatrixRouteKind::SetTile:
        return "settile";
      case MatrixRouteKind::TileCsr:
        return "tile-csr";
      case MatrixRouteKind::Lsu:
        return "lsu";
      case MatrixRouteKind::Mma:
        return "mma";
      case MatrixRouteKind::Arith:
        return "arith";
      case MatrixRouteKind::Release:
        return "release";
      case MatrixRouteKind::SyncReset:
        return "sync-reset";
      case MatrixRouteKind::Acquire:
        return "acquire";
      case MatrixRouteKind::None:
        return "unknown";
    }

    return "unknown";
}

const char *
matrixCommitBoundaryNameStr(MatrixCommitBoundary boundary)
{
    switch (boundary) {
      case MatrixCommitBoundary::ArchStateOnly:
        return "arch-only";
      case MatrixCommitBoundary::TokenSyncOnly:
        return "token-sync";
      case MatrixCommitBoundary::FutureAmuProducer:
        return "future-amu";
      case MatrixCommitBoundary::None:
        return "none";
    }

    return "none";
}

const char *
matrixPayloadKindNameStr(MatrixExecPayload::Kind kind)
{
    switch (kind) {
      case MatrixExecPayload::Kind::Lsu:
        return "lsu";
      case MatrixExecPayload::Kind::Mma:
        return "mma";
      case MatrixExecPayload::Kind::Arith:
        return "arith";
      case MatrixExecPayload::Kind::Release:
        return "release";
      case MatrixExecPayload::Kind::None:
        return "none";
    }

    return "none";
}

bool
isMatrixTileCsr(uint16_t csr_idx)
{
    return csr_idx == RiscvISA::CSR_MTILEM ||
           csr_idx == RiscvISA::CSR_MTILEN ||
           csr_idx == RiscvISA::CSR_MTILEK;
}

MatrixStateOperand
tileCsrToMatrixStateOperand(uint16_t csr_idx)
{
    switch (csr_idx) {
      case RiscvISA::CSR_MTILEM:
        return MatrixStateOperand::Mtilem;
      case RiscvISA::CSR_MTILEN:
        return MatrixStateOperand::Mtilen;
      case RiscvISA::CSR_MTILEK:
        return MatrixStateOperand::Mtilek;
      default:
        return MatrixStateOperand::None;
    }
}

bool
csrWritesMatrixState(uint8_t funct3, uint8_t rs1)
{
    switch (funct3) {
      case 0x1:
      case 0x5:
        return true;
      case 0x2:
      case 0x3:
      case 0x6:
      case 0x7:
        return rs1 != 0;
      default:
        return false;
    }
}

} // namespace

DynInst::DynInst(const Arrays &arrays, const StaticInstPtr &static_inst,
        const StaticInstPtr &_macroop, InstSeqNum seq_num, CPU *_cpu)
    : seqNum(seq_num), staticInst(static_inst),
      xsMeta(new XsDynInstMeta(seq_num)),
      cpu(_cpu),
      _numSrcs(arrays.numSrcs), _numDests(arrays.numDests),
      _flatDestIdx(arrays.flatDestIdx), _destIdx(arrays.destIdx),
      _prevDestIdx(arrays.prevDestIdx), _srcIdx(arrays.srcIdx),
      _readySrcIdx(arrays.readySrcIdx), macroop(_macroop)
{
    std::fill(_readySrcIdx, _readySrcIdx + (numSrcs() + 7) / 8, 0);

    status.reset();

    instFlags.reset();
    instFlags[RecordResult] = true;
    instFlags[Predicate] = true;
    instFlags[MemAccPredicate] = true;

    initMatrixInstInfo();
    matrixExecPayload = {};
    stagedMatrixTokenResetValid = false;
    stagedMatrixTokenReset = 0;

#ifndef NDEBUG
    ++cpu->instcount;

    if (cpu->instcount > 1500) {
#ifdef DEBUG
        cpu->dumpInsts();
        dumpSNList();
#endif
        assert(cpu->instcount <= 1500);
    }

    DPRINTF(DynInst,
        "DynInst: [sn:%lli] Instruction created. Instcount for %s = %i\n",
        seqNum, cpu->name(), cpu->instcount);
#endif

    if (isMatrixInst()) {
        DPRINTF(DynInst,
                "Matrix dyninst created [sn:%lli] class=%s route=%s "
                "boundary=%s opcode7=%#x funct7=%#x rd=x%u rs1=x%u rs2=x%u "
                "loadLike=%d storeLike=%d tokenLike=%d needAmu=%d "
                "dirtyMs=%d usesLsq=%d srcs=%u dests=%u.\n",
                seqNum, matrixInstClassName(), matrixRouteName(),
                matrixCommitBoundaryName(), matrixInst.opcode7,
                matrixInst.funct7, matrixInst.rd, matrixInst.rs1,
                matrixInst.rs2, matrixInst.loadLike,
                matrixInst.storeLike, matrixInst.tokenLike,
                matrixNeedAmuCtrl(), matrixDirtyMs(), matrixInst.usesLsq,
                matrixInst.staticNumSrcs, matrixInst.staticNumDests);
    }

#ifdef DEBUG
    cpu->snList.insert(seqNum);
#endif

}

DynInst::DynInst(const Arrays &arrays, const StaticInstPtr &static_inst,
        const StaticInstPtr &_macroop, const PCStateBase &_pc,
        const PCStateBase &pred_pc, InstSeqNum seq_num, CPU *_cpu)
    : DynInst(arrays, static_inst, _macroop, seq_num, _cpu)
{
    set(pc, _pc);
    set(predPC, pred_pc);
}

DynInst::DynInst(const Arrays &arrays, const StaticInstPtr &_staticInst,
        const StaticInstPtr &_macroop)
    : DynInst(arrays, _staticInst, _macroop, 0, nullptr)
{}

/*
 * This custom "new" operator uses the default "new" operator to allocate space
 * for a DynInst, but also pads out the number of bytes to make room for some
 * extra structures the DynInst needs. We save time and improve performance by
 * only going to the heap once to get space for all these structures.
 *
 * When a DynInst is allocated with new, the compiler will call this "new"
 * operator with "count" set to the number of bytes it needs to store the
 * DynInst. We ultimately call into the default new operator to get those
 * bytes, but before we do, we pad out "count" so that there will be extra
 * space for some structures the DynInst needs. We take into account both the
 * absolute size of these structures, and also what alignment they need.
 *
 * Once we've gotten a buffer large enough to hold the DynInst itself and these
 * extra structures, we construct the extra bits using placement new. This
 * constructs the structures in place in the space we created for them.
 *
 * Next, we return the buffer as the result of our operator. The compiler takes
 * that buffer and constructs the DynInst in the beginning of it using the
 * DynInst constructor.
 *
 * To avoid having to calculate where these extra structures are twice, once
 * when making room for them and initializing them, and then once again in the
 * DynInst constructor, we also pass in a structure called "arrays" which holds
 * pointers to them. The fields of "arrays" are initialized in this operator,
 * and are then consumed in the DynInst constructor.
 */
void *
DynInst::operator new(size_t count, Arrays &arrays)
{
    // Convenience variables for brevity.
    const auto num_dests = arrays.numDests;
    const auto num_srcs = arrays.numSrcs;

    // Figure out where everything will go.
    uintptr_t inst = 0;
    size_t inst_size = count;

    uintptr_t flat_dest_idx = roundUp(inst + inst_size, alignof(RegId));
    size_t flat_dest_idx_size = sizeof(*arrays.flatDestIdx) * num_dests;

    uintptr_t dest_idx =
        roundUp(flat_dest_idx + flat_dest_idx_size, alignof(VirtRegId));
    size_t dest_idx_size = sizeof(*arrays.destIdx) * num_dests;

    uintptr_t prev_dest_idx =
        roundUp(dest_idx + dest_idx_size, alignof(VirtRegId));
    size_t prev_dest_idx_size = sizeof(*arrays.prevDestIdx) * num_dests;

    uintptr_t src_idx =
        roundUp(prev_dest_idx + prev_dest_idx_size, alignof(VirtRegId));
    size_t src_idx_size = sizeof(*arrays.srcIdx) * num_srcs;

    uintptr_t ready_src_idx =
        roundUp(src_idx + src_idx_size, alignof(uint8_t));
    size_t ready_src_idx_size =
        sizeof(*arrays.readySrcIdx) * ((num_srcs + 7) / 8);

    // Figure out how much space we need in total.
    size_t total_size = ready_src_idx + ready_src_idx_size;

    // Actually allocate it.
    uint8_t *buf = (uint8_t *)::operator new(total_size);

    // Fill in "arrays" with pointers to all the arrays.
    arrays.flatDestIdx = (RegId *)(buf + flat_dest_idx);
    arrays.destIdx = (VirtRegId *)(buf + dest_idx);
    arrays.prevDestIdx = (VirtRegId *)(buf + prev_dest_idx);
    arrays.srcIdx = (VirtRegId *)(buf + src_idx);
    arrays.readySrcIdx = (uint8_t *)(buf + ready_src_idx);

    // Initialize all the extra components.
    new (arrays.flatDestIdx) RegId[num_dests];
    new (arrays.destIdx) VirtRegId[num_dests];
    new (arrays.prevDestIdx) VirtRegId[num_dests];
    new (arrays.srcIdx) VirtRegId[num_srcs];
    new (arrays.readySrcIdx) uint8_t[num_srcs];

    return buf;
}

// Because of the custom "new" operator that allocates more bytes than the
// size of the DynInst object, AddressSanitizer throw new-delete-type-mismatch.
// Adding a custom delete function is enough to shut down this false positive
void
DynInst::operator delete(void *ptr)
{
    ::operator delete(ptr);
}

DynInst::~DynInst()
{
    /*
     * The buffer this DynInst occupies also holds some of the structures it
     * points to. We need to call their destructors manually to make sure that
     * they're cleaned up appropriately, but we don't need to free their memory
     * explicitly since that's part of the DynInst's buffer and is already
     * going to be freed as part of deleting the DynInst.
     */
    for (int i = 0; i < _numDests; i++) {
        _flatDestIdx[i].~RegId();
        _destIdx[i].~VirtRegId();
        _prevDestIdx[i].~VirtRegId();
    }

    for (int i = 0; i < _numSrcs; i++)
        _srcIdx[i].~VirtRegId();

    for (int i = 0; i < ((_numSrcs + 7) / 8); i++)
        _readySrcIdx[i].~uint8_t();

#if TRACING_ON
    if (debug::O3PipeView) {
        Tick fetch = fetchTick;
        // fetchTick can be -1 if the instruction fetched outside the trace
        // window.
        if (fetch != -1) {
            Tick val;
            // Print info needed by the pipeline activity viewer.
            DPRINTFR(O3PipeView, "O3PipeView:fetch:%llu:0x%08llx:%d:%llu:%s\n",
                     fetch,
                     pcState().instAddr(),
                     pcState().microPC(),
                     seqNum,
                     staticInst->disassemble(pcState().instAddr()));

            val = (decodeTick == -1) ? 0 : fetch + decodeTick;
            DPRINTFR(O3PipeView, "O3PipeView:decode:%llu\n", val);
            val = (renameTick == -1) ? 0 : fetch + renameTick;
            DPRINTFR(O3PipeView, "O3PipeView:rename:%llu\n", val);
            val = (dispatchTick == -1) ? 0 : fetch + dispatchTick;
            DPRINTFR(O3PipeView, "O3PipeView:dispatch:%llu\n", val);
            val = (issueTick == -1) ? 0 : fetch + issueTick;
            DPRINTFR(O3PipeView, "O3PipeView:issue:%llu\n", val);
            val = (completeTick == -1) ? 0 : fetch + completeTick;
            DPRINTFR(O3PipeView, "O3PipeView:complete:%llu\n", val);
            val = (commitTick == -1) ? 0 : fetch + commitTick;

            Tick valS = (storeTick == -1) ? 0 : fetch + storeTick;
            DPRINTFR(O3PipeView, "O3PipeView:retire:%llu:store:%llu\n",
                    val, valS);
        }
    }
#endif

    delete [] memData;
    delete traceData;
    fault = NoFault;

#ifndef NDEBUG
    --cpu->instcount;

    DPRINTF(DynInst,
        "DynInst: [sn:%lli] Instruction destroyed. Instcount for %s = %i\n",
        seqNum, cpu->name(), cpu->instcount);
#endif
#ifdef DEBUG
    cpu->snList.erase(seqNum);
#endif
};


#ifdef DEBUG
void
DynInst::dumpSNList()
{
    std::set<InstSeqNum>::iterator sn_it = cpu->snList.begin();

    int count = 0;
    while (sn_it != cpu->snList.end()) {
        cprintf("%i: [sn:%lli] not destroyed\n", count, (*sn_it));
        count++;
        sn_it++;
    }
}
#endif

bool
DynInst::isDependentOn(const DynInstPtr &other) const
{
    for (int i = 0; i < numSrcRegs(); i++) {
        for (int j = 0; j < other->numDestRegs(); j++) {
            if (srcRegIdx(i) == other->destRegIdx(j)) {
                return true;
            }
        }
    }
    return false;
}

void
DynInst::dump()
{
    cprintf("T%d : %#08d `", threadNumber, pc->instAddr());
    std::cout << staticInst->disassemble(pc->instAddr());
    cprintf("'\n");
}

void
DynInst::dump(std::string &outstring)
{
    std::ostringstream s;
    s << "T" << threadNumber << " : 0x" << pc->instAddr() << " "
      << staticInst->disassemble(pc->instAddr());

    outstring = s.str();
}

void
DynInst::markSrcRegReady()
{
    DPRINTF(IQ, "[sn:%lli] has %d ready out of %d sources. RTI %d)\n",
            seqNum, readyRegs+1, numSrcRegs(), readyToIssue());
    if (++readyRegs == numSrcRegs()) {
        setCanIssue();
    }
}

void
DynInst::markSrcRegReady(RegIndex src_idx)
{
    readySrcIdx(src_idx, true);
    markSrcRegReady();
}

void
DynInst::clearSrcRegReady(RegIndex src_idx)
{
    assert(readySrcIdx(src_idx));
    readySrcIdx(src_idx, false);
    readyRegs--;
    clearCanIssue();
}

void DynInst::resetNumSrcRegReady(uint8_t n) {
    readyRegs = n;
    if (n < numSrcRegs()) {
        clearCanIssue();
    }
    memset(_readySrcIdx, 0, (numSrcs() + 7) / 8);
}

void
DynInst::initMatrixInstInfo()
{
    auto *riscv_inst =
        dynamic_cast<const RiscvISA::RiscvStaticInst *>(staticInst.get());
    if (!riscv_inst) {
        return;
    }

    const auto &mach_inst = riscv_inst->machInst;
    const bool is_matrix_opcode = mach_inst.opcode7 == MatrixOpcode7;
    const bool is_csr =
        mach_inst.opcode7 == SystemOpcode7 && mach_inst.funct3 != 0;
    const auto csr_idx = static_cast<uint16_t>(mach_inst.funct12);
    const bool is_matrix_csr = is_csr && isMatrixTileCsr(csr_idx);
    if (!is_matrix_opcode && !is_matrix_csr) {
        return;
    }

    matrixInst.valid = true;
    matrixInst.opcode7 = mach_inst.opcode7;
    matrixInst.funct7 = mach_inst.funct7;
    matrixInst.funct3 = mach_inst.funct3;
    matrixInst.rd = mach_inst.rd;
    matrixInst.rs1 = mach_inst.rs1;
    matrixInst.rs2 = mach_inst.rs2;
    matrixInst.csrIndex = is_matrix_csr ? csr_idx : 0;
    matrixInst.staticNumSrcs = staticInst->numSrcRegs();
    matrixInst.staticNumDests = staticInst->numDestRegs();

    const auto max_srcs =
        std::min<size_t>(matrixInst.staticNumSrcs, MaxMatrixSummarySrcs);
    for (size_t i = 0; i < max_srcs; ++i) {
        const auto &reg = staticInst->srcRegIdx(i);
        matrixInst.staticSrcClasses[i] = reg.classValue();
        matrixInst.staticSrcIndices[i] = reg.index();
    }

    const auto max_dests =
        std::min<size_t>(matrixInst.staticNumDests, MaxMatrixSummaryDests);
    for (size_t i = 0; i < max_dests; ++i) {
        const auto &reg = staticInst->destRegIdx(i);
        matrixInst.staticDestClasses[i] = reg.classValue();
        matrixInst.staticDestIndices[i] = reg.index();
    }

    if (is_matrix_csr) {
        matrixInst.instClass = MatrixInstClass::Csr;
        matrixInst.commitBoundary = MatrixCommitBoundary::ArchStateOnly;
        const bool writes_matrix_state =
            csrWritesMatrixState(mach_inst.funct3, mach_inst.rs1);

        const auto csr_operand = tileCsrToMatrixStateOperand(csr_idx);
        matrixInst.stateReads[0] = csr_operand;
        if (writes_matrix_state) {
            matrixInst.stateWrites[0] = csr_operand;
        }

        matrixInst.route = MatrixRouteKind::TileCsr;

        return;
    }

    auto setMatrixRoute = [&](MatrixInstClass inst_class,
                              MatrixRouteKind route,
                              MatrixCommitBoundary boundary) {
        matrixInst.instClass = inst_class;
        matrixInst.route = route;
        matrixInst.commitBoundary = boundary;
    };

    auto setStateReads =
        [&](std::initializer_list<MatrixStateOperand> operands) {
            size_t idx = 0;
            for (auto operand : operands) {
                if (idx >= matrixInst.stateReads.size()) {
                    break;
                }
                matrixInst.stateReads[idx++] = operand;
            }
        };

    switch (mach_inst.funct7) {
      case 0x00:
        setMatrixRoute(
            MatrixInstClass::Init,
            MatrixRouteKind::Init,
            MatrixCommitBoundary::ArchStateOnly);
        break;
      case 0x08:
      case 0x09:
        setMatrixRoute(
            MatrixInstClass::Set,
            MatrixRouteKind::SetTile,
            MatrixCommitBoundary::ArchStateOnly);
        matrixInst.stateWrites[0] = MatrixStateOperand::Mtilek;
        break;
      case 0x10:
      case 0x11:
        setMatrixRoute(
            MatrixInstClass::Set,
            MatrixRouteKind::SetTile,
            MatrixCommitBoundary::ArchStateOnly);
        matrixInst.stateWrites[0] = MatrixStateOperand::Mtilem;
        break;
      case 0x18:
      case 0x19:
        setMatrixRoute(
            MatrixInstClass::Set,
            MatrixRouteKind::SetTile,
            MatrixCommitBoundary::ArchStateOnly);
        matrixInst.stateWrites[0] = MatrixStateOperand::Mtilen;
        break;
      case 0x02:
        setMatrixRoute(
            MatrixInstClass::Lsu,
            MatrixRouteKind::Lsu,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.loadLike = true;
        matrixInst.usesLsq = true;
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilem,
            MatrixStateOperand::Mtilek
        });
        break;
      case 0x0a:
        setMatrixRoute(
            MatrixInstClass::Lsu,
            MatrixRouteKind::Lsu,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.loadLike = true;
        matrixInst.usesLsq = true;
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilek,
            MatrixStateOperand::Mtilen
        });
        break;
      case 0x12:
        setMatrixRoute(
            MatrixInstClass::Lsu,
            MatrixRouteKind::Lsu,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.loadLike = true;
        matrixInst.usesLsq = true;
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilem,
            MatrixStateOperand::Mtilek
        });
        break;
      case 0x1a:
        setMatrixRoute(
            MatrixInstClass::Lsu,
            MatrixRouteKind::Lsu,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.loadLike = true;
        matrixInst.usesLsq = true;
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilek,
            MatrixStateOperand::Mtilen
        });
        break;
      case 0x22:
        setMatrixRoute(
            MatrixInstClass::Lsu,
            MatrixRouteKind::Lsu,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.loadLike = true;
        matrixInst.usesLsq = true;
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilem,
            MatrixStateOperand::Mtilen
        });
        break;
      case 0x13:
        setMatrixRoute(
            MatrixInstClass::Lsu,
            MatrixRouteKind::Lsu,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.storeLike = true;
        matrixInst.usesLsq = true;
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilem,
            MatrixStateOperand::Mtilen
        });
        break;
      case 0x04:
      case 0x0c:
        setMatrixRoute(
            MatrixInstClass::Mma,
            MatrixRouteKind::Mma,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilem,
            MatrixStateOperand::Mtilen,
            MatrixStateOperand::Mtilek
        });
        break;
      case 0x06:
        setMatrixRoute(
            MatrixInstClass::Arith,
            MatrixRouteKind::Arith,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.needAmuCtrlCandidate = true;
        setStateReads({
            MatrixStateOperand::Mtilem,
            MatrixStateOperand::Mtilen
        });
        break;
      case 0x40:
        setMatrixRoute(
            MatrixInstClass::Sync,
            MatrixRouteKind::SyncReset,
            MatrixCommitBoundary::TokenSyncOnly);
        matrixInst.tokenLike = true;
        matrixInst.tokenIndex = mach_inst.rs2;
        break;
      case 0x48:
        setMatrixRoute(
            MatrixInstClass::Sync,
            MatrixRouteKind::Release,
            MatrixCommitBoundary::FutureAmuProducer);
        matrixInst.tokenLike = true;
        matrixInst.needAmuCtrlCandidate = true;
        matrixInst.tokenIndex = mach_inst.rs2;
        break;
      case 0x50:
        setMatrixRoute(
            MatrixInstClass::Sync,
            MatrixRouteKind::Acquire,
            MatrixCommitBoundary::TokenSyncOnly);
        matrixInst.tokenLike = true;
        matrixInst.tokenIndex = mach_inst.rs2;
        break;
      default:
        matrixInst.valid = false;
        matrixInst.instClass = MatrixInstClass::None;
        matrixInst.route = MatrixRouteKind::None;
        matrixInst.commitBoundary = MatrixCommitBoundary::None;
        break;
    }

}

const char *
DynInst::matrixInstClassName() const
{
    return matrixInstClassNameStr(matrixInst.instClass);
}

const char *
DynInst::matrixRouteName() const
{
    return matrixRouteNameStr(matrixInst.route);
}

const char *
DynInst::matrixCommitBoundaryName() const
{
    return matrixCommitBoundaryNameStr(matrixInst.commitBoundary);
}

const char *
DynInst::matrixPayloadKindName() const
{
    return matrixPayloadKindNameStr(matrixExecPayload.kind);
}

void
DynInst::noteMatrixRenamed()
{
    if (!matrixInst.valid) {
        return;
    }
    matrixInst.renameAttempt++;
    matrixInst.renameSeen = true;
    matrixInst.robInserted = false;
    matrixInst.robAttempt = 0;
    matrixInst.squashed = false;
}

void
DynInst::noteMatrixRobInserted()
{
    if (!matrixInst.valid) {
        return;
    }
    matrixInst.robInserted = true;
    matrixInst.robAttempt = matrixInst.renameAttempt;
}

void
DynInst::setSquashed()
{
    status.set(Squashed);
    xsMeta->squashed = true;
    matrixInst.squashed = matrixInst.valid;
    if (matrixInst.valid) {
        matrixInst.squashAttempt = matrixInst.renameAttempt;
    }

    if (matrixInst.valid) {
        DPRINTF(DynInst,
                "Matrix dyninst squashed [sn:%llu] class=%s route=%s "
                "boundary=%s renamed=%d rob=%d renameAttempt=%u "
                "robAttempt=%u squashAttempt=%u.\n",
                seqNum, matrixInstClassName(), matrixRouteName(),
                matrixCommitBoundaryName(), matrixInst.renameSeen,
                matrixInst.robInserted, matrixInst.renameAttempt,
                matrixInst.robAttempt, matrixInst.squashAttempt);
    }

    if (!isPinnedRegsRenamed() || isPinnedRegsSquashDone())
        return;

    // This inst has been renamed already so it may go through rename
    // again (e.g. if the squash is due to memory access order violation).
    // Reset the write counters for all pinned destination register to ensure
    // that they are in a consistent state for a possible re-rename. This also
    // ensures that dest regs will be pinned to the same phys register if
    // re-rename happens.
    for (int idx = 0; idx < numDestRegs(); idx++) {
        PhysRegIdPtr phys_dest_reg = renamedDestIdx(idx);
        if (phys_dest_reg->isPinned()) {
            phys_dest_reg->incrNumPinnedWrites();
            if (isPinnedRegsWritten())
                phys_dest_reg->incrNumPinnedWritesToComplete();
        }
    }
    setPinnedRegsSquashDone();
}

Fault
DynInst::execute()
{
    // @todo: Pretty convoluted way to avoid squashing from happening
    // when using the TC during an instruction's execution
    // (specifically for instructions that have side-effects that use
    // the TC).  Fix this.
    bool no_squash_from_TC = thread->noSquashFromTC;
    thread->noSquashFromTC = true;

    fault = staticInst->execute(this, traceData);

    thread->noSquashFromTC = no_squash_from_TC;

    return fault;
}

Fault
DynInst::initiateAcc()
{
    // @todo: Pretty convoluted way to avoid squashing from happening
    // when using the TC during an instruction's execution
    // (specifically for instructions that have side-effects that use
    // the TC).  Fix this.
    bool no_squash_from_TC = thread->noSquashFromTC;
    thread->noSquashFromTC = true;

    fault = staticInst->initiateAcc(this, traceData);

    thread->noSquashFromTC = no_squash_from_TC;

    return fault;
}

Fault
DynInst::completeAcc(PacketPtr pkt)
{
    // @todo: Pretty convoluted way to avoid squashing from happening
    // when using the TC during an instruction's execution
    // (specifically for instructions that have side-effects that use
    // the TC).  Fix this.
    bool no_squash_from_TC = thread->noSquashFromTC;
    thread->noSquashFromTC = true;

    if (cpu->checker) {
        if (isStoreConditional()) {
            reqToVerify->setExtraData(pkt->req->getExtraData());
        }
    }

    fault = staticInst->completeAcc(pkt, this, traceData);

    if (fault == NoFault) {
        if (isStoreConditional()) {
            lockedWriteSuccess(pkt->req->getExtraData() != 0);
        }
    }

    thread->noSquashFromTC = no_squash_from_TC;

    return fault;
}

void DynInst::buildStoreAddrUop()
{
    assert(staticInst->isSplitStoreAddr());
    assert(numSrcRegs() == 2);
    // tramsform self to storeAddr uop

    // mark addr ready
    if (!this->readySrcIdx(1)) this->markSrcRegReady(1);
    this->renameSrcReg(1, VirtRegId(UnifiedRenameMap::getInvalid()));
}

DynInstPtr DynInst::createStoreDataUop()
{
    assert(staticInst->isSplitStoreAddr());
    Arrays arrays;
    arrays.numSrcs = 1;
    arrays.numDests = 0;
    StaticInstPtr stdinst = new RiscvISA::StoreData(this->staticInst);
    DynInstPtr stduop = new (arrays) DynInst(arrays, stdinst, macroop, *this->pc, *this->predPC, this->seqNum, cpu);

    // Propagate essential execution context to the data uop.
    // KISS: copy only what downstream stages require (PC/predPC/tid/thread).
    stduop->pcState(this->pcState());
    stduop->setPredTarg(this->readPredTarg());
    stduop->setPredTaken(this->readPredTaken());
    stduop->setTid(this->threadNumber);
    stduop->setThreadState(this->thread);

    stduop->renameSrcReg(0, this->extRenamedSrcIdx(1));

    if (this->readySrcIdx(1)) {
        stduop->markSrcRegReady(0);
    }

    stduop->sqIdx = this->sqIdx;
    stduop->sqIt = this->sqIt;

    return stduop;
}

void
DynInst::trap(const Fault &fault)
{
    cpu->trap(fault, threadNumber, staticInst);
}

Fault
DynInst::initiateMemRead(Addr addr, unsigned size, Request::Flags flags,
                               const std::vector<bool> &byte_enable)
{
    assert(byte_enable.size() == size);
    // In trace-mode, prefer the address recorded in the trace (if any)
    // KISS: override only when metadata exists; YAGNI: first address only.
    if (cpu->isTraceMode()) {
        const o3::TraceInstruction* ti = cpu->getTraceInstMetadata(seqNum);
        if (ti && ti->getLoad() && !ti->getLoadAddresses().empty()) {
            Addr trace_addr = ti->getLoadAddresses()[0];
            if (trace_addr != 0) {
                addr = trace_addr;
            }
        }
    }
    return cpu->pushRequest(
        dynamic_cast<DynInstPtr::PtrType>(this),
        /* ld */ true, nullptr, size, addr, flags, nullptr, nullptr,
        byte_enable);
}

Fault
DynInst::initiateMemMgmtCmd(Request::Flags flags)
{
    const unsigned int size = 8;
    return cpu->pushRequest(
            dynamic_cast<DynInstPtr::PtrType>(this),
            /* ld */ true, nullptr, size, 0x0ul, flags, nullptr, nullptr,
            std::vector<bool>(size, true));
}

Fault
DynInst::writeMem(uint8_t *data, unsigned size, Addr addr,
                        Request::Flags flags, uint64_t *res,
                        const std::vector<bool> &byte_enable)
{
    assert(byte_enable.size() == size);
    // In trace-mode, prefer the store address recorded in trace (if any).
    if (cpu->isTraceMode()) {
        const o3::TraceInstruction* ti = cpu->getTraceInstMetadata(seqNum);
        if (ti && ti->getStore() && !ti->getStoreAddresses().empty()) {
            Addr trace_addr = ti->getStoreAddresses()[0];
            if (trace_addr != 0) {
                addr = trace_addr;
            }
        }
    }
    return cpu->pushRequest(
        dynamic_cast<DynInstPtr::PtrType>(this),
        /* st */ false, data, size, addr, flags, res, nullptr,
        byte_enable);
}

Fault
DynInst::initiateMemAMO(Addr addr, unsigned size, Request::Flags flags,
                              AtomicOpFunctorPtr amo_op)
{
    // atomic memory instructions do not have data to be written to memory yet
    // since the atomic operations will be executed directly in cache/memory.
    // Therefore, its `data` field is nullptr.
    // Atomic memory requests need to carry their `amo_op` fields to cache/
    // memory
    return cpu->pushRequest(
            dynamic_cast<DynInstPtr::PtrType>(this),
            /* atomic */ false, nullptr, size, addr, flags, nullptr,
            std::move(amo_op), std::vector<bool>(size, true));
}
void
DynInst::printDisassemblyAndResult(const std::string &site) const
{
    DPRINTF(CommitTrace, "%s [sn:%lu pc:%#lx] enDqT: %lu, exDqT: %lu, readyT: %lu, CompleT:%lu, %s",
            site.c_str(), seqNum, pcState().instAddr(),
            enterDQTick, exitDQTick, readyTick, completionTick,
            staticInst->disassemble(pcState().instAddr()));
    if (instResult.size() > 0) {
        DPRINTFR(CommitTrace, ", res: %#lx", instResult.front().as<uint64_t>());
    } else if (numDestRegs() > 0 && isVector()) {
        uint64_t val[RiscvISA::NumVecElemPerVecReg];
        cpu->getArchReg(destRegIdx(0), val, threadNumber);
        std::string s_val;
        for (int j = RiscvISA::NumVecElemPerVecReg - 1; j >= 0; j--) {
            s_val += csprintf("%016lx", val[j]);
            if (j != 0) {
                s_val += "_";
            }
        }
        DPRINTFR(CommitTrace, ", res: %s", s_val);
    }
    if (isMemRef()) {
        DPRINTFR(CommitTrace, ", paddr: %#lx", physEffAddr);
    }
    DPRINTFR(CommitTrace, "\n");
}

} // namespace o3
} // namespace gem5

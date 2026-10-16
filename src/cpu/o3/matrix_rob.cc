/*
 * Copyright (c) 2012 ARM Limited
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/rob.hh"
#include "matrix/MatrixRequestValidator.hh"

namespace gem5
{

namespace o3
{

namespace
{

bool
toCuteRequest(const ExecContext::MatrixExecPayload &payload,
              InstSeqNum seq, matrix::CuteRequest &out)
{
    if (!payload.valid) {
        return false;
    }

    switch (payload.kind) {
      case ExecContext::MatrixExecPayload::Kind::Release:
        out = matrix::CuteRequest::makeRelease(seq, payload.tokenIndex);
        return true;
      case ExecContext::MatrixExecPayload::Kind::Arith:
        if (payload.op == 0x06) {
            out = matrix::CuteRequest::makeArithZero(
                seq, matrix::MatrixBankKind::C, payload.md,
                payload.mtilem, payload.mtilen,
                payload.elemType);
            return true;
        }
        return false;
      case ExecContext::MatrixExecPayload::Kind::Mma:
        out = matrix::CuteRequest::makeMma(
            seq, payload.md, payload.ms1, payload.ms2,
            payload.mtilem, payload.mtilen, payload.mtilek);
        out.mma.isFp = payload.isFp;
        out.mma.rm = payload.rm;
        out.mma.frm = payload.frm;
        out.mma.types1 = payload.types1;
        out.mma.types2 = payload.types2;
        out.mma.typed = payload.typed;
        out.mma.lhsElemType = payload.lhsElemType;
        out.mma.rhsElemType = payload.rhsElemType;
        out.mma.dstElemType = payload.dstElemType;
        out.mma.sat = payload.sat;
        out.op = payload.op;
        return true;
      case ExecContext::MatrixExecPayload::Kind::Lsu:
        if (!payload.isLoad && !payload.isStore) {
            return false;
        }
        out = {};
        out.seq = seq;
        out.kind = matrix::CuteRequestKind::Lsu;
        out.op = payload.op;
        out.lsu.ms = payload.ms;
        out.lsu.widths = payload.widths;
        out.lsu.isStore = payload.isStore;
        out.lsu.transpose = payload.transpose;
        out.lsu.isAcc = payload.isAcc;
        out.lsu.isA = payload.isA;
        out.lsu.isB = payload.isB;
        out.lsu.baseAddr = payload.baseAddr;
        out.lsu.physBaseAddr = payload.physBaseAddr;
        out.lsu.pageMappings = payload.pageMappings;
        out.lsu.stride = payload.stride;
        out.lsu.row = payload.row;
        out.lsu.column = payload.column;
        out.lsu.elemType = payload.elemType;
        return true;
      case ExecContext::MatrixExecPayload::Kind::None:
        return false;
    }

    return false;
}

} // anonymous namespace

bool
ROB::shouldTrackMatrixAmu(const DynInstPtr &inst) const
{
    return cpu->isMatrixBackendEnabled() && inst && inst->isMatrixInst() &&
           inst->matrixNeedAmuCtrl();
}

void
ROB::insertMatrixAmuEntry(const DynInstPtr &inst)
{
    if (!shouldTrackMatrixAmu(inst)) {
        return;
    }

    matrixAmuBuffers[inst->threadNumber].allocate(inst->seqNum);
}

void
ROB::noteMatrixAmuWriteback(const DynInstPtr &inst)
{
    if (!shouldTrackMatrixAmu(inst)) {
        return;
    }

    matrix::CuteRequest backend_req = {};
    const bool backend_req_valid =
        inst->isMatrixInst() && inst->matrixPayloadValid() &&
        toCuteRequest(inst->matrixPayload(), inst->seqNum, backend_req);
    backend_req.pc = inst->pcState().instAddr();

    if (inst->getFault() == NoFault && inst->matrixPayloadValid() &&
        (!backend_req_valid ||
         matrix::validateMatrixRequest(backend_req) !=
             matrix::MatrixRequestError::None)) {
        const auto *riscv_inst = dynamic_cast<const RiscvISA::RiscvStaticInst *>(
            inst->staticInst.get());
        panic_if(!riscv_inst, "Matrix request lacks RISC-V static inst");
        inst->getFault() = std::make_shared<RiscvISA::IllegalInstFault>(
            "unsupported matrix request", riscv_inst->machInst);
    }

    matrixAmuBuffers[inst->threadNumber].noteWriteback(
        inst->seqNum, inst->getFault() != NoFault,
        backend_req_valid,
        backend_req);
}

void
ROB::noteMatrixAmuCommit(const DynInstPtr &inst)
{
    if (!shouldTrackMatrixAmu(inst)) {
        return;
    }

    matrixAmuBuffers[inst->threadNumber].noteCommit(
        inst->seqNum);
}

bool
ROB::peekReadyMatrixAmuEntry(ThreadID tid, MatrixAmuEntry &entry_out)
{
    if (!cpu->isMatrixBackendEnabled()) {
        return false;
    }
    return matrixAmuBuffers[tid].peekReady(entry_out);
}

bool
ROB::popReadyMatrixAmuEntry(ThreadID tid, MatrixAmuEntry &entry_out)
{
    if (!cpu->isMatrixBackendEnabled()) {
        return false;
    }
    return matrixAmuBuffers[tid].popReady(entry_out);
}

unsigned
ROB::numFreeMatrixAmuEntries(ThreadID tid)
{
    if (!cpu->isMatrixBackendEnabled()) {
        return matrixAmuBufferEntries;
    }
    return matrixAmuBuffers[tid].numFreeEntries();
}

void
ROB::squashMatrixAmuEntry(ThreadID tid, InstSeqNum seq_num)
{
    if (!cpu->isMatrixBackendEnabled()) {
        return;
    }
    matrixAmuBuffers[tid].squash(seq_num);
}

} // namespace o3
} // namespace gem5

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


#include "arch/riscv/insts/static_inst.hh"
#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

void
DynInst::initMatrixInstInfo()
{
    if (!cpu->isMatrixBackendEnabled()) {
        return;
    }
    auto *riscv_inst =
        dynamic_cast<const RiscvISA::RiscvStaticInst *>(staticInst.get());
    if (!riscv_inst) {
        return;
    }

    const auto &static_info = riscv_inst->matrixStaticInfo();
    if (!static_info.valid) {
        return;
    }

    static_cast<RiscvISA::MatrixStaticInfo &>(matrixState.matrixInst) = static_info;
}

bool
DynInst::stageMatrixExecPayload(const ExecContext::MatrixExecPayload &payload)
{
    matrixState.matrixExecPayload = payload;
    return true;
}

bool
DynInst::stageMatrixTokenReset(RegVal token_idx)
{
    matrixState.stagedMatrixTokenReset = token_idx;
    matrixState.stagedMatrixTokenResetValid = true;
    return true;
}

bool
DynInst::stageMatrixTokenRelease(RegVal token_idx)
{
    panic("Legacy stageMatrixTokenRelease is unsupported; "
          "matrix release token must be updated from backend completion.");
}

bool
DynInst::readStagedMatrixTokenReady(RegVal token_idx, RegVal threshold,
                                  bool &ready)
{
    ready = cpu->matrixShadowTokenReady(threadNumber, token_idx,
                                       threshold);
    return true;
}

} // namespace o3
} // namespace gem5

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


#ifndef __ARCH_RISCV_REGS_VECTOR_HH__
#define __ARCH_RISCV_REGS_VECTOR_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "arch/generic/vec_pred_reg.hh"
#include "arch/generic/vec_reg.hh"

namespace gem5
{

namespace RiscvISA
{

constexpr unsigned NumVecElemPerVecReg = 4;
using VecElem = uint64_t;
constexpr size_t vlenb = NumVecElemPerVecReg * sizeof(VecElem);
constexpr size_t VLEN = vlenb * 8;
using VecRegContainer =
    gem5::VecRegContainer<vlenb>;
using vreg_t = VecRegContainer;

using VecPredReg =
    gem5::VecPredRegT<VecElem, NumVecElemPerVecReg, false, false>;
using ConstVecPredReg =
    gem5::VecPredRegT<VecElem, NumVecElemPerVecReg, false, true>;
using VecPredRegContainer = VecPredReg::Container;
using VecRegIdx = uint8_t;

const int NumVecStandardRegs = 32;
const int NumVecMemInternalRegs = 4;

const int NumVecRegs = NumVecStandardRegs + NumVecMemInternalRegs;

const std::vector<std::string> VecRegNames = {
    "v0",   "v1",   "v2",   "v3",   "v4",   "v5",   "v6",   "v7",
    "v8",   "v9",   "v10",  "v11",  "v12",  "v13",  "v14",  "v15",
    "v16",  "v17",  "v18",  "v19",  "v20",  "v21",  "v22",  "v23",
    "v24",  "v25",  "v26",  "v27",  "v28",  "v29",  "v30",  "v31",
    "vmem0", "vmem1", "vmem2", "vmem3"
};

// internal vector indices
constexpr int VecMemInternalReg0 = NumVecStandardRegs;

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_REGS_VECTOR_HH__

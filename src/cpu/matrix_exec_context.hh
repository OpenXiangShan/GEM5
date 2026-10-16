/*
 * Copyright (c) 2014, 2016-2018, 2020-2021 ARM Limited
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
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * Copyright (c) 2015 Advanced Micro Devices, Inc.
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

#ifndef __CPU_MATRIX_EXEC_CONTEXT_HH__
#define __CPU_MATRIX_EXEC_CONTEXT_HH__

#include "base/types.hh"
#include "matrix/MatrixTypes.hh"

namespace gem5
{

struct MatrixExecPayload
{
    enum class Kind : uint8_t
    {
        None,
        Lsu,
        Mma,
        Arith,
        Release
    };

    static const char *
    kindName(Kind kind)
    {
        switch (kind) {
          case Kind::Lsu:
            return "lsu";
          case Kind::Mma:
            return "mma";
          case Kind::Arith:
            return "arith";
          case Kind::Release:
            return "release";
          case Kind::None:
            return "none";
        }

        return "none";
    }

    const char *
    kindName() const
    {
        return kindName(kind);
    }

    Kind kind = Kind::None;
    bool valid = false;
    bool isLoad = false;
    bool isStore = false;
    bool transpose = false;
    bool isAcc = false;
    bool isA = false;
    bool isB = false;
    bool isFp = false;
    bool sat = false;
    RegVal op = 0;
    RegVal md = 0;
    RegVal ms = 0;
    RegVal ms1 = 0;
    RegVal ms2 = 0;
    RegVal tokenIndex = 0;
    RegVal baseAddr = 0;
    RegVal physBaseAddr = 0;
    matrix::FrozenMatrixPageMappings pageMappings;
    RegVal stride = 0;
    RegVal row = 0;
    RegVal column = 0;
    RegVal widths = 0;
    RegVal mtilem = 0;
    RegVal mtilen = 0;
    RegVal mtilek = 0;
    RegVal rm = 0;
    RegVal frm = 0;
    RegVal types1 = 0;
    RegVal types2 = 0;
    RegVal typed = 0;
    matrix::MatrixElemType elemType = matrix::MatrixElemType::Int8;
    matrix::MatrixElemType lhsElemType = matrix::MatrixElemType::Int8;
    matrix::MatrixElemType rhsElemType = matrix::MatrixElemType::Int8;
    matrix::MatrixElemType dstElemType = matrix::MatrixElemType::Int32;
};

} // namespace gem5

#endif // __CPU_MATRIX_EXEC_CONTEXT_HH__

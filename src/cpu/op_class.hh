/*
 * Copyright (c) 2010, 2017-2018 ARM Limited
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
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
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

#ifndef __CPU__OP_CLASS_HH__
#define __CPU__OP_CLASS_HH__

#include "enums/OpClass.hh"

namespace gem5
{

/*
 * Do a bunch of wonky stuff to maintain backward compatability so I
 * don't have to change code in a zillion places.
 */
using enums::OpClass;
using enums::No_OpClass;

static const OpClass IntAluOp = enums::IntAlu;
static const OpClass IntBrOp = enums::IntBr;
static const OpClass IntMultOp = enums::IntMult;
static const OpClass IntDivOp = enums::IntDiv;
static const OpClass FloatAddOp = enums::FloatAdd;
static const OpClass FloatCmpOp = enums::FloatCmp;
static const OpClass FloatCvtOp = enums::FloatCvt;
static const OpClass FloatMvOp = enums::FloatMv;
static const OpClass FloatMultOp = enums::FloatMult;
static const OpClass FloatMultAccOp = enums::FloatMultAcc;
static const OpClass FloatDivOp = enums::FloatDiv;
static const OpClass FloatMiscOp = enums::FloatMisc;
static const OpClass FloatSqrtOp = enums::FloatSqrt;
static const OpClass MemReadOp = enums::MemRead;
static const OpClass MemWriteOp = enums::MemWrite;
static const OpClass MemAtomicOp = enums::MemAtomic;
static const OpClass FloatMemReadOp = enums::FloatMemRead;
static const OpClass FloatMemWriteOp = enums::FloatMemWrite;
static const OpClass StoreDataOp = enums::StoreData;
static const OpClass IprAccessOp = enums::IprAccess;
static const OpClass InstPrefetchOp = enums::InstPrefetch;

// riscv vload/store
static const OpClass VectorUnitStrideLoadOp = enums::VectorUnitStrideLoad;
static const OpClass VectorSegUnitStrideLoadOp = enums::VectorSegUnitStrideLoad;
static const OpClass VectorUnitStrideStoreOp = enums::VectorUnitStrideStore;
static const OpClass VectorSegUnitStrideStoreOp = enums::VectorSegUnitStrideStore;
static const OpClass VectorUnitStrideMaskLoadOp = enums::VectorUnitStrideMaskLoad;
static const OpClass VectorSegUnitStrideMaskLoadOp = enums::VectorSegUnitStrideMaskLoad;
static const OpClass VectorUnitStrideMaskStoreOp = enums::VectorUnitStrideMaskStore;
static const OpClass VectorStridedLoadOp = enums::VectorStridedLoad;
static const OpClass VectorSegStridedLoadOp = enums::VectorSegStridedLoad;
static const OpClass VectorStridedStoreOp = enums::VectorStridedStore;
static const OpClass VectorIndexedLoadOp = enums::VectorIndexedLoad;
static const OpClass VectorSegIndexedLoadOp = enums::VectorSegIndexedLoad;
static const OpClass VectorIndexedStoreOp = enums::VectorIndexedStore;
static const OpClass VectorUnitStrideFaultOnlyFirstLoadOp
             = enums::VectorUnitStrideFaultOnlyFirstLoad;
static const OpClass VectorWholeRegisterLoadOp
             = enums::VectorWholeRegisterLoad;
static const OpClass VectorWholeRegisterStoreOp
             = enums::VectorWholeRegisterStore;

static const OpClass VectorIntegerArithOp = enums::VectorIntegerArith;
static const OpClass VectorFloatArithOp = enums::VectorFloatArith;
static const OpClass VectorFloatConvertOp = enums::VectorFloatConvert;
static const OpClass VectorIntegerReduceOp = enums::VectorIntegerReduce;
static const OpClass VectorFloatReduceOp = enums::VectorFloatReduce;
static const OpClass VectorMiscOp = enums::VectorMisc;
static const OpClass VectorIntegerExtensionOp = enums::VectorIntegerExtension;
static const OpClass VectorConfigOp = enums::VectorConfig;
static const OpClass Num_OpClasses = enums::Num_OpClass;

} // namespace gem5

#endif // __CPU__OP_CLASS_HH__

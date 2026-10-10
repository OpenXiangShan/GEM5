/*
 * Copyright (c) 2026 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the disclaimer; redistributions in
 * binary form must reproduce the above copyright notice, this list of
 * conditions and the disclaimer in the documentation and/or other materials
 * provided with the distribution; neither the name of the copyright holders
 * nor the names of any contributors may be used to endorse or promote
 * products derived from this software without specific written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_TRACE_TRACE_BRANCH_TRUTH_HH__
#define __CPU_O3_TRACE_TRACE_BRANCH_TRUTH_HH__

#include <cstdint>

namespace gem5
{
namespace o3
{

/**
 * Trace branch-truth override rules, extracted so that every consumer
 * (DecoupledBPUWithBTB::makeBranchInfo, BpTrace::BpTrace,
 * DecoupledBPUWithBTB::commitBranch) applies the *same* rule and the
 * anchored unit tests exercise it directly.
 *
 * Tombstone (port-gap chain): c7be4bd32a fixed trace-truth classification
 * CPU-side; the PR #1196 extraction 23b02c3f0d ported only part of it, and
 * b5fefebcef had to complete the BPU-stats consumers: a synthetic NOP
 * encoding classified a trace *conditional* branch as non-conditional,
 * including when processMisprediction checked a not-taken outcome. The
 * rule "trace metadata wins over static decode" must hold at every
 * consumer; a shared pure function makes the consumers structurally
 * symmetric so a future port cannot update one and forget the other.
 */
namespace TraceBranchTruth
{

/** Trace-side branch facts. An absent `present` field means "no trace
 *  metadata, keep the static-decode value" (default-constructed). */
struct TraceBranchFacts
{
    bool present = false;
    uint64_t nextPC = 0;
    bool isCond = false;
    bool isIndirect = false;
    bool isCall = false;
    bool isReturn = false;
    bool taken = false;
};

/** Collect facts from an instruction exposing the trace accessors
 *  (DynInst in production; a minimal fake in the anchored tests).
 *  Caller must have checked hasTraceBranchInfo(). */
template <typename Inst>
inline TraceBranchFacts
factsFrom(const Inst &inst)
{
    TraceBranchFacts f;
    f.present = true;
    f.nextPC = inst.traceBranchNextPC();
    f.isCond = inst.traceIsCond();
    f.isIndirect = inst.traceIsIndirect();
    f.isCall = inst.traceIsCall();
    f.isReturn = inst.traceIsReturn();
    f.taken = inst.traceBranchTaken();
    return f;
}

/** Trace truth wins; without metadata the static-decode value stands. */
inline uint64_t
effectiveTarget(uint64_t decoded_target, const TraceBranchFacts &t)
{
    return t.present ? t.nextPC : decoded_target;
}

inline bool
effectiveTaken(bool decoded_taken, const TraceBranchFacts &t)
{
    return t.present ? t.taken : decoded_taken;
}

/** Apply the classification override to any BranchInfo-like object with
 *  public isCond/isIndirect/isDirect/isCall/isReturn fields. */
template <typename BranchInfoLike>
inline void
applyTraceClassification(BranchInfoLike &info, const TraceBranchFacts &t)
{
    info.isCond = t.isCond;
    info.isIndirect = t.isIndirect;
    info.isDirect = !t.isIndirect;
    info.isCall = t.isCall;
    info.isReturn = t.isReturn;
}

} // namespace TraceBranchTruth
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_BRANCH_TRUTH_HH__

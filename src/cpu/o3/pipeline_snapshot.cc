/*
 * Copyright (c) 2026 The Regents of The University of Michigan
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
#include "cpu/o3/pipeline_snapshot.hh"

namespace gem5
{

namespace o3
{

namespace
{

template <class Slots>
uint64_t
countForwardInstRefs(const Slots &slots)
{
    uint64_t refs = 0;
    for (const auto &entry : slots.entries) {
        if (entry.size > 0)
            refs += entry.size;
    }
    return refs;
}

uint64_t
countFetchGroups(const PipelineTimeBufferSnapshots::Frame::Slots<FetchStruct>
        &slots)
{
    uint64_t groups = 0;
    for (const auto &entry : slots.entries) {
        if (entry.size > 0)
            ++groups;
    }
    return groups;
}

} // anonymous namespace

void
PipelineTimeBufferSnapshots::configureWindow(unsigned cycles)
{
    if (cycles == 0)
        cycles = 1;

    inputWindow_.clear();
    inputWindow_.resize(cycles);
    outputWindow_.clear();
    outputWindow_.resize(cycles);
}

PipelineTimeBufferSnapshots::Frame &
PipelineTimeBufferSnapshots::captureFrame(std::vector<Frame> &window,
                                          Cycles cycle,
                                          unsigned &last_index)
{
    if (window.empty())
        configureWindow(1);

    last_index = static_cast<uint64_t>(cycle) % window.size();
    return window[last_index];
}

const PipelineTimeBufferSnapshots::Frame *
PipelineTimeBufferSnapshots::lookupFrame(const std::vector<Frame> &window,
                                         Cycles cycle) const
{
    if (window.empty())
        return nullptr;

    const auto index = static_cast<uint64_t>(cycle) % window.size();
    const auto &frame = window[index];
    return frame.valid && frame.cycle == cycle ? &frame : nullptr;
}

unsigned
PipelineTimeBufferSnapshots::validFrames(
        const std::vector<Frame> &window) const
{
    unsigned valid = 0;
    for (const auto &frame : window) {
        if (frame.valid)
            ++valid;
    }
    return valid;
}

unsigned
PipelineTimeBufferSnapshots::Frame::capture(
        Cycles snapshot_cycle, const TimeBuffer<TimeStruct> &backward_buffer,
        const TimeBuffer<FetchStruct> &fetch_buffer,
        const TimeBuffer<DecodeStruct> &decode_buffer,
        const TimeBuffer<RenameStruct> &rename_buffer,
        const TimeBuffer<IEWStruct> &iew_buffer)
{
    return captureShifted(snapshot_cycle, backward_buffer, fetch_buffer,
                          decode_buffer, rename_buffer, iew_buffer, 0);
}

unsigned
PipelineTimeBufferSnapshots::Frame::captureShifted(
        Cycles snapshot_cycle, const TimeBuffer<TimeStruct> &backward_buffer,
        const TimeBuffer<FetchStruct> &fetch_buffer,
        const TimeBuffer<DecodeStruct> &decode_buffer,
        const TimeBuffer<RenameStruct> &rename_buffer,
        const TimeBuffer<IEWStruct> &iew_buffer,
        int source_offset_shift)
{
    cycle = snapshot_cycle;
    valid = true;

    unsigned captured = 0;
    captured += backward.captureShifted(backward_buffer, source_offset_shift);
    captured += fetchToDecode.captureShifted(fetch_buffer,
                                             source_offset_shift);
    captured += decodeToRename.captureShifted(decode_buffer,
                                              source_offset_shift);
    captured += renameToIEW.captureShifted(rename_buffer,
                                           source_offset_shift);
    captured += iewToCommit.captureShifted(iew_buffer, source_offset_shift);

    return captured;
}

unsigned
PipelineTimeBufferSnapshots::captureInputs(
        Cycles cycle, const TimeBuffer<TimeStruct> &backward,
        const TimeBuffer<FetchStruct> &fetch,
        const TimeBuffer<DecodeStruct> &decode,
        const TimeBuffer<RenameStruct> &rename,
        const TimeBuffer<IEWStruct> &iew)
{
    auto &frame = captureFrame(inputWindow_, cycle, lastInputIndex_);
    return frame.capture(cycle, backward, fetch, decode, rename, iew);
}

unsigned
PipelineTimeBufferSnapshots::captureOutputs(
        Cycles cycle, const TimeBuffer<TimeStruct> &backward,
        const TimeBuffer<FetchStruct> &fetch,
        const TimeBuffer<DecodeStruct> &decode,
        const TimeBuffer<RenameStruct> &rename,
        const TimeBuffer<IEWStruct> &iew)
{
    auto &frame = captureFrame(outputWindow_, cycle, lastOutputIndex_);
    return frame.capture(cycle, backward, fetch, decode, rename, iew);
}

const PipelineTimeBufferSnapshots::Frame &
PipelineTimeBufferSnapshots::inputFrame() const
{
    return inputWindow_.empty() ? inputFrame_ : inputWindow_[lastInputIndex_];
}

const PipelineTimeBufferSnapshots::Frame &
PipelineTimeBufferSnapshots::outputFrame() const
{
    return outputWindow_.empty() ? outputFrame_ :
        outputWindow_[lastOutputIndex_];
}

const PipelineTimeBufferSnapshots::Frame *
PipelineTimeBufferSnapshots::inputFrame(Cycles cycle) const
{
    if (const auto *frame = lookupFrame(inputWindow_, cycle))
        return frame;
    return inputFrame_.valid && inputFrame_.cycle == cycle ?
        &inputFrame_ : nullptr;
}

const PipelineTimeBufferSnapshots::Frame *
PipelineTimeBufferSnapshots::outputFrame(Cycles cycle) const
{
    if (const auto *frame = lookupFrame(outputWindow_, cycle))
        return frame;
    return outputFrame_.valid && outputFrame_.cycle == cycle ?
        &outputFrame_ : nullptr;
}

unsigned
PipelineTimeBufferSnapshots::validInputFrames() const
{
    return validFrames(inputWindow_);
}

unsigned
PipelineTimeBufferSnapshots::validOutputFrames() const
{
    return validFrames(outputWindow_);
}

PipelineTimeBufferSnapshots::PrepareSummary
PipelineTimeBufferSnapshots::summarizeFrame(const Frame &frame)
{
    PrepareSummary summary;
    summary.cycle = frame.cycle;
    summary.valid = frame.valid;
    if (!frame.valid)
        return summary;

    summary.forwardInstRefs += countForwardInstRefs(frame.fetchToDecode);
    summary.forwardInstRefs += countForwardInstRefs(frame.decodeToRename);
    summary.forwardInstRefs += countForwardInstRefs(frame.renameToIEW);
    summary.forwardInstRefs += countForwardInstRefs(frame.iewToCommit);
    summary.fetchGroups = countFetchGroups(frame.fetchToDecode);

    for (const auto &entry : frame.backward.entries) {
        for (int tid = 0; tid < MaxThreads; ++tid) {
            if (entry.decodeInfo[tid].squash)
                ++summary.squashSignals;
            if (entry.decodeInfo[tid].branchMispredict)
                ++summary.branchMispredictSignals;
            if (entry.commitInfo[tid].squash)
                ++summary.squashSignals;
            if (entry.commitInfo[tid].robSquashing)
                ++summary.robSquashingSignals;
            summary.resolvedCFIs += entry.iewInfo[tid].resolvedCFIs.size();
        }
    }

    for (const auto &entry : frame.iewToCommit.entries) {
        for (int tid = 0; tid < MaxThreads; ++tid) {
            if (entry.squash[tid])
                ++summary.squashSignals;
            if (entry.branchMispredict[tid])
                ++summary.branchMispredictSignals;
        }
    }

    return summary;
}

PipelineTimeBufferSnapshots::PrepareSummary
PipelineTimeBufferSnapshots::prepareInputSummary() const
{
    return summarizeFrame(inputFrame());
}

void
PipelineTimeBufferSnapshots::mergeInputSummary(
        const PrepareSummary &summary)
{
    lastInputPrepareSummary_ = summary;
}

} // namespace o3
} // namespace gem5

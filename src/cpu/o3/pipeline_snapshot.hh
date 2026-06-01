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

#ifndef __CPU_O3_PIPELINE_SNAPSHOT_HH__
#define __CPU_O3_PIPELINE_SNAPSHOT_HH__

#include <cassert>
#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/o3/comm.hh"
#include "cpu/timebuf.hh"

namespace gem5
{

namespace o3
{

class PipelineTimeBufferSnapshots
{
  public:
    struct PrepareSummary
    {
        Cycles cycle = Cycles(0);
        bool valid = false;
        uint64_t forwardInstRefs = 0;
        uint64_t fetchGroups = 0;
        uint64_t squashSignals = 0;
        uint64_t robSquashingSignals = 0;
        uint64_t branchMispredictSignals = 0;
        uint64_t resolvedCFIs = 0;
    };

    struct Frame
    {
        Cycles cycle = Cycles(0);
        bool valid = false;

        template <class T>
        struct Slots
        {
            int past = 0;
            int future = 0;
            std::vector<T> entries;

            unsigned capture(const TimeBuffer<T> &buffer);
            unsigned captureShifted(const TimeBuffer<T> &buffer,
                                    int source_offset_shift);
            const T &at(int offset) const;
            const T *get(int offset) const;
        };

        Slots<TimeStruct> backward;
        Slots<FetchStruct> fetchToDecode;
        Slots<DecodeStruct> decodeToRename;
        Slots<RenameStruct> renameToIEW;
        Slots<IEWStruct> iewToCommit;

        unsigned capture(Cycles cycle, const TimeBuffer<TimeStruct> &backward,
                         const TimeBuffer<FetchStruct> &fetch,
                         const TimeBuffer<DecodeStruct> &decode,
                         const TimeBuffer<RenameStruct> &rename,
                         const TimeBuffer<IEWStruct> &iew);
        unsigned captureShifted(Cycles cycle,
                                const TimeBuffer<TimeStruct> &backward,
                                const TimeBuffer<FetchStruct> &fetch,
                                const TimeBuffer<DecodeStruct> &decode,
                                const TimeBuffer<RenameStruct> &rename,
                                const TimeBuffer<IEWStruct> &iew,
                                int source_offset_shift);
    };

    unsigned captureInputs(Cycles cycle, const TimeBuffer<TimeStruct> &backward,
                           const TimeBuffer<FetchStruct> &fetch,
                           const TimeBuffer<DecodeStruct> &decode,
                           const TimeBuffer<RenameStruct> &rename,
                           const TimeBuffer<IEWStruct> &iew);
    unsigned captureOutputs(Cycles cycle,
                            const TimeBuffer<TimeStruct> &backward,
                            const TimeBuffer<FetchStruct> &fetch,
                            const TimeBuffer<DecodeStruct> &decode,
                            const TimeBuffer<RenameStruct> &rename,
                            const TimeBuffer<IEWStruct> &iew);

    void configureWindow(unsigned cycles);
    const Frame &inputFrame() const;
    const Frame &outputFrame() const;
    const Frame *inputFrame(Cycles cycle) const;
    const Frame *outputFrame(Cycles cycle) const;
    unsigned windowCapacity() const { return inputWindow_.size(); }
    unsigned validInputFrames() const;
    unsigned validOutputFrames() const;
    const PrepareSummary &lastInputPrepareSummary() const
    {
        return lastInputPrepareSummary_;
    }

    static PrepareSummary summarizeFrame(const Frame &frame);
    PrepareSummary prepareInputSummary() const;
    void mergeInputSummary(const PrepareSummary &summary);

  private:
    Frame &captureFrame(std::vector<Frame> &window, Cycles cycle,
                        unsigned &last_index);
    const Frame *lookupFrame(const std::vector<Frame> &window,
                             Cycles cycle) const;
    unsigned validFrames(const std::vector<Frame> &window) const;

    Frame inputFrame_;
    Frame outputFrame_;
    std::vector<Frame> inputWindow_;
    std::vector<Frame> outputWindow_;
    unsigned lastInputIndex_ = 0;
    unsigned lastOutputIndex_ = 0;
    PrepareSummary lastInputPrepareSummary_;
};

template <class T>
unsigned
PipelineTimeBufferSnapshots::Frame::Slots<T>::capture(
        const TimeBuffer<T> &buffer)
{
    return captureShifted(buffer, 0);
}

template <class T>
unsigned
PipelineTimeBufferSnapshots::Frame::Slots<T>::captureShifted(
        const TimeBuffer<T> &buffer, int source_offset_shift)
{
    past = buffer.pastCycles();
    future = buffer.futureCycles();
    entries.clear();
    entries.reserve(past + future + 1);

    for (int offset = -past; offset <= future; ++offset) {
        const int source_offset = offset + source_offset_shift;
        if (source_offset >= -past && source_offset <= future) {
            entries.push_back(buffer[source_offset]);
        } else {
            entries.push_back(T{});
        }
    }

    return entries.size();
}

template <class T>
const T &
PipelineTimeBufferSnapshots::Frame::Slots<T>::at(int offset) const
{
    assert(offset >= -past && offset <= future);
    return entries[offset + past];
}

template <class T>
const T *
PipelineTimeBufferSnapshots::Frame::Slots<T>::get(int offset) const
{
    if (offset < -past || offset > future)
        return nullptr;
    return &entries[offset + past];
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_PIPELINE_SNAPSHOT_HH__

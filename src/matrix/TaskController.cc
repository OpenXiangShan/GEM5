/*
 * Copyright (c) 2026
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

#include "matrix/TaskController.hh"

#include <cassert>

#include "matrix/CUTETOP.hh"

namespace gem5
{

namespace matrix
{

// Active request issue path: fifo, headReady, issueHead, dispatchTask.
DetailedCuteBackend::DetailedCuteBackend(
    size_t fifo_depth, size_t ab_reg_count, size_t c_reg_count,
    statistics::Group *stats_parent)
    : DetailedCuteBackend(
          fifo_depth, ab_reg_count, c_reg_count, TimingConfig(),
          stats_parent)
{
}

DetailedCuteBackend::DetailedCuteBackend(
    size_t fifo_depth, size_t ab_reg_count, size_t c_reg_count,
    TimingConfig timing_config, statistics::Group *stats_parent)
    : fifo(fifo_depth), regFile(ab_reg_count, c_reg_count),
      scoreboard(ab_reg_count, c_reg_count),
      timingConfig(timing_config),
      localMmu(LocalMmuModel::Config{
          timing_config.localMmuMaxOutstanding}),
      matrixL2FillTable(MatrixL2FillTable::Config{
          timing_config.matrixL2FillTableEntries,
          timing_config.matrixL2FillBankFifoDepth}),
      cutePhaseStats(stats_parent)
{
}

bool
DetailedCuteBackend::canAccept(const CuteRequest &)
    const
{
    return fifo.canAccept();
}

void
DetailedCuteBackend::submit(const CuteRequest &req)
{
    assert(canAccept(req));
    fifo.enqueue(decodeCuteRequest(req));
}

DetailedCuteBackend::MicroTaskKind
DetailedCuteBackend::microTaskKindForEntry(const DecodedFifoEntry &entry) const
{
    if (entry.isLoad) {
        if (entry.request.lsu.isAcc) {
            return MicroTaskKind::CML;
        }
        return entry.request.lsu.isB ? MicroTaskKind::BML :
                                       MicroTaskKind::AML;
    }

    if (entry.isStore) {
        return MicroTaskKind::CML;
    }

    if (entry.isMma) {
        return MicroTaskKind::Compute;
    }

    if (entry.isZeroAcc) {
        return MicroTaskKind::CML;
    }

    if (entry.isZeroTr) {
        return MicroTaskKind::AML;
    }

    return MicroTaskKind::Release;
}

size_t
DetailedCuteBackend::activeTaskCount() const
{
    size_t active = 0;
    active += amlTask.has_value() ? 1 : 0;
    active += bmlTask.has_value() ? 1 : 0;
    active += cmlTask.has_value() ? 1 : 0;
    active += computeTasks.size();
    active += releaseTask.has_value() ? 1 : 0;
    return active;
}

bool
DetailedCuteBackend::issuePathReady(const DecodedFifoEntry &entry) const
{
    if (entry.isMma) {
        return computeUnitAvailable(ComputeUnitKind::ADC) &&
               computeUnitAvailable(ComputeUnitKind::BDC) &&
               computeUnitAvailable(ComputeUnitKind::CDC);
    }
    if (entry.isRelease) {
        return releaseReady() && !releaseTask.has_value();
    }

    switch (microTaskKindForEntry(entry)) {
      case MicroTaskKind::AML:
        return !amlTask.has_value();
      case MicroTaskKind::BML:
        return !bmlTask.has_value();
      case MicroTaskKind::CML:
        return !cmlTask.has_value();
      case MicroTaskKind::Compute:
        return true;
      case MicroTaskKind::Release:
        return !releaseTask.has_value();
      case MicroTaskKind::Count:
        return false;
    }
    return false;
}

bool
DetailedCuteBackend::headReady(const DecodedFifoEntry &entry,
                               DetailedCuteScoreboard::BlockReason &reason) const
{
    reason = scoreboard.blockReason(entry);
    if (reason != DetailedCuteScoreboard::BlockReason::None) {
        return false;
    }

    return issuePathReady(entry);
}

void
DetailedCuteBackend::issueHead(const DecodedFifoEntry &entry)
{
    scoreboard.onIssue(entry);
    if (entry.isStore) {
        ++pendingStoreCount;
    }

    dispatchTask(entry);
}

void
DetailedCuteBackend::dispatchTask(const DecodedFifoEntry &entry)
{
    const auto kind = microTaskKindForEntry(entry);
    TaskSlot task;
    task.entry = entry;
    if (entry.isZeroAcc) {
        task.zeroCyclesRemaining =
            timingConfig.matrixCZeroLoadLatencyCycles;
    }

    switch (kind) {
      case MicroTaskKind::AML:
        amlTask = task;
        break;
      case MicroTaskKind::BML:
        bmlTask = task;
        break;
      case MicroTaskKind::CML:
        cmlTask = task;
        break;
      case MicroTaskKind::Compute:
        computeTasks.emplace_back();
        computeTasks.back().entry = entry;
        break;
      case MicroTaskKind::Release:
        releaseTask = task;
        break;
      case MicroTaskKind::Count:
        assert(false && "dispatchTask called with unexpected compute/count task");
    }
}

bool
DetailedCuteBackend::computeUnitBusy(ComputeUnitKind kind) const
{
    if (kind == ComputeUnitKind::None || kind == ComputeUnitKind::Count) {
        return false;
    }

    for (const auto &task : computeTasks) {
        if (kind == ComputeUnitKind::ADC &&
            task.readIssued[ComputeReadAIdx] &&
            !task.readComplete[ComputeReadAIdx]) {
            return true;
        }
        if (kind == ComputeUnitKind::BDC &&
            task.readIssued[ComputeReadBIdx] &&
            !task.readComplete[ComputeReadBIdx]) {
            return true;
        }
        if (kind == ComputeUnitKind::CDC &&
            ((task.readIssued[ComputeReadCIdx] &&
              !task.readComplete[ComputeReadCIdx]) ||
             task.activeUnit == ComputeUnitKind::CDC)) {
            return true;
        }
        if (task.activeUnit == kind) {
            return true;
        }
    }
    return false;
}

bool
DetailedCuteBackend::computeUnitAvailable(ComputeUnitKind kind) const
{
    return !computeUnitBusy(kind);
}

} // namespace matrix
} // namespace gem5

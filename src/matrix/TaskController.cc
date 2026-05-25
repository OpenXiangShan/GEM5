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
#include <utility>

#include "base/trace.hh"
#include "debug/MatrixCuteTrace.hh"
#include "matrix/CUTETOP.hh"

namespace gem5
{

namespace matrix
{

// Active request issue path: fifo, headReady, issueHead, dispatchTask.
DetailedCuteBackend::DetailedCuteBackend(
    std::unique_ptr<MatrixMemoryAdapter> memory_adapter,
    size_t fifo_depth, size_t ab_reg_count, size_t c_reg_count)
    : DetailedCuteBackend(
          std::move(memory_adapter), fifo_depth, ab_reg_count, c_reg_count,
          TimingConfig())
{
}

DetailedCuteBackend::DetailedCuteBackend(
    std::unique_ptr<MatrixMemoryAdapter> memory_adapter,
    size_t fifo_depth, size_t ab_reg_count, size_t c_reg_count,
    TimingConfig timing_config)
    : fifo(fifo_depth), regFile(ab_reg_count, c_reg_count),
      memory(std::move(memory_adapter)),
      scoreboard(ab_reg_count, c_reg_count),
      timingConfig(timing_config),
      localMmu(LocalMmuModel::Config{
          timing_config.localMmuLatencyCycles,
          timing_config.localMmuMaxOutstanding})
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
    ++counters.fifoEnqueue;
    DPRINTF(MatrixCuteTrace,
            "fifo_enq [sn:%llu] kind=%u depth=%llu/%llu.\n",
            req.seq,
            static_cast<unsigned>(req.kind),
            static_cast<unsigned long long>(fifo.size() + 1),
            static_cast<unsigned long long>(fifo.depth()));
    fifo.enqueue(decodeCuteRequest(req));
}

bool
DetailedCuteBackend::canDownstreamAccept(const DecodedFifoEntry &entry) const
{
    if (entry.isLoad) {
        if (entry.request.lsu.isAcc) {
            return cmlReady();
        }
        return entry.request.lsu.isB ? bmlReady() : amlReady();
    }

    if (entry.isStore) {
        return cmlReady();
    }

    if (entry.isMma) {
        return computePathReady(entry);
    }

    if (entry.isRelease) {
        return !releaseTask.has_value();
    }

    if (entry.isZeroAcc) {
        return cmlReady();
    }

    if (entry.isZeroTr) {
        return amlReady();
    }

    return false;
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

bool
DetailedCuteBackend::microTaskAvailable(MicroTaskKind kind) const
{
    switch (kind) {
      case MicroTaskKind::AML:
        return !amlTask.has_value();
      case MicroTaskKind::BML:
        return !bmlTask.has_value();
      case MicroTaskKind::CML:
        return !cmlTask.has_value();
      case MicroTaskKind::Release:
        return !releaseTask.has_value();
      case MicroTaskKind::Compute:
        return true;
      case MicroTaskKind::Count:
        break;
    }

    return true;
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
DetailedCuteBackend::loadPathReady(const DecodedFifoEntry &entry) const
{
    return entry.isLoad &&
           microTaskAvailable(microTaskKindForEntry(entry)) &&
           canDownstreamAccept(entry);
}

bool
DetailedCuteBackend::storePathReady(const DecodedFifoEntry &entry) const
{
    return entry.isStore &&
           microTaskAvailable(MicroTaskKind::CML) &&
           canDownstreamAccept(entry);
}

bool
DetailedCuteBackend::computePathReady(const DecodedFifoEntry &entry) const
{
    return entry.isMma &&
           microTaskAvailable(MicroTaskKind::Compute) &&
           computeUnitAvailable(ComputeUnitKind::ADC) &&
           computeUnitAvailable(ComputeUnitKind::BDC) &&
           computeUnitAvailable(ComputeUnitKind::CDC);
}

bool
DetailedCuteBackend::arithPathReady(const DecodedFifoEntry &entry) const
{
    return (entry.isZeroAcc || entry.isZeroTr) &&
           microTaskAvailable(microTaskKindForEntry(entry)) &&
           canDownstreamAccept(entry);
}

bool
DetailedCuteBackend::releasePathReady(const DecodedFifoEntry &entry) const
{
    return entry.isRelease && releaseReady() &&
           microTaskAvailable(MicroTaskKind::Release) &&
           canDownstreamAccept(entry);
}

bool
DetailedCuteBackend::headReady(const DecodedFifoEntry &entry,
                               DetailedCuteScoreboard::BlockReason &reason) const
{
    reason = scoreboard.blockReason(entry);
    if (reason != DetailedCuteScoreboard::BlockReason::None) {
        return false;
    }

    if (entry.isLoad) {
        return loadPathReady(entry);
    } else if (entry.isStore) {
        return storePathReady(entry);
    } else if (entry.isMma) {
        return computePathReady(entry);
    } else if (entry.isZeroAcc || entry.isZeroTr) {
        return arithPathReady(entry);
    } else if (entry.isRelease) {
        return releasePathReady(entry);
    }

    return false;
}

void
DetailedCuteBackend::recordScoreboardBlock(
    DetailedCuteScoreboard::BlockReason reason)
{
    ++counters.scoreboardBlock;
    ++counters.scoreboardBlockReasons[static_cast<size_t>(reason)];
}

void
DetailedCuteBackend::recordFifoBlock(FifoBlockReason reason)
{
    ++counters.fifoBlock;
    ++counters.fifoBlockReasons[static_cast<size_t>(reason)];
}

void
DetailedCuteBackend::issueHead(const DecodedFifoEntry &entry)
{
    scoreboard.onIssue(entry);
    if (entry.isStore) {
        ++pendingStoreCount;
    }

    if (entry.writeValid[0]) {
        // ownership metadata no longer lives in MatrixRegFile
    }

    const auto kind = microTaskKindForEntry(entry);
    ++counters.microtaskIssue;
    ++counters.microtaskIssuesByKind[static_cast<size_t>(kind)];

    dispatchTask(entry);
}

void
DetailedCuteBackend::dispatchTask(const DecodedFifoEntry &entry)
{
    TaskSlot task;
    task.entry = entry;
    task.microTaskKind = microTaskKindForEntry(entry);
    task.issueStep = backendStep;

    DPRINTF(MatrixCuteTrace,
            "microtask_issue [sn:%llu] unit=%u stage=%u step=%llu.\n",
            entry.request.seq,
            static_cast<unsigned>(task.microTaskKind),
            static_cast<unsigned>(task.stage),
            static_cast<unsigned long long>(task.issueStep));

    switch (task.microTaskKind) {
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
        computeTasks.back().issueStep = backendStep;
        break;
      case MicroTaskKind::Release:
        releaseTask = task;
        break;
      case MicroTaskKind::Count:
        assert(false && "dispatchTask called with unexpected compute/count task");
    }
}

bool
DetailedCuteBackend::computeUnitBusyForTest(ComputeUnitKind kind) const
{
    if (kind == ComputeUnitKind::None || kind == ComputeUnitKind::Count) {
        return false;
    }

    for (const auto &task : computeTasks) {
        if (kind == ComputeUnitKind::ADC &&
            task.adcReadIssued && !task.adcReadComplete) {
            return true;
        }
        if (kind == ComputeUnitKind::BDC &&
            task.bdcReadIssued && !task.bdcReadComplete) {
            return true;
        }
        if (kind == ComputeUnitKind::CDC &&
            ((task.cdcReadIssued && !task.cdcReadComplete) ||
             task.activeUnit == ComputeUnitKind::CDC)) {
            return true;
        }
        if (task.activeUnit == kind) {
            return true;
        }
    }
    return false;
}

DetailedCuteBackend::ComputeUnitKind
DetailedCuteBackend::activeComputeUnitForTest() const
{
    if (computeTasks.empty()) {
        return ComputeUnitKind::None;
    }
    const auto &task = computeTasks.front();
    if (task.adcReadIssued && !task.adcReadComplete) {
        return ComputeUnitKind::ADC;
    }
    if (task.bdcReadIssued && !task.bdcReadComplete) {
        return ComputeUnitKind::BDC;
    }
    if (task.cdcReadIssued && !task.cdcReadComplete) {
        return ComputeUnitKind::CDC;
    }
    return computeTasks.front().activeUnit;
}

bool
DetailedCuteBackend::computeUnitAvailable(ComputeUnitKind kind) const
{
    return !computeUnitBusyForTest(kind);
}

} // namespace matrix
} // namespace gem5

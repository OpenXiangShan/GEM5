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

#include "matrix/Scoreboard.hh"

namespace gem5
{

namespace matrix
{

DetailedCuteScoreboard::BlockReason
DetailedCuteScoreboard::blockReason(const DecodedFifoEntry &entry) const
{
    if (entry.isRelease) {
        return BlockReason::None;
    }

    if (entry.isLoad) {
        if (destBusy(entry)) {
            return BlockReason::DestBusy;
        }
        if (destHasPendingReaders(entry)) {
            return BlockReason::DestPendingReaders;
        }
        return BlockReason::None;
    }

    if (entry.isStore) {
        if (!sourceReady(entry, 0)) {
            return BlockReason::SrcNotReady;
        }
        if (sourceHasPendingReaders(entry, 0)) {
            return BlockReason::SrcPendingReaders;
        }
        return BlockReason::None;
    }

    if (entry.isMma) {
        if (!sourceReady(entry, 0) ||
            !sourceReady(entry, 1) ||
            !sourceReady(entry, 2)) {
            return BlockReason::SrcNotReady;
        }
        if (destBusy(entry)) {
            return BlockReason::DestBusy;
        }
        if (destHasPendingReaders(entry)) {
            return BlockReason::DestPendingReaders;
        }
        return BlockReason::None;
    }

    if (entry.isZeroAcc) {
        if (destBusy(entry)) {
            return BlockReason::DestBusy;
        }
        if (destHasPendingReaders(entry)) {
            return BlockReason::DestPendingReaders;
        }
        return BlockReason::None;
    }

    return BlockReason::None;
}

void
DetailedCuteScoreboard::onLoadIssue(const DecodedFifoEntry &entry)
{
    reserveDest(entry, loadFu(entry));
}

void
DetailedCuteScoreboard::onLoadFinish(const DecodedFifoEntry &entry)
{
    releaseDest(entry, loadFu(entry));
}

void
DetailedCuteScoreboard::onStoreIssue(const DecodedFifoEntry &entry)
{
    incrementPendingReader(entry.readRegs[0], MatrixBankKind::C);
}

void
DetailedCuteScoreboard::onStoreReadFinish(const DecodedFifoEntry &entry)
{
    decrementPendingReader(entry.readRegs[0], MatrixBankKind::C);
}

void
DetailedCuteScoreboard::onComputeIssue(const DecodedFifoEntry &entry)
{
    reserveDest(entry, FuKind::Compute);

    incrementPendingReader(entry.readRegs[0], MatrixBankKind::A);
    incrementPendingReader(entry.readRegs[1], MatrixBankKind::B);
    incrementPendingReader(entry.readRegs[2], MatrixBankKind::C);
}

void
DetailedCuteScoreboard::onComputeReadFinishA(const DecodedFifoEntry &entry)
{
    onComputeReadFinish(entry, SrcAIdx, MatrixBankKind::A);
}

void
DetailedCuteScoreboard::onComputeReadFinishB(const DecodedFifoEntry &entry)
{
    onComputeReadFinish(entry, SrcBIdx, MatrixBankKind::B);
}

void
DetailedCuteScoreboard::onComputeReadFinishC(const DecodedFifoEntry &entry)
{
    onComputeReadFinish(entry, SrcCIdx, MatrixBankKind::C);
}

void
DetailedCuteScoreboard::onComputeWriteFinishC(const DecodedFifoEntry &entry)
{
    releaseDest(entry, FuKind::Compute);
}

void
DetailedCuteScoreboard::onArithIssue(const DecodedFifoEntry &entry)
{
    const auto fu_kind = entry.isZeroAcc ? FuKind::CML : FuKind::AML;
    reserveDest(entry, fu_kind);
}

void
DetailedCuteScoreboard::onArithFinish(const DecodedFifoEntry &entry)
{
    const auto fu_kind = entry.isZeroAcc ? FuKind::CML : FuKind::AML;
    releaseDest(entry, fu_kind);
}

void
DetailedCuteScoreboard::onIssue(const DecodedFifoEntry &entry)
{
    if (entry.isRelease) {
        return;
    }

    if (entry.isLoad) {
        onLoadIssue(entry);
        return;
    }

    if (entry.isStore) {
        onStoreIssue(entry);
        return;
    }

    if (entry.isMma) {
        onComputeIssue(entry);
        return;
    }

    if (entry.isZeroAcc) {
        onArithIssue(entry);
    }
}

DetailedCuteScoreboard::RegState &
DetailedCuteScoreboard::regStatus(uint8_t reg, MatrixBankKind bank)
{
    return bank == MatrixBankKind::C ? cRegs.at(reg) : abRegs.at(reg);
}

const DetailedCuteScoreboard::RegState &
DetailedCuteScoreboard::regStatus(uint8_t reg, MatrixBankKind bank) const
{
    return bank == MatrixBankKind::C ? cRegs.at(reg) : abRegs.at(reg);
}

MatrixBankKind
DetailedCuteScoreboard::bankForSource(const DecodedFifoEntry &entry,
                                      size_t src_idx) const
{
    if (entry.isStore) {
        return MatrixBankKind::C;
    }
    if (entry.isMma) {
        return src_idx == SrcAIdx ? MatrixBankKind::A :
               (src_idx == SrcBIdx ? MatrixBankKind::B : MatrixBankKind::C);
    }
    if (entry.isZeroAcc) {
        return MatrixBankKind::C;
    }
    return MatrixBankKind::A;
}

DetailedCuteScoreboard::FuKind
DetailedCuteScoreboard::loadFu(const DecodedFifoEntry &entry) const
{
    if (entry.request.lsu.isAcc) {
        return FuKind::CML;
    }
    return entry.request.lsu.isB ? FuKind::BML : FuKind::AML;
}

bool
DetailedCuteScoreboard::sourceReady(const DecodedFifoEntry &entry,
                                    size_t src_idx) const
{
    if (!entry.readValid[src_idx]) {
        return true;
    }
    return !regStatus(entry.readRegs[src_idx],
                      bankForSource(entry, src_idx)).busy;
}

bool
DetailedCuteScoreboard::destBusy(const DecodedFifoEntry &entry) const
{
    if (!entry.writeValid[0]) {
        return false;
    }
    return regStatus(entry.writeRegs[0], destBank(entry)).busy;
}

bool
DetailedCuteScoreboard::sourceHasPendingReaders(const DecodedFifoEntry &entry,
                                                size_t src_idx) const
{
    if (!entry.readValid[src_idx]) {
        return false;
    }
    return regStatus(entry.readRegs[src_idx],
                     bankForSource(entry, src_idx)).pendingReaders != 0;
}

bool
DetailedCuteScoreboard::destHasPendingReaders(const DecodedFifoEntry &entry) const
{
    if (!entry.writeValid[0]) {
        return false;
    }
    return regStatus(entry.writeRegs[0], destBank(entry)).pendingReaders != 0;
}

void
DetailedCuteScoreboard::onComputeReadFinish(
    const DecodedFifoEntry &entry, size_t src_idx, MatrixBankKind bank)
{
    decrementPendingReader(entry.readRegs[src_idx], bank);
}

void
DetailedCuteScoreboard::reserveDest(const DecodedFifoEntry &entry, FuKind writer)
{
    if (!entry.writeValid[0]) {
        return;
    }
    auto &status = regStatus(entry.writeRegs[0], destBank(entry));
    status.busy = true;
    status.writer = writer;
}

void
DetailedCuteScoreboard::releaseDest(const DecodedFifoEntry &entry, FuKind writer)
{
    if (!entry.writeValid[0]) {
        return;
    }

    auto &status = regStatus(entry.writeRegs[0], destBank(entry));
    if (status.busy && status.writer == writer) {
        status.busy = false;
        status.writer = FuKind::None;
    }
}

void
DetailedCuteScoreboard::incrementPendingReader(uint8_t reg, MatrixBankKind bank)
{
    ++regStatus(reg, bank).pendingReaders;
}

void
DetailedCuteScoreboard::decrementPendingReader(uint8_t reg, MatrixBankKind bank)
{
    auto &status = regStatus(reg, bank);
    if (status.pendingReaders > 0) {
        --status.pendingReaders;
    }
}

} // namespace matrix
} // namespace gem5

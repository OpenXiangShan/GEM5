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

bool
DetailedCuteScoreboard::canIssue(const DecodedFifoEntry &entry) const
{
    return blockReason(entry) == BlockReason::None;
}

DetailedCuteScoreboard::BlockReason
DetailedCuteScoreboard::blockReason(const DecodedFifoEntry &entry) const
{
    if (entry.isRelease) {
        return BlockReason::None;
    }

    if (entry.isLoad) {
        const auto fu = loadFu(entry);
        if (isFuBusy(fu)) {
            return BlockReason::FuBusy;
        }
        if (destBusy(entry)) {
            return BlockReason::DestBusy;
        }
        if (destHasPendingReaders(entry)) {
            return BlockReason::DestPendingReaders;
        }
        return BlockReason::None;
    }

    if (entry.isStore) {
        if (isFuBusy(FuKind::CML)) {
            return BlockReason::FuBusy;
        }
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

    if (entry.isZeroAcc || entry.isZeroTr) {
        const auto fu = entry.isZeroAcc ? FuKind::CML : FuKind::AML;
        if (isFuBusy(fu)) {
            return BlockReason::FuBusy;
        }
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
    auto &fu = fuState(loadFu(entry));
    resetFu(fu);
    fu.busy = true;
    fu.destValid = entry.writeValid[0];
    fu.destBank = destBank(entry);
    fu.destReg = entry.writeRegs[0];
    reserveDest(entry, loadFu(entry));
}

void
DetailedCuteScoreboard::onLoadFinish(const DecodedFifoEntry &entry)
{
    releaseDest(entry, loadFu(entry));
    resetFu(fuState(loadFu(entry)));
}

void
DetailedCuteScoreboard::onStoreIssue(const DecodedFifoEntry &entry)
{
    auto &fu = fuState(FuKind::CML);
    resetFu(fu);
    fu.busy = true;
    setupSrc(fu.srcs[SrcCIdx], MatrixBankKind::C, entry.readRegs[0]);
    incrementPendingReader(entry.readRegs[0], MatrixBankKind::C);
}

void
DetailedCuteScoreboard::onStoreReadFinish(const DecodedFifoEntry &entry)
{
    decrementPendingReader(entry.readRegs[0], MatrixBankKind::C);
    auto &src = fuState(FuKind::CML).srcs[SrcCIdx];
    src.readPending = false;
}

void
DetailedCuteScoreboard::onStoreWriteFinish(const DecodedFifoEntry &entry)
{
    if (entry.readValid[0]) {
        auto &src = fuState(FuKind::CML).srcs[SrcCIdx];
        src.readPending = false;
    }
    resetFu(fuState(FuKind::CML));
}

void
DetailedCuteScoreboard::onComputeIssue(const DecodedFifoEntry &entry)
{
    auto &fu = fuState(FuKind::Compute);
    resetFu(fu);
    fu.destValid = entry.writeValid[0];
    fu.destBank = MatrixBankKind::C;
    fu.destReg = entry.writeRegs[0];

    setupSrc(fu.srcs[SrcAIdx], MatrixBankKind::A, entry.readRegs[0]);
    setupSrc(fu.srcs[SrcBIdx], MatrixBankKind::B, entry.readRegs[1]);
    setupSrc(fu.srcs[SrcCIdx], MatrixBankKind::C, entry.readRegs[2]);

    incrementPendingReader(entry.readRegs[0], MatrixBankKind::A);
    incrementPendingReader(entry.readRegs[1], MatrixBankKind::B);
    incrementPendingReader(entry.readRegs[2], MatrixBankKind::C);
    reserveDest(entry, FuKind::Compute);
}

void
DetailedCuteScoreboard::onComputeReadFinishA(const DecodedFifoEntry &entry)
{
    decrementPendingReader(entry.readRegs[0], MatrixBankKind::A);
    auto &src = fuState(FuKind::Compute).srcs[SrcAIdx];
    src.readPending = false;
}

void
DetailedCuteScoreboard::onComputeReadFinishB(const DecodedFifoEntry &entry)
{
    decrementPendingReader(entry.readRegs[1], MatrixBankKind::B);
    auto &src = fuState(FuKind::Compute).srcs[SrcBIdx];
    src.readPending = false;
}

void
DetailedCuteScoreboard::onComputeWriteFinishC(const DecodedFifoEntry &entry)
{
    decrementPendingReader(entry.readRegs[2], MatrixBankKind::C);
    auto &src = fuState(FuKind::Compute).srcs[SrcCIdx];
    src.readPending = false;
    releaseDest(entry, FuKind::Compute);
    resetFu(fuState(FuKind::Compute));
}

void
DetailedCuteScoreboard::onArithIssue(const DecodedFifoEntry &entry)
{
    const auto fu_kind = entry.isZeroAcc ? FuKind::CML : FuKind::AML;
    auto &fu = fuState(fu_kind);
    resetFu(fu);
    fu.busy = true;
    fu.destValid = entry.writeValid[0];
    fu.destBank = destBank(entry);
    fu.destReg = entry.writeRegs[0];
    reserveDest(entry, fu_kind);
}

void
DetailedCuteScoreboard::onArithFinish(const DecodedFifoEntry &entry)
{
    const auto fu_kind = entry.isZeroAcc ? FuKind::CML : FuKind::AML;
    releaseDest(entry, fu_kind);
    resetFu(fuState(fu_kind));
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

    if (entry.isZeroAcc || entry.isZeroTr) {
        onArithIssue(entry);
    }
}

bool
DetailedCuteScoreboard::fuBusyForTest(FuKind fu) const
{
    return isFuBusy(fu);
}

bool
DetailedCuteScoreboard::regBusyForTest(uint8_t reg, MatrixBankKind bank) const
{
    return regStatus(reg, bank).busy;
}

unsigned
DetailedCuteScoreboard::pendingReadersForTest(uint8_t reg,
                                              MatrixBankKind bank) const
{
    return regStatus(reg, bank).pendingReaders;
}

DetailedCuteScoreboard::FuState &
DetailedCuteScoreboard::fuState(FuKind fu)
{
    return fuStates[static_cast<size_t>(fu)];
}

const DetailedCuteScoreboard::FuState &
DetailedCuteScoreboard::fuState(FuKind fu) const
{
    return fuStates[static_cast<size_t>(fu)];
}

bool
DetailedCuteScoreboard::isFuBusy(FuKind fu) const
{
    return fu != FuKind::None && fuState(fu).busy;
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

MatrixBankKind
DetailedCuteScoreboard::destBank(const DecodedFifoEntry &entry) const
{
    if (entry.isMma || entry.isZeroAcc) {
        return MatrixBankKind::C;
    }
    if (entry.isZeroTr) {
        return MatrixBankKind::A;
    }
    if (entry.isLoad) {
        if (entry.request.lsu.isAcc) {
            return MatrixBankKind::C;
        }
        return entry.request.lsu.isB ? MatrixBankKind::B :
                                       MatrixBankKind::A;
    }
    return MatrixBankKind::C;
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
DetailedCuteScoreboard::resetSrc(SrcState &src)
{
    src = {};
}

void
DetailedCuteScoreboard::resetFu(FuState &fu)
{
    fu.busy = false;
    fu.destValid = false;
    fu.destBank = MatrixBankKind::A;
    fu.destReg = 0;
    for (auto &src : fu.srcs) {
        resetSrc(src);
    }
}

void
DetailedCuteScoreboard::setupSrc(SrcState &src, MatrixBankKind bank, uint8_t reg)
{
    src.valid = true;
    src.bank = bank;
    src.reg = reg;

    const auto &status = regStatus(reg, bank);
    if (status.busy) {
        src.ready = false;
        src.waitFu = status.writer;
    } else {
        src.ready = true;
        src.waitFu = FuKind::None;
    }
    src.readPending = true;
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
        wakeupConsumers(entry.writeRegs[0], destBank(entry), writer);
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

void
DetailedCuteScoreboard::wakeupConsumers(uint8_t reg, MatrixBankKind bank,
                                        FuKind producer)
{
    for (size_t fu_idx = 0; fu_idx < static_cast<size_t>(FuKind::Count);
         ++fu_idx) {
        auto &fu = fuStates[fu_idx];
        if (!fu.busy) {
            continue;
        }
        for (auto &src : fu.srcs) {
            if (!src.valid) {
                continue;
            }
            if (src.bank == bank && src.reg == reg &&
                src.waitFu == producer) {
                src.ready = true;
                src.waitFu = FuKind::None;
            }
        }
    }
}

} // namespace matrix
} // namespace gem5

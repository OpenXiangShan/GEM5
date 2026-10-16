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

#include "cpu/o3/matrix_amu_buffer.hh"

#include "base/logging.hh"

namespace gem5
{

namespace o3
{

MatrixAmuBuffer::MatrixAmuBuffer(unsigned capacity, unsigned fire_width)
    : capacity_(capacity), fireWidth_(fire_width ? fire_width : 1)
{
}

void
MatrixAmuBuffer::reset(unsigned capacity, unsigned fire_width)
{
    capacity_ = capacity;
    fireWidth_ = fire_width ? fire_width : 1;
    entries_.clear();
}

MatrixAmuBuffer::Entry *
MatrixAmuBuffer::find(InstSeqNum seq_num)
{
    for (auto &entry : entries_) {
        if (entry.seqNum == seq_num) {
            return &entry;
        }
    }

    return nullptr;
}

const MatrixAmuBuffer::Entry *
MatrixAmuBuffer::find(InstSeqNum seq_num) const
{
    for (const auto &entry : entries_) {
        if (entry.seqNum == seq_num) {
            return &entry;
        }
    }

    return nullptr;
}

void
MatrixAmuBuffer::cleanupFront()
{
    while (!entries_.empty() && entries_.front().canDeq) {
        entries_.pop_front();
    }
}

std::list<MatrixAmuBuffer::Entry>::iterator
MatrixAmuBuffer::findReadyToFire()
{
    auto it = entries_.begin();
    unsigned scanned = 0;
    while (it != entries_.end() && scanned < fireWidth_) {
        if (it->amuReqValid()) {
            return it;
        }
        ++it;
        ++scanned;
    }

    return entries_.end();
}

void
MatrixAmuBuffer::allocate(InstSeqNum seq_num)
{
    panic_if(capacity_ == 0,
             "Matrix AMU buffer capacity is zero at alloc [sn:%llu]",
             seq_num);
    panic_if(entries_.size() >= capacity_,
             "Matrix AMU buffer overflow at alloc [sn:%llu] used=%llu "
             "capacity=%u",
             seq_num, static_cast<unsigned long long>(entries_.size()),
             capacity_);
    entries_.push_back(Entry{});
    auto &entry = entries_.back();
    entry.needAMU = true;
    entry.seqNum = seq_num;
}

void
MatrixAmuBuffer::noteWriteback(InstSeqNum seq_num, bool faulted,
                               bool req_valid,
                               const matrix::CuteRequest &backend_req)
{
    auto *entry = find(seq_num);
    panic_if(!entry, "Matrix AMU entry missing at writeback [sn:%llu]",
             seq_num);

    if (!entry->needAMU) {
        entry->writebacked = true;
        return;
    }

    if (faulted || !req_valid) {
        entry->needAMU = false;
        entry->writebacked = true;
        return;
    }

    entry->backendReq = backend_req;
    entry->writebacked = true;
}

void
MatrixAmuBuffer::noteCommit(InstSeqNum seq_num)
{
    auto *entry = find(seq_num);
    panic_if(!entry, "Matrix AMU entry missing at commit [sn:%llu]",
             seq_num);
    entry->committed = true;
    if (!entry->needAMU) {
        entry->canDeq = true;
    }
    cleanupFront();
}

bool
MatrixAmuBuffer::peekReady(Entry &entry_out)
{
    cleanupFront();

    if (entries_.empty()) {
        return false;
    }

    auto ready_it = findReadyToFire();
    if (ready_it == entries_.end()) {
        return false;
    }

    entry_out = *ready_it;
    return true;
}

bool
MatrixAmuBuffer::popReady(Entry &entry_out)
{
    if (!peekReady(entry_out)) {
        return false;
    }

    auto ready_it = findReadyToFire();
    panic_if(ready_it == entries_.end(),
             "Matrix AMU ready entry disappeared before pop [sn:%llu]",
             entry_out.seqNum);

    auto &entry = *ready_it;
    entry.canDeq = true;
    cleanupFront();
    return true;
}

void
MatrixAmuBuffer::squash(InstSeqNum seq_num)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->seqNum == seq_num) {
            entries_.erase(it);
            cleanupFront();
            return;
        }
    }
}

unsigned
MatrixAmuBuffer::numFreeEntries()
{
    cleanupFront();
    if (capacity_ <= entries_.size()) {
        return 0;
    }
    return capacity_ - entries_.size();
}

} // namespace o3
} // namespace gem5

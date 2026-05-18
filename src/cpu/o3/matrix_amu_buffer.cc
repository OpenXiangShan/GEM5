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
#include "debug/ROB.hh"

namespace gem5
{

namespace o3
{

namespace
{

const char *
requestKindName(matrix::CuteRequestKind kind)
{
    switch (kind) {
      case matrix::CuteRequestKind::Lsu:
        return "lsu";
      case matrix::CuteRequestKind::Mma:
        return "mma";
      case matrix::CuteRequestKind::Arith:
        return "arith";
      case matrix::CuteRequestKind::Release:
        return "release";
    }

    return "unknown";
}

} // anonymous namespace

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
MatrixAmuBuffer::cleanupFront(ThreadID tid)
{
    while (!entries_.empty() && entries_.front().canDeq) {
        DPRINTF(ROB,
                "[tid:%i] Matrix AMU entry cleanup [sn:%llu] needAMU=%d "
                "writebacked=%d committed=%d.\n",
                tid, entries_.front().seqNum, entries_.front().needAMU,
                entries_.front().writebacked, entries_.front().committed);
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
MatrixAmuBuffer::allocate(ThreadID tid, InstSeqNum seq_num, bool need_amu,
                          const char *class_name, const char *route_name)
{
    panic_if(capacity_ == 0,
             "[tid:%i] Matrix AMU buffer capacity is zero at alloc [sn:%llu]",
             tid, seq_num);
    panic_if(entries_.size() >= capacity_,
             "[tid:%i] Matrix AMU buffer overflow at alloc [sn:%llu] used=%llu "
             "capacity=%u",
             tid, seq_num, static_cast<unsigned long long>(entries_.size()),
             capacity_);
    entries_.push_back(Entry{});
    auto &entry = entries_.back();
    entry.valid = true;
    entry.needAMU = need_amu;
    entry.seqNum = seq_num;
    DPRINTF(ROB,
            "[tid:%i] Matrix AMU entry alloc [sn:%llu] class=%s route=%s.\n",
            tid, seq_num, class_name, route_name);
}

void
MatrixAmuBuffer::noteWriteback(ThreadID tid, InstSeqNum seq_num, bool faulted,
                               bool req_valid,
                               const matrix::CuteRequest &backend_req,
                               const char *payload_kind_name)
{
    auto *entry = find(seq_num);
    panic_if(!entry, "[tid:%i] Matrix AMU entry missing at writeback [sn:%llu]",
             tid, seq_num);

    if (!entry->needAMU) {
        entry->writebacked = true;
        return;
    }

    if (faulted || !req_valid) {
        entry->needAMU = false;
        entry->writebacked = true;
        DPRINTF(ROB,
                "[tid:%i] Matrix AMU entry writeback suppressed [sn:%llu] "
                "fault=%d payloadValid=%d.\n",
                tid, seq_num, faulted, req_valid);
        return;
    }

    entry->backendReq = backend_req;
    entry->backendReqValid = true;
    entry->writebacked = true;
    DPRINTF(ROB,
            "[tid:%i] Matrix AMU entry writeback [sn:%llu] payload=%s.\n",
            tid, seq_num, payload_kind_name);
}

void
MatrixAmuBuffer::noteCommit(ThreadID tid, InstSeqNum seq_num)
{
    auto *entry = find(seq_num);
    panic_if(!entry, "[tid:%i] Matrix AMU entry missing at commit [sn:%llu]",
             tid, seq_num);
    entry->committed = true;
    if (!entry->needAMU) {
        entry->canDeq = true;
    }
    DPRINTF(ROB,
            "[tid:%i] Matrix AMU entry committed [sn:%llu] writebacked=%d "
            "needAMU=%d.\n",
            tid, seq_num, entry->writebacked, entry->needAMU);
    cleanupFront(tid);
}

bool
MatrixAmuBuffer::peekReady(ThreadID tid, Entry &entry_out)
{
    cleanupFront(tid);

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
MatrixAmuBuffer::popReady(ThreadID tid, Entry &entry_out)
{
    if (!peekReady(tid, entry_out)) {
        return false;
    }

    auto ready_it = findReadyToFire();
    panic_if(ready_it == entries_.end(),
             "[tid:%i] Matrix AMU ready entry disappeared before pop [sn:%llu]",
             tid, entry_out.seqNum);

    auto &entry = *ready_it;
    entry.canDeq = true;
    DPRINTF(ROB,
            "[tid:%i] Matrix AMU entry toAMU proxy ready [sn:%llu] kind=%s.\n",
            tid, entry.seqNum, requestKindName(entry.backendReq.kind));
    cleanupFront(tid);
    return true;
}

void
MatrixAmuBuffer::squash(ThreadID tid, InstSeqNum seq_num)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->seqNum == seq_num) {
            DPRINTF(ROB,
                    "[tid:%i] Matrix AMU entry squash [sn:%llu] "
                    "writebacked=%d committed=%d.\n",
                    tid, seq_num, it->writebacked, it->committed);
            entries_.erase(it);
            cleanupFront(tid);
            return;
        }
    }
}

unsigned
MatrixAmuBuffer::numFreeEntries(ThreadID tid)
{
    cleanupFront(tid);
    if (capacity_ <= entries_.size()) {
        return 0;
    }
    return capacity_ - entries_.size();
}

void
MatrixAmuBuffer::clear(ThreadID tid)
{
    entries_.clear();
}

} // namespace o3
} // namespace gem5

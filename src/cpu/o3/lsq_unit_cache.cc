/*
 * Copyright (c) 2010-2014, 2017-2021 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include <algorithm>
#include <cassert>

#include "arch/generic/debugfaults.hh"
#include "arch/riscv/faults.hh"
#include "base/logging.hh"
#include "base/str.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/golden_global_mem.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/replay_events.hh"
#include "cpu/o3/trace/TraceInstruction.hh"
#include "cpu/utils.hh"
#include "debug/Activity.hh"
#include "debug/Diff.hh"
#include "debug/Hint.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
#include "debug/StoreBuffer.hh"
#include "debug/StorePipeline.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace o3
{



bool
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt, bool &bank_conflict, bool &tag_read_fail,
                        bool &mshr_used, bool &mshr_alias_fail, bool &hit_in_write_buffer)
{
    bool ret = true;
    bool cache_got_blocked = false;
    LSQRequest *request = dynamic_cast<LSQRequest *>(data_pkt->senderState);
    if (isLoad) {
        bank_conflict = lsq->loadBankConflictedCheck(
            data_pkt->req->getVaddr(), data_pkt->req->getSize());
    }
    // Record the tick count at the time of sending to let
    // the subsequent cache understand the request's sending time.
    data_pkt->sendTick = curTick();
    PacketPtr pkt = data_pkt;

    auto inst = dynamic_cast<LSQRequest *>(data_pkt->senderState)->instruction();

    DPRINTF(LSQUnit, "Attempting to send packet for inst [sn:%llu], addr: %#x\n",
            inst->seqNum, data_pkt->getAddr());

    if (isLoad) {
        const int fwd_base =
            pkt->req->getVaddr() - request->mainReq()->getVaddr();
        const int fwd_end = fwd_base + pkt->req->getSize();
        assert(fwd_base >= 0);
        for (auto it = request->SBforwardPackets.begin();
             it != request->SBforwardPackets.end();) {
            if (it->idx >= fwd_base && it->idx < fwd_end) {
                it = request->SBforwardPackets.erase(it);
            } else {
                ++it;
            }
        }

        const Addr block_addr = pkt->getAddr() & cacheBlockMask;
        auto entry = lsq->findForwardingStoreBufferEntry(
            block_addr, lsqID);
        if (entry) {
            DPRINTF(StoreBuffer, "sbuffer entry[%#x] coverage %s\n",
                    entry->blockPaddr, pkt->print());
            if (entry->recordForward(
                    pkt->req, request, lsqID)) {
                assert(request->isSplit()); // here must be split request
                stats.sbufferFullForward++;
            } else if (!request->SBforwardPackets.empty()) {
                stats.sbufferPartiForward++;
            }
        }
    }

    if (!lsq->cacheBlocked() && lsq->cachePortAvailable(isLoad)) {
        if (bank_conflict) {
            ++stats.bankConflictTimes;
            if (!isLoad) {
                assert(request == storeWBIt->request());
                isStoreBlocked = true;
            }
            bank_conflict = true;
            ret = false;
        }
        if (!bank_conflict && !dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            mshr_used = data_pkt->mshrArbFailed();
            mshr_alias_fail = data_pkt->mshrAliasFailed();
            hit_in_write_buffer = data_pkt->isHitInWriteBuffer();
            tag_read_fail = data_pkt->tagReadFail;

            if (!tag_read_fail && !mshr_used && !mshr_alias_fail && !hit_in_write_buffer) {
                cache_got_blocked = true;
            }
        }
    }
    else {
        ret = false;
    }

    if (ret) {
        if (!isLoad) {
            isStoreBlocked = false;
        }
        lsq->cachePortBusy(isLoad);
        request->packetSent();
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        if (!isLoad) {
            assert(request == storeWBIt->request());
            isStoreBlocked = true;
        }
        request->packetNotSent();
    }
    DPRINTF(LSQUnit,
            "Memory request (pkt: %s) from inst [sn:%llu] was"
            " %ssent (cache is blocked: %d, cache_got_blocked: %d, bank conflict: %d, tag_read_fail: %d,"
            " mshr_used: %d, mshr_alias_fail: %d, hit_in_write_buffer: %d)\n",
            data_pkt->print(), request->instruction()->seqNum, ret ? "" : "not ", lsq->cacheBlocked(),
            cache_got_blocked, bank_conflict, tag_read_fail, mshr_used, mshr_alias_fail, hit_in_write_buffer);
    return ret;
}

void
LSQUnit::startStaleTranslationFlush()
{
    DPRINTF(LSQUnit, "Unit %p marking stale translations %d %d\n", this,
        storeQueue.size(), loadQueue.size());
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
}

bool
LSQUnit::checkStaleTranslations() const
{
    DPRINTF(LSQUnit, "Unit %p checking stale translations\n", this);
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    DPRINTF(LSQUnit, "Unit %p found no stale translations\n", this);
    return false;
}

void
LSQUnit::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

} // namespace o3
} // namespace gem5

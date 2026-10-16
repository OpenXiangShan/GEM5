/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020 ARM Limited
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
 * Copyright (c) 2011 Regents of the University of California
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


#include <cassert>
#include <cstring>

#include "cpu/o3/cpu.hh"
#include "cpu/o3/matrix_cpu_state.hh"
#include "matrix/CUTETOP.hh"
#include "matrix/MemoryLoader.hh"

namespace gem5
{

namespace o3
{

namespace
{

std::vector<bool>
byteEnableFromMask(uint64_t byte_mask, uint32_t packet_size)
{
    std::vector<bool> byte_enable(packet_size, byte_mask == 0);
    for (uint32_t byte = 0; byte < packet_size && byte < 64; ++byte) {
        if ((byte_mask & (uint64_t(1) << byte)) != 0) {
            byte_enable[byte] = true;
        }
    }
    return byte_enable;
}

} // anonymous namespace

MatrixMemPort::MatrixMemPort(const std::string &name, CPU *cpu_,
                                bool acquire)
    : RequestPort(name), stats(cpu_), cpu(cpu_), rmwAcquire(acquire)
{
}

MatrixMemPort::PortStats::PortStats(statistics::Group *parent)
    : statistics::Group(parent, "matrixMemoryPort"),
      ADD_STAT(sourcesAccepted, statistics::units::Count::get(),
               "Backend sources accepted by the timing adapter"),
      ADD_STAT(sourcesCompleted, statistics::units::Count::get(),
               "Sources completed at final read data or store acknowledgement"),
      ADD_STAT(sourceTicks, statistics::units::Tick::get(),
               "Total source lifetime including preparation and retry"),
      ADD_STAT(sourceTicksMax, statistics::units::Tick::get(),
               "Maximum source lifetime"),
      ADD_STAT(sourcesMax, statistics::units::Count::get(),
               "Maximum sources awaiting final response")
{
}

void
MatrixMemPort::noteSourceCompletion(uint32_t source)
{
    const auto it = sourceStarted.find(source);
    panic_if(it == sourceStarted.end(), "Missing Matrix source lifetime");
    const Tick lifetime = curTick() - it->second;
    sourceStarted.erase(it);
    stats.sourcesCompleted++;
    stats.sourceTicks += lifetime;
    stats.sourceTicksMax = std::max<double>(stats.sourceTicksMax.value(), lifetime);
}

PacketPtr
MatrixMemPort::buildPacket(const Request &request, uint64_t transaction_id,
                           bool continuation)
{
    const bool prepare = request.isStore && !rmwAcquire && !continuation;
    gem5::Request::Flags flags = gem5::Request::PHYSICAL;
    if (prepare) {
        flags.set(gem5::Request::CLEAN | gem5::Request::INVALIDATE |
                  gem5::Request::DST_POU);
    }
    auto req = std::make_shared<gem5::Request>(
        request.paddr, request.packetSize, flags, cpu->dataRequestorId());
    req->taskId(cpu->taskId());
    if (request.contextId != InvalidContextID) {
        req->setContext(request.contextId);
    }
    if (request.matrixKey || transaction_id) {
        gem5::Request::XsMetadata metadata;
        metadata.validXsMetadata = true;
        metadata.matrixKey = request.matrixKey & 0x3;
        if (transaction_id) {
            metadata.matrixTxnKind = request.isStore ?
                gem5::Request::XsMetadata::MatrixTxnKind::Store :
                gem5::Request::XsMetadata::MatrixTxnKind::CRead;
            metadata.matrixTxnPhase = prepare ?
                gem5::Request::XsMetadata::MatrixTxnPhase::Prepare :
                gem5::Request::XsMetadata::MatrixTxnPhase::Access;
            metadata.matrixTxnId = transaction_id;
            metadata.matrixRmwAcquire = !request.isStore;
            metadata.matrixTryDirect = !prepare && !continuation;
        }
        req->setXsMetadata(metadata);
    }
    if (request.isStore && !prepare) {
        assert(request.packetSize <= request.data.size());
        req->setByteEnable(
            byteEnableFromMask(request.byteMask, request.packetSize));
    }
    auto *pkt = new Packet(req, prepare ? MemCmd::CleanInvalidReq :
        (request.isStore ? MemCmd::WriteReq : MemCmd::ReadReq));
    if (!prepare) {
        auto *data = new uint8_t[request.packetSize];
        if (request.isStore) {
            std::memcpy(data, request.data.data(), request.packetSize);
        } else {
            std::memset(data, 0, request.packetSize);
        }
        pkt->dataDynamic(data);
    }
    pkt->senderState = new SenderState(request, transaction_id, continuation);
    return pkt;
}

void
MatrixMemPort::sendOrBlock(PacketPtr pkt)
{
    const auto *state = static_cast<SenderState *>(pkt->senderState);
    if (state->continuation) {
        blockedPackets.push_front(pkt);
    } else {
        blockedPackets.push_back(pkt);
    }
    if (!waitingForRetry) {
        trySendBlocked();
    }
}

bool
MatrixMemPort::sendTimingRequest(const Request &request)
{
    panic_if(!sourceStarted.emplace(request.sourceId, curTick()).second,
             "Matrix source reused before final response");
    stats.sourcesAccepted++;
    stats.sourcesMax = std::max<double>(stats.sourcesMax.value(), sourceStarted.size());
    uint64_t transaction_id = 0;
    if (request.isStore || (rmwAcquire && (request.matrixKey & 0x2))) {
        panic_if(!++nextTransactionId, "Matrix transaction ID wrapped");
        transaction_id = nextTransactionId;
    }
    sendOrBlock(buildPacket(request, transaction_id));
    return true;
}

bool
MatrixMemPort::recvTimingResp(PacketPtr pkt)
{
    auto *state = dynamic_cast<SenderState *>(pkt->senderState);
    panic_if(state == nullptr,
             "Matrix memory response missing sender state");

    const uint32_t source_id = state->sourceId;
    if (pkt->cmd == MemCmd::CleanInvalidResp) {
        panic_if(!state->transactionId || state->continuation,
                 "Unexpected Matrix preparation response");
        const Request request = state->originalRequest();
        const uint64_t transaction_id = state->transactionId;
        delete pkt->senderState;
        pkt->senderState = nullptr;
        delete pkt;

        sendOrBlock(buildPacket(request, transaction_id, true));
        cpu->activityRec.activity();
        cpu->scheduleTickEvent(Cycles(0));
        return true;
    }

    panic_if(pkt->cmd != (state->isStore ? MemCmd::WriteResp : MemCmd::ReadResp),
             "Matrix memory received unexpected response command %s",
             pkt->cmd.toString());
    const bool has_data = pkt->hasData() && pkt->getSize() != 0;
    const uint32_t data_size = has_data ? pkt->getSize() : 0;
    std::vector<uint8_t> data;
    if (has_data) {
        data.resize(data_size);
        std::memcpy(data.data(), pkt->getPtr<uint8_t>(), data.size());
    }

    delete pkt->senderState;
    pkt->senderState = nullptr;
    delete pkt;

    noteSourceCompletion(source_id);

    const bool completed =
        cpu->matrixState->backend &&
        cpu->matrixState->backend->completeTimingMemoryResponse(
            source_id, data.empty() ? nullptr : data.data(), data.size());
    panic_if(!completed,
             "Matrix memory response for unknown source %u", source_id);

    cpu->activityRec.activity();
    cpu->scheduleTickEvent(Cycles(0));
    return true;
}

void
MatrixMemPort::trySendBlocked()
{
    while (!blockedPackets.empty()) {
        PacketPtr pkt = blockedPackets.front();
        if (!sendTimingReq(pkt)) {
            waitingForRetry = true;
            return;
        }
        blockedPackets.pop_front();
    }
}

void
MatrixMemPort::recvReqRetry()
{
    panic_if(!waitingForRetry, "Unexpected Matrix request retry");
    waitingForRetry = false;
    trySendBlocked();
    cpu->activityRec.activity();
    cpu->scheduleTickEvent(Cycles(0));
}

} // namespace o3
} // namespace gem5

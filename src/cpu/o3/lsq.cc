/*
 * Copyright (c) 2011-2012, 2014, 2017-2019, 2021 ARM Limited
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
 * Copyright (c) 2005-2006 The Regents of The University of Michigan
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

#include "cpu/o3/lsq.hh"

#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <utility>

#include "arch/riscv/insts/fusion.hh"
#include "arch/riscv/insts/vector.hh"
#include "base/compiler.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/iew.hh"
#include "cpu/o3/limits.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/Hint.hh"
#include "debug/HtmCpu.hh"
#include "debug/LSQ.hh"
#include "debug/PacketSender.hh"
#include "debug/Schedule.hh"
#include "debug/StoreBuffer.hh"
#include "debug/TagReadFail.hh"
#include "debug/Writeback.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

LSQ::DcachePort::DcachePort(LSQ *_lsq, CPU *_cpu) :
    RequestPort(_cpu->name() + ".dcache_port", _cpu), lsq(_lsq), cpu(_cpu)
{}

std::list<LSQ::SingleDataRequest*> LSQ::SingleDataRequest::singleList;

void
LSQ::StoreBufferEntry::reset(ThreadID tid, InstSeqNum seq_num,
                             uint64_t block_vaddr, uint64_t block_paddr,
                             uint64_t offset, uint8_t *datas, uint64_t size,
                             const std::vector<bool> &mask)
{
    std::fill(validMask.begin(), validMask.begin() + offset, false);

    for (int i = 0; i < size; i++) {
        validMask[offset + i] = mask[i];
    }

    std::fill(validMask.begin() + offset + size, validMask.end(), false);
    memcpy(blockDatas.data() + offset, datas, size);

    this->tid = tid;
    this->seqNum = seq_num;
    this->blockVaddr = block_vaddr;
    this->blockPaddr = block_paddr;
    this->sending = false;
    this->inDcacheMainPipe = false;
    this->replayQueued = false;
    this->request = nullptr;
    this->vice = nullptr;
}

void
LSQ::StoreBufferEntry::merge(uint64_t offset, uint8_t *datas, uint64_t size,
                             const std::vector<bool> &mask)
{
    assert(offset + size <= validMask.size());
    for (uint64_t i = 0; i < size; ++i) {
        if (mask[i]) {
            blockDatas[offset + i] = datas[i];
            validMask[offset + i] = true;
        }
    }
}

bool
LSQ::StoreBufferEntry::recordForward(RequestPtr req, LSQRequest *lsqreq,
                                     ThreadID load_tid, InstSeqNum load_seq)
{
    int offset = req->getPaddr() & (validMask.size() - 1);
    // the offset in the split request
    int goffset = req->getVaddr() - lsqreq->mainReq()->getVaddr();
    if (goffset > 0) {
        assert(offset == 0);
    }
    bool full_forward = true;
    auto byteEligible = [&](StoreBufferEntry *entry, int byte_idx) {
        return entry && entry->tid == load_tid && entry->seqNum < load_seq &&
               entry->validMask[byte_idx];
    };
    for (int i = 0; i < req->getSize(); i++) {
        assert(goffset + i < lsqreq->_size);
        const bool vice_eligible = byteEligible(vice, offset + i);
        const bool self_eligible = byteEligible(this, offset + i);
        if (vice_eligible) {
            // vice is newer
            assert(vice->blockVaddr == blockVaddr);
            lsqreq->SBforwardPackets.push_back(
                LSQRequest::FWDPacket{
                    .idx = goffset + i, .byte = vice->blockDatas[offset + i]});
        } else if (self_eligible) {
            lsqreq->SBforwardPackets.push_back(
                LSQRequest::FWDPacket{
                    .idx = goffset + i, .byte = blockDatas[offset + i]});
        } else {
            full_forward = false;
        }
    }

    return full_forward;
}

void
LSQ::StoreBuffer::setData(std::vector<StoreBufferEntry *> &data_vec)
{
    this->data_vec = data_vec;
    int way = data_vec.size();
    _size = 0;
    max_size = way;
    lru_index.set_capacity(way);
    free_list.set_capacity(way);
    crossRef.resize(way);
    this->data_vec.resize(way);
    data_vld.resize(way, false);
    for (uint64_t i = 0; i < way; i++) {
        free_list.push_back(i);
    }
}

void
LSQ::StoreBuffer::setMaxThread(ThreadID _max_thread)
{
    max_thread = _max_thread;
    vld_cnt_vec.resize(max_thread, 0);
}

bool
LSQ::StoreBuffer::full() const
{
    return free_list.size() == 0;
}

bool
LSQ::StoreBuffer::full(ThreadID tid) const
{
    assert(vld_cnt_vec[tid] <= max_size);
    return (vld_cnt_vec[tid] == (max_size - max_thread + 1));
}

uint64_t
LSQ::StoreBuffer::size() const
{
    return _size;
}

uint64_t
LSQ::StoreBuffer::size(ThreadID tid) const
{
    uint64_t count = 0;
    for (size_t index = 0; index < data_vec.size(); ++index) {
        if (!data_vld[index]) {
            continue;
        }

        auto *entry = data_vec[index];
        if (entry && entry->tid == tid) {
            ++count;
        }
    }
    return count;
}

uint64_t
LSQ::StoreBuffer::size(ThreadID tid, InstSeqNum seq_num) const
{
    uint64_t count = 0;
    for (size_t index = 0; index < data_vec.size(); ++index) {
        if (!data_vld[index]) {
            continue;
        }

        auto *entry = data_vec[index];
        if (entry && entry->tid == tid && entry->seqNum < seq_num) {
            ++count;
        }
    }
    return count;
}

uint64_t
LSQ::StoreBuffer::unsentSize() const
{
    return lru_index.size();
}

LSQ::StoreBufferEntry *
LSQ::StoreBuffer::getEmpty()
{
    assert(!full());
    uint64_t index = free_list.back();
    free_list.pop_back();
    return data_vec[index];
}

void
LSQ::StoreBuffer::insert(StoreBufferEntry *entry)
{
    int index = entry->index;
    ThreadID tid = entry->tid;
    Addr addr = entry->blockPaddr;
    assert(_size < data_vec.size());
    assert(!data_vld[index]);
    assert(!lru_index.full());
    _size++;
    vld_cnt_vec[tid]++;
    assert(vld_cnt_vec[tid] <= max_size);
    auto [it, _] = data_map.insert({hashKey(tid, addr), data_vec[index]});
    crossRef[index] = it;
    data_vld[index] = true;
    lru_index.push_front(index);
}

LSQ::StoreBufferEntry *
LSQ::StoreBuffer::get(ThreadID tid, uint64_t addr) const
{
    auto iter = data_map.find(hashKey(tid, addr));
    if (iter == data_map.end() || iter->second->tid != tid) {
        return nullptr;
    }
    assert(data_vld[iter->second->index]);
    return iter->second;
}

void
LSQ::StoreBuffer::update(int index)
{
    assert(std::find(lru_index.begin(), lru_index.end(), index) !=
           lru_index.end());
    lru_index.erase(std::find(lru_index.begin(), lru_index.end(), index));
    lru_index.push_front(index);
}

LSQ::StoreBufferEntry *
LSQ::StoreBuffer::getEvict()
{
    assert(lru_index.size() > 0);
    uint64_t index = lru_index.back();
    lru_index.pop_back();
    assert(data_vld[index]);
    return data_vec[index];
}

LSQ::StoreBufferEntry *
LSQ::StoreBuffer::getEvict(const bool *eligible_tids,
                           const InstSeqNum *eligible_seq,
                           size_t num_threads)
{
    if (eligible_tids == nullptr && eligible_seq == nullptr) {
        return getEvict();
    }

    for (auto it = lru_index.rbegin(); it != lru_index.rend(); ++it) {
        auto *entry = data_vec[*it];
        if (!entry) {
            continue;
        }

        const ThreadID tid = entry->tid;
        if (tid >= num_threads) {
            continue;
        }
        if (eligible_tids && !eligible_tids[tid]) {
            continue;
        }
        if (eligible_seq &&
            eligible_seq[tid] != static_cast<InstSeqNum>(-1) &&
            entry->seqNum >= eligible_seq[tid]) {
            continue;
        }

        lru_index.erase(std::find(lru_index.begin(), lru_index.end(), *it));
        return entry;
    }

    return nullptr;
}

LSQ::StoreBufferEntry *
LSQ::StoreBuffer::createVice(StoreBufferEntry *entry)
{
    _size++;
    auto vice = getEmpty();
    assert(!entry->vice);
    entry->vice = vice;
    data_vld[vice->index] = true;
    assert(entry->tid < max_thread);
    vld_cnt_vec[entry->tid]++;
    assert(vld_cnt_vec[entry->tid] <= max_size);
    // do not insert map and lru_index
    return vice;
}

void
LSQ::StoreBuffer::release(StoreBufferEntry *entry)
{
    assert(_size > 0);
    _size--;
    vld_cnt_vec[entry->tid]--;
    assert(vld_cnt_vec[entry->tid] >= 0);
    int index = entry->index;
    assert(!entry->inDcacheMainPipe);
    assert(!entry->replayQueued);
    data_vld[index] = false;
    data_map.erase(crossRef[index]);
    assert(std::find(free_list.begin(), free_list.end(), index) ==
           free_list.end());
    free_list.push_back(index);
    if (entry->vice) {
        // make vice regular
        auto vice = entry->vice;
        assert(data_vld[vice->index]);
        auto [it, _] = data_map.insert({hashKey(vice->tid, vice->blockPaddr), vice});
        crossRef[vice->index] = it;
        lru_index.push_front(vice->index);
    }
}

LSQ::LSQStats::LSQStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lqAvgEntryNum, statistics::units::Count::get(),
               "Average number of entries in load queue"),
      ADD_STAT(sqAvgEntryNum, statistics::units::Count::get(),
               "Average number of entries in store queue"),
      ADD_STAT(sbufferAvgEntryNum, statistics::units::Count::get(),
               "Average number of valid entries in store buffer"),
      ADD_STAT(lqFullCycles, statistics::units::Cycle::get(),
               "Cycles that LQ cannot accept a full enqueue bundle"),
      ADD_STAT(sqFullCycles, statistics::units::Cycle::get(),
               "Cycles that SQ cannot accept a full enqueue bundle"),
      ADD_STAT(lsqFullCycles, statistics::units::Cycle::get(),
               "Cycles that LSQ cannot accept a full enqueue bundle"),
      ADD_STAT(sbufferFullCycles, statistics::units::Cycle::get(),
               "Number of cycles that store buffer is physically full"),
      ADD_STAT(sbufferEvictDuetoFlush, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferEvictDuetoFull, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferEvictDuetoSQFull, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferEvictDuetoTimeout, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferDcacheReqFire, statistics::units::Count::get(),
               "Number of sbuffer write requests accepted by dcache"),
      ADD_STAT(sbufferDcacheReqBlocked, statistics::units::Count::get(),
               "Number of sbuffer write request attempts rejected by dcache"),
      ADD_STAT(sbufferDcacheReqBlockedByMainPipe,
               statistics::units::Count::get(),
               "Number of sbuffer write requests blocked by fake dcache mainpipe"),
      ADD_STAT(dcacheMainPipeRefillEnter, statistics::units::Count::get(),
               "Number of refill requests accepted by fake dcache mainpipe"),
      ADD_STAT(dcacheMainPipeStoreEnter, statistics::units::Count::get(),
               "Number of store buffer requests accepted by fake dcache mainpipe"),
      ADD_STAT(dcacheMainPipeStoreBlockedByRefill,
               statistics::units::Count::get(),
               "Number of store buffer requests blocked by pending refill priority"),
      ADD_STAT(dcacheMainPipeStoreBlockedBySet,
               statistics::units::Count::get(),
               "Number of store buffer requests blocked by fake dcache mainpipe set conflict"),
      ADD_STAT(dcacheMainPipeBlockedByS1Backpressure,
               statistics::units::Count::get(),
               "Number of requests blocked by fake dcache mainpipe S1 backpressure"),
      ADD_STAT(dcacheMainPipeStoreBlockedByS1Backpressure,
               statistics::units::Count::get(),
               "Number of store buffer requests blocked by fake dcache mainpipe S1 backpressure"),
      ADD_STAT(dcacheMainPipeRefillBlockedByS1Backpressure,
               statistics::units::Count::get(),
               "Number of refill requests blocked by fake dcache mainpipe S1 backpressure"),
      ADD_STAT(dcacheMainPipeStoreBlockedByTagWrite,
               statistics::units::Count::get(),
               "Number of store buffer requests blocked by fake dcache mainpipe tag write"),
      ADD_STAT(dcacheMainPipeRefillBlocked,
               statistics::units::Count::get(),
               "Number of pending refill requests blocked before fake dcache mainpipe entry"),
      ADD_STAT(dcacheMainPipeRefillBlockedByPipeResource,
               statistics::units::Count::get(),
               "Number of pending refill requests blocked by fake dcache mainpipe resources"),
      ADD_STAT(dcacheMainPipeBlockedByDataConflict,
               statistics::units::Count::get(),
               "Number of fake dcache mainpipe S1 data reads blocked by S4 data writes"),
      ADD_STAT(dcacheMainPipeStoreS2IssueBlocked,
               statistics::units::Count::get(),
               "Number of store buffer requests blocked when issuing from fake dcache mainpipe S2"),
      ADD_STAT(dcacheMainPipeStoreS2MissExit,
               statistics::units::Count::get(),
               "Number of store buffer requests that miss and exit fake dcache mainpipe at S2")
{
}

LSQ::LSQ(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params)
    : cpu(cpu_ptr), iewStage(iew_ptr),
      recentlyloadAddr(8 * (params.DcacheSetDivNum ? params.DcacheSetDivNum : 1)),
      _cacheBlocked(false),
      cacheStorePorts(params.cacheStorePorts), usedStorePorts(0),
      cacheLoadPorts(params.cacheLoadPorts), usedLoadPorts(0),
      sbufferEvictThreshold(params.SbufferEvictThreshold),
      sbufferEntries(params.SbufferEntries),
      storeBufferInactiveThreshold(params.storeBufferInactiveThreshold),
      enableBankConflictCheck(params.BankConflictCheck),
      sbufferBankWriteAccurately(params.sbufferBankWriteAccurately),
      dcacheSetBits(params.DcacheSetBits),
      dcacheSetDivNum(params.DcacheSetDivNum),
      dcacheLineBits(floorLog2(cpu_ptr->cacheLineSize())),
      dcacheSetBankBits(params.DcacheSetBits + 3),
      _enableLdMissReplay(params.EnableLdMissReplay),
      _enablePipeNukeCheck(params.EnablePipeNukeCheck),
      _enableReplayBasedMDP(params.EnableReplayBasedMDP),
      _storeWbStage(params.StoreWbStage),
      waitingForStaleTranslation(false),
      staleTranslationWaitTxnId(0),
      lsqMode(params.smtLSQMode),
      lsqPolicy(params.smtLSQPolicy),
      smtLSQThreshold(params.smtLSQThreshold),
      stats(nullptr),
      LQEntries(params.LQEntries),
      SQEntries(params.SQEntries),
      enqueueWidth(params.renameWidth),
      RARQEntries(params.RARQEntries),
      RAWQEntries(params.RAWQEntries),
      dcachePort(this, cpu_ptr),
      numThreads(params.numThreads)
{
    assert(numThreads > 0 && numThreads <= MaxThreads);
    if (!_enableLdMissReplay && _enablePipeNukeCheck) {
        panic("LSQ can not support pipeline nuke replay when EnableLdMissReplay is False");
    }
    assert(_storeWbStage >= 2 && _storeWbStage <= 4);
    panic_if(dcacheSetDivNum == 0, "DcacheSetDivNum must be >= 1\n");
    panic_if(!isPowerOf2(dcacheSetDivNum),
             "DcacheSetDivNum must be power of two (got %u)\n",
             dcacheSetDivNum);
    panic_if(dcacheSetBankBits >= 64,
             "DcacheSetBits too large for bank conflict model (setBits=%u)\n",
             dcacheSetBits);
    panic_if(dcacheSetDivNum > (1ULL << dcacheSetBits),
             "DcacheSetDivNum (%u) must be <= num_sets (2^%u)\n",
             dcacheSetDivNum, dcacheSetBits);

    cpu->addStatGroup("lsq", &stats);

    //**********************************************
    //************ Handle SMT Parameters ***********
    //**********************************************

    if (lsqMode == SMTLSQMode::Independent) {
        DPRINTF(LSQ, "LSQ mode set to Independent: each thread gets up to "
                "%u LQ, %u SQ, %u RARQ and %u RAWQ entries\n",
                LQEntries, SQEntries, RARQEntries, RAWQEntries);
    } else if (lsqMode == SMTLSQMode::Shared) {
        panic_if(lsqPolicy == SMTQueuePolicy::Threshold &&
                 smtLSQThreshold == 0,
                 "SMT LSQ threshold must be non-zero in shared threshold mode");

        if (lsqPolicy == SMTQueuePolicy::Dynamic ||
            lsqPolicy == SMTQueuePolicy::DynamicBorrowing) {
            DPRINTF(LSQ, "LSQ mode set to Shared/Dynamic: %u LQ and %u SQ "
                    "entries are shared across active SMT threads, along "
                    "with %u RARQ and %u RAWQ entries\n",
                    LQEntries, SQEntries, RARQEntries, RAWQEntries);
        } else if (lsqPolicy == SMTQueuePolicy::Partitioned) {
            DPRINTF(LSQ, "LSQ mode set to Shared/Partitioned\n");
        } else if (lsqPolicy == SMTQueuePolicy::Threshold) {
            DPRINTF(LSQ, "LSQ mode set to Shared/Threshold: threshold=%u\n",
                    smtLSQThreshold);
        } else {
            panic("Invalid LSQ sharing policy. Options are: Dynamic, "
                        "Partitioned, Threshold, DynamicBorrowing");
        }
    } else {
        panic("Invalid SMT LSQ mode. Options are: Independent, Shared");
    }

    thread.reserve(numThreads);
    // TODO: Parameterize the load/store pipeline stages
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        thread.emplace_back(LQEntries, SQEntries,
            params.LdPipeStages, params.StPipeStages, params.RARQEntries, params.RAWQEntries,
            params.RARDequeuePerCycle, params.RAWDequeuePerCycle, params.LoadCompletionWidth,
            params.StoreCompletionWidth);
        thread[tid].init(cpu, iew_ptr, params, this, tid);
        thread[tid].setDcachePort(&dcachePort);
        _storeBufferFlushing[tid] = false;
    }

    std::vector<StoreBufferEntry *> store_buffer_entries;
    for (uint32_t i = 0; i < sbufferEntries; ++i) {
        store_buffer_entries.push_back(new StoreBufferEntry(cpu->cacheLineSize(), i));
    }
    storeBuffer.setData(store_buffer_entries);
    storeBuffer.setMaxThread(numThreads);
    bankOccupied.resize(dcacheSetDivNum, std::vector<bool>(numBank, false));
}


std::string
LSQ::name() const
{
    return iewStage->name() + ".lsq";
}

void
LSQ::recordStoreBufferEviction(StoreBufferEvictCause cause)
{
    switch (cause) {
      case StoreBufferEvictCause::Flush:
        stats.sbufferEvictDuetoFlush++;
        break;
      case StoreBufferEvictCause::Full:
        stats.sbufferEvictDuetoFull++;
        break;
      case StoreBufferEvictCause::SQFull:
        stats.sbufferEvictDuetoSQFull++;
        break;
      case StoreBufferEvictCause::Timeout:
        stats.sbufferEvictDuetoTimeout++;
        break;
    }
}

void
LSQ::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
    assert(activeThreads != 0);
}

void
LSQ::drainSanityCheck() const
{
    assert(isDrained());

    for (ThreadID tid = 0; tid < numThreads; tid++)
        thread[tid].drainSanityCheck();
}

bool
LSQ::isDrained() const
{
    bool drained(true);

    if (!lqEmpty()) {
        DPRINTF(Drain, "Not drained, LQ not empty.\n");
        drained = false;
    }

    if (!sqEmpty()) {
        DPRINTF(Drain, "Not drained, SQ not empty.\n");
        drained = false;
    }

    return drained;
}

void
LSQ::takeOverFrom()
{
    usedStorePorts = 0;
    _cacheBlocked = false;

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        thread[tid].takeOverFrom();
    }
}

void
LSQ::tick()
{
    // Re-issue loads which got blocked on the per-cycle load ports limit.
    if (usedLoadPorts == cacheLoadPorts && !_cacheBlocked)
        iewStage->cacheUnblocked();

    usedLoadPorts = 0;
    usedStorePorts = 0;
    // tick lsq_unit
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    unsigned lq_entry_num = 0;
    unsigned sq_entry_num = 0;
    bool lq_full = false;
    bool sq_full = false;

    while (threads != end) {
        ThreadID tid = *threads++;
        lq_entry_num += thread[tid].numLoads();
        sq_entry_num += thread[tid].numStores();
        // TODO: this per-thread OR is an approximation for SMT/shared-LSQ
        // configurations. With multiple active threads it may not match the
        // aggregate free-entry condition seen by rename/dispatch.
        lq_full = lq_full || thread[tid].numFreeLoadEntries() < enqueueWidth;
        sq_full = sq_full || thread[tid].numFreeStoreEntries() < enqueueWidth;
        thread[tid].tick();
    }

    // Sample current load queue occupancy once per cycle.
    stats.lqAvgEntryNum = lq_entry_num;

    // Sample current store queue occupancy once per cycle.
    stats.sqAvgEntryNum = sq_entry_num;

    // Sample current store buffer occupancy once per cycle.
    stats.sbufferAvgEntryNum = storeBuffer.size();
    if (storeBuffer.full()) {
        ++stats.sbufferFullCycles;
    }

    if (lq_full) {
        ++stats.lqFullCycles;
    }
    if (sq_full) {
        ++stats.sqFullCycles;
    }
    if (lq_full || sq_full) {
        ++stats.lsqFullCycles;
    }

}

void
LSQ::clearAddresses()
{
    advanceDcacheMainPipe();
    markDcacheMainPipeBusyBanks();
    recentlyloadAddr.clear();
}

void
LSQ::advanceDcacheMainPipe()
{
    DcacheMainPipeBufferedPipe next_pipe = {};

    const auto &s1_data_read =
        dcacheMainPipeStage(DcacheMainPipeStage::S1DataRead);
    const auto &s2_data_resp =
        dcacheMainPipeStage(DcacheMainPipeStage::S2DataResp);
    const auto &s3_tag_write =
        dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite);
    const auto &s4_data_write =
        dcacheMainPipeStage(DcacheMainPipeStage::S4DataWrite);

    auto &next_s1_data_read =
        next_pipe.at(dcacheMainPipeIndex(DcacheMainPipeStage::S1DataRead));
    auto &next_s2_data_resp =
        next_pipe.at(dcacheMainPipeIndex(DcacheMainPipeStage::S2DataResp));
    auto &next_s3_tag_write =
        next_pipe.at(dcacheMainPipeIndex(DcacheMainPipeStage::S3TagWrite));
    auto &next_s4_data_write =
        next_pipe.at(dcacheMainPipeIndex(DcacheMainPipeStage::S4DataWrite));

    // S4 resources are modeled as always ready for now. Keeping the local
    // variable makes later resource backpressure additions contained here.
    const bool s4_can_go = true;
    const bool s4_ready = !s4_data_write.valid || s4_can_go;
    const bool s3_can_go = s4_ready;
    const bool s3_ready = !s3_tag_write.valid || s3_can_go;
    const bool s2_can_go = s3_ready;

    DcacheMainPipeS2Result s2_issue_result =
        DcacheMainPipeS2Result::Blocked;
    if (s2_data_resp.valid && s2_can_go) {
        // StoreBuffer S2 issue can either hit and proceed to S3, or miss and
        // leave the fake pipe while the classic cache tracks the miss.
        s2_issue_result = !s2_data_resp.req.onS2Issue ?
            DcacheMainPipeS2Result::GoToS3 :
            s2_data_resp.req.onS2Issue(curTick());
    }
    const bool s2_ready = !s2_data_resp.valid || s2_can_go;

    const bool s1_data_conflict = hasDcacheMainPipeDataArrayConflict();
    const bool s1_can_go = s2_ready && !s1_data_conflict;

    if (s1_data_conflict) {
        ++stats.dcacheMainPipeBlockedByDataConflict;
    }

    if (s4_data_write.valid && s4_can_go &&
        s4_data_write.req.onComplete) {
        s4_data_write.req.onComplete(curTick());
    }

    if (s4_data_write.valid && !s4_can_go) {
        next_s4_data_write = s4_data_write;
    }

    if (s3_tag_write.valid && !s3_can_go) {
        next_s3_tag_write = s3_tag_write;
    } else if (s3_tag_write.valid) {
        next_s4_data_write = s3_tag_write;
    }

    if (s2_data_resp.valid) {
        if (!s2_can_go) {
            next_s2_data_resp = s2_data_resp;
        } else if (s2_issue_result == DcacheMainPipeS2Result::GoToS3) {
            next_s3_tag_write = s2_data_resp;
        }
    }

    if (s1_data_read.valid) {
        if (s1_can_go) {
            next_s2_data_resp = s1_data_read;
        } else {
            next_s1_data_read = s1_data_read;
        }
    }

    if (!dcacheMainPipeRefillQ.empty()) {
        const auto &queued_refill = dcacheMainPipeRefillQ.front();
        if (canEnterDcacheMainPipe(queued_refill, next_pipe)) {
            next_s1_data_read.valid = true;
            next_s1_data_read.req = queued_refill;
            dcacheMainPipeRefillQ.pop();
            ++stats.dcacheMainPipeRefillEnter;
        } else {
            ++stats.dcacheMainPipeRefillBlocked;
            ++stats.dcacheMainPipeRefillBlockedByPipeResource;
        }
    }

    dcacheMainPipe = next_pipe;
}

LSQ::DcacheMainPipeSlot &
LSQ::dcacheMainPipeStage(DcacheMainPipeStage stage)
{
    return dcacheMainPipe.at(dcacheMainPipeIndex(stage));
}

const LSQ::DcacheMainPipeSlot &
LSQ::dcacheMainPipeStage(DcacheMainPipeStage stage) const
{
    return dcacheMainPipe.at(dcacheMainPipeIndex(stage));
}

bool
LSQ::willDcacheRefillTagWriteNextCycle() const
{
    const auto &s2_data_resp =
        dcacheMainPipeStage(DcacheMainPipeStage::S2DataResp);
    const auto &s3_tag_write =
        dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite);
    const auto &s4_data_write =
        dcacheMainPipeStage(DcacheMainPipeStage::S4DataWrite);

    // Keep these readiness rules aligned with advanceDcacheMainPipe().
    const bool s4_can_go = true;
    const bool s4_ready = !s4_data_write.valid || s4_can_go;
    const bool s3_can_go = s4_ready;
    const bool s3_ready = !s3_tag_write.valid || s3_can_go;
    const bool s2_can_go = s3_ready;

    const DcacheMainPipeSlot *next_s3 = nullptr;
    if (s3_tag_write.valid && !s3_can_go) {
        next_s3 = &s3_tag_write;
    } else if (s2_data_resp.valid && s2_can_go) {
        next_s3 = &s2_data_resp;
    }

    return next_s3 &&
        next_s3->req.isRefill() &&
        next_s3->req.needTagWrite;
}

LSQ::DcacheBankMask
LSQ::fullDcacheBankMask() const
{
    DcacheBankMask mask = {};
    std::fill(mask.begin(), mask.end(), true);
    return mask;
}

LSQ::DcacheBankMask
LSQ::storeMaskToDcacheBanks(const std::vector<bool> &mask) const
{
    assert(mask.size() == DcacheBankCount * 8);
    DcacheBankMask bank_mask = {};
    if (sbufferBankWriteAccurately) {
        for (unsigned bank = 0; bank < DcacheBankCount; ++bank) {
            bank_mask.at(bank) = std::any_of(
                mask.begin() + 8 * bank, mask.begin() + 8 * bank + 8,
                [](bool v) { return v; });
        }
    } else {
        std::fill(bank_mask.begin(), bank_mask.end(), true);
    }
    return bank_mask;
}

LSQ::DcacheMainPipeRequest
LSQ::makeDcacheRefillMainPipeRequest(
    Addr addr, bool need_data_read,
    DcacheMainPipeCompleteCallback on_complete) const
{
    DcacheMainPipeRequest req;
    req.source = DcacheMainPipeSource::Refill;
    req.addr = addr;
    req.div = getDcacheDiv(addr);
    req.setKey = getDcacheSetKey(addr);
    req.needDataRead = need_data_read;
    req.needTagWrite = true;
    req.needDataWrite = true;
    req.needWritebackPort = need_data_read;
    req.readBanks = need_data_read ? fullDcacheBankMask() : DcacheBankMask{};
    req.writeBanks = fullDcacheBankMask();
    req.onComplete = std::move(on_complete);
    return req;
}

LSQ::DcacheMainPipeRequest
LSQ::makeStoreBufferMainPipeRequest(
    const StoreBufferEntry &entry, DcacheMainPipeS2Callback on_s2_issue) const
{
    DcacheMainPipeRequest req;
    req.source = DcacheMainPipeSource::StoreBuffer;
    req.addr = entry.blockVaddr;
    req.div = getDcacheDiv(entry.blockVaddr);
    req.setKey = getDcacheSetKey(entry.blockVaddr);
    req.writeBanks = storeMaskToDcacheBanks(entry.validMask);
    req.needDataWrite = dcacheBankMaskAny(req.writeBanks);
    req.needTagWrite = false;

    DcacheBankMask full_write = fullDcacheBankMask();
    for (unsigned bank = 0; bank < DcacheBankCount; ++bank) {
        const bool bank_fully_written = std::all_of(
            entry.validMask.begin() + 8 * bank,
            entry.validMask.begin() + 8 * bank + 8,
            [](bool v) { return v; });
        full_write.at(bank) = bank_fully_written;
    }

    for (unsigned bank = 0; bank < DcacheBankCount; ++bank) {
        req.readBanks.at(bank) = req.writeBanks.at(bank) && !full_write.at(bank);
    }
    req.needDataRead = dcacheBankMaskAny(req.readBanks);
    req.onS2Issue = std::move(on_s2_issue);
    return req;
}

void
LSQ::markDcacheMainPipeBusyBanks()
{
    for (unsigned div = 0; div < dcacheSetDivNum; ++div) {
        std::fill(bankOccupied.at(div).begin(), bankOccupied.at(div).end(),
                  false);
    }

    auto mark_banks = [this](const DcacheMainPipeRequest &req,
                             const DcacheBankMask &mask) {
        for (unsigned bank = 0; bank < DcacheBankCount; ++bank) {
            if (mask.at(bank)) {
                bankOccupied.at(req.div).at(bank) = true;
            }
        }
    };

    const auto &s1_data_read =
        dcacheMainPipeStage(DcacheMainPipeStage::S1DataRead);
    const auto &s4_data_write =
        dcacheMainPipeStage(DcacheMainPipeStage::S4DataWrite);

    if (s1_data_read.valid && s1_data_read.req.needDataRead) {
        mark_banks(s1_data_read.req, s1_data_read.req.readBanks);
    }
    if (s4_data_write.valid && s4_data_write.req.needDataWrite) {
        mark_banks(s4_data_write.req, s4_data_write.req.writeBanks);
    }
}

bool
LSQ::dcacheBankMaskAny(const DcacheBankMask &mask) const
{
    return std::any_of(mask.begin(), mask.end(), [](bool v) { return v; });
}

bool
LSQ::dcacheBankMaskOverlap(const DcacheBankMask &lhs,
                           const DcacheBankMask &rhs) const
{
    for (unsigned bank = 0; bank < DcacheBankCount; ++bank) {
        if (lhs.at(bank) && rhs.at(bank)) {
            return true;
        }
    }
    return false;
}

bool
LSQ::hasDcacheMainPipeDataArrayConflict() const
{
    const auto &s1_data_read =
        dcacheMainPipeStage(DcacheMainPipeStage::S1DataRead);
    const auto &s4_data_write =
        dcacheMainPipeStage(DcacheMainPipeStage::S4DataWrite);

    return s1_data_read.valid &&
        s1_data_read.req.needDataRead &&
        s4_data_write.valid &&
        s4_data_write.req.needDataWrite &&
        s1_data_read.req.div == s4_data_write.req.div &&
        dcacheBankMaskOverlap(s1_data_read.req.readBanks,
                              s4_data_write.req.writeBanks);
}

bool
LSQ::isDcacheMainPipeSetBlocked(uint64_t set_key) const
{
    // S4 only models data write resource usage; it does not block S0 tag/meta
    // reads by same-set conflict.
    for (unsigned stage = static_cast<unsigned>(DcacheMainPipeStage::S1DataRead);
         stage <= static_cast<unsigned>(DcacheMainPipeStage::S3TagWrite);
         ++stage) {
        const auto &pipe_stage =
            dcacheMainPipeStage(static_cast<DcacheMainPipeStage>(stage));
        if (pipe_stage.valid && pipe_stage.req.setKey == set_key) {
            return true;
        }
    }
    return false;
}

bool
LSQ::canEnterDcacheMainPipe(
    const DcacheMainPipeRequest &request,
    const DcacheMainPipeBufferedPipe &next_pipe)
{
    const bool s0_tag_read_blocked =
        dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite).valid &&
        dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite).req.needTagWrite;
    const bool s1_backpressured =
        next_pipe.at(dcacheMainPipeIndex(DcacheMainPipeStage::S1DataRead)).valid;

    bool blocked = false;
    if (s1_backpressured) {
        ++stats.dcacheMainPipeBlockedByS1Backpressure;
        if (request.isStoreBuffer()) {
            ++stats.dcacheMainPipeStoreBlockedByS1Backpressure;
        } else if (request.isRefill()) {
            ++stats.dcacheMainPipeRefillBlockedByS1Backpressure;
        }
        blocked = true;
    }
    if (s0_tag_read_blocked) {
        if (request.isStoreBuffer()) {
            ++stats.dcacheMainPipeStoreBlockedByTagWrite;
        }
        blocked = true;
    }
    if (blocked) {
        return false;
    }
    return !isDcacheMainPipeSetBlocked(request.setKey);
}

bool
LSQ::canEnterStoreBufferDcacheMainPipe(const StoreBufferEntry &entry)
{
    const auto req = makeStoreBufferMainPipeRequest(entry);
    const bool s1_backpressured =
        dcacheMainPipeStage(DcacheMainPipeStage::S1DataRead).valid;
    const bool s0_tag_read_blocked =
        dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite).valid &&
        dcacheMainPipeStage(DcacheMainPipeStage::S3TagWrite).req.needTagWrite;

    if (!dcacheMainPipeRefillQ.empty()) {
        if (s1_backpressured) {
            ++stats.dcacheMainPipeStoreBlockedByS1Backpressure;
        }
        if (s0_tag_read_blocked) {
            ++stats.dcacheMainPipeStoreBlockedByTagWrite;
        }
        ++stats.dcacheMainPipeStoreBlockedByRefill;
        return false;
    }

    bool blocked = false;
    if (s1_backpressured) {
        ++stats.dcacheMainPipeBlockedByS1Backpressure;
        ++stats.dcacheMainPipeStoreBlockedByS1Backpressure;
        blocked = true;
    }
    if (s0_tag_read_blocked) {
        ++stats.dcacheMainPipeStoreBlockedByTagWrite;
        blocked = true;
    }
    if (isDcacheMainPipeSetBlocked(req.setKey)) {
        ++stats.dcacheMainPipeStoreBlockedBySet;
        blocked = true;
    }
    if (blocked) {
        return false;
    }

    return true;
}

void
LSQ::enterStoreBufferDcacheMainPipe(StoreBufferEntry &entry, PacketPtr data_pkt)
{
    const auto req = makeStoreBufferMainPipeRequest(
        entry,
        [this, data_pkt](Tick tick) {
            return issueSbufferPacketFromDcacheMainPipe(data_pkt, tick);
        });
    auto &s1_data_read =
        dcacheMainPipeStage(DcacheMainPipeStage::S1DataRead);
    assert(!s1_data_read.valid);
    s1_data_read.valid = true;
    s1_data_read.req = req;
    entry.inDcacheMainPipe = true;
    ++stats.dcacheMainPipeStoreEnter;
}

unsigned
LSQ::getDcacheDiv(Addr vaddr) const
{
    return (vaddr >> dcacheLineBits) & (dcacheSetDivNum - 1);
}

uint64_t
LSQ::getDcacheSetKey(Addr vaddr) const
{
    return (vaddr >> dcacheLineBits) & ((1ULL << dcacheSetBits) - 1);
}

uint64_t
LSQ::getDcacheBankSetKey(Addr vaddr) const
{
    // [setIndex][bankIndex][dataOffset]
    //         ^ (cacheLineBits)   ^ (3 bits)
    return (vaddr >> 3) & ((1ULL << dcacheSetBankBits) - 1);
}

uint64_t
LSQ::getDcacheDivBankSetKey(Addr vaddr) const
{
    return (static_cast<uint64_t>(getDcacheDiv(vaddr)) << dcacheSetBankBits) |
        getDcacheBankSetKey(vaddr);
}

bool
LSQ::loadBankConflictedCheck(Addr vaddr)
{
    bool now_bank_conflict = false;
    const int bankIndex = bankNum(vaddr);
    const unsigned div = getDcacheDiv(vaddr);
    const uint64_t key = getDcacheDivBankSetKey(vaddr);

    if (enableBankConflictCheck) {
        if (recentlyloadAddr.contains(key)) {
            recentlyloadAddr.get(key);
            return false;
        }
        if (bankOccupied[div][bankIndex]) {
            now_bank_conflict = true;

        } else {
            bankOccupied[div][bankIndex] = true;
            recentlyloadAddr.insert(key, {});
        }
    }
    return now_bank_conflict;
}

void
LSQ::notifyDcacheRefill(
    Addr addr, bool need_data_read,
    DcacheMainPipeCompleteCallback on_complete)
{
    dcacheMainPipeRefillQ.push(
        makeDcacheRefillMainPipeRequest(
            addr, need_data_read, std::move(on_complete)));
    cpu->wakeCPU();
    cpu->activityThisCycle();
}

unsigned
LSQ::getFreeLQEntries(ThreadID tid)
{
    return logicalFreeLoadEntries(tid);
}

unsigned
LSQ::getFreeSQEntries(ThreadID tid)
{
    return logicalFreeStoreEntries(tid);
}

unsigned
LSQ::getAndResetLastLQPopEntries(ThreadID tid)
{
    return thread[tid].getAndResetLastClockLQPopEntries();
}

unsigned
LSQ::getAndResetLastSQPopEntries(ThreadID tid)
{
    return thread[tid].getAndResetLastClockSQPopEntries();
}

bool
LSQ::cacheBlocked() const
{
    return _cacheBlocked;
}

void
LSQ::cacheBlocked(bool v)
{
    _cacheBlocked = v;
}

bool
LSQ::cachePortAvailable(bool is_load) const
{
    bool ret;
    if (is_load) {
        ret  = usedLoadPorts < cacheLoadPorts;
    } else {
        ret  = usedStorePorts < cacheStorePorts;
    }
    return ret;
}

void
LSQ::cachePortBusy(bool is_load)
{
    assert(cachePortAvailable(is_load));
    if (is_load) {
        usedLoadPorts++;
    } else {
        usedStorePorts++;
    }
}

void
LSQ::insertLoad(const DynInstPtr &load_inst)
{
    ThreadID tid = load_inst->threadNumber;

    thread[tid].insertLoad(load_inst);
}

void
LSQ::insertStore(const DynInstPtr &store_inst)
{
    ThreadID tid = store_inst->threadNumber;

    thread[tid].insertStore(store_inst);
}

bool
LSQ::splitStoreAddrSquashed(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    return thread[tid].splitStoreAddrSquashed(inst);
}

void
LSQ::issueToLoadPipe(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    thread[tid].issueToLoadPipe(inst);
}

void
LSQ::issueToStorePipe(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    thread[tid].issueToStorePipe(inst);
}

void
LSQ::executePipeSx()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        thread[tid].executePipeSx();
    }
}

Fault
LSQ::executeAmo(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    return thread[tid].executeAmo(inst);
}

void
LSQ::commitLoads(InstSeqNum &youngest_inst, ThreadID tid)
{
    thread.at(tid).commitLoads(youngest_inst);
}

void
LSQ::commitStores(InstSeqNum &youngest_inst, ThreadID tid)
{
    thread.at(tid).commitStores(youngest_inst);
}

void
LSQ::processWriteback()
{
    // after load sendpackets
    // before sbuffer sendpackets
    clearAddresses();

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    while (threads != end) {
        ThreadID tid = *threads++;
        thread[tid].writebackBlockedStore(); // amo
    }

    storeBufferWriteback();


    if (storeBufferBlocked()) {
        DPRINTF(StoreBuffer, "Store buffer is blocking, skip SQ offload\n");
        return;
    }

    std::vector<uint32_t> offload_quota(numThreads, 0);
    std::vector<uint32_t> offload_demand(numThreads, 0);
    std::vector<ThreadID> requester_tids;
    std::vector<bool> offload_fail(numThreads, false);
    requester_tids.reserve(activeThreads->size());

    for (ThreadID tid : *activeThreads) {
        offload_demand[tid] = thread[tid].countStoreBufferOffloadableEntries(
            maxStoreBufferEntriesAcceptedFromSQPerCycle);
        if (offload_demand[tid] != 0) {
            requester_tids.push_back(tid);
        }
    }
    if (!requester_tids.empty()) {
        size_t start_idx = 0;
        if (nextStoreBufferOffloadTid != InvalidThreadID) {
            auto it = std::find(requester_tids.begin(), requester_tids.end(),
                                nextStoreBufferOffloadTid);
            if (it != requester_tids.end()) {
                start_idx = std::distance(requester_tids.begin(), it);
            }
        }

        uint32_t remaining_budget = maxStoreBufferEntriesAcceptedFromSQPerCycle;
        size_t cursor = start_idx;
        while (remaining_budget != 0) {
            bool granted = false;
            for (size_t scanned = 0; scanned < requester_tids.size();
                ++scanned) {
                const size_t idx = (cursor + scanned) % requester_tids.size();
                const ThreadID tid = requester_tids[idx];
                if (offload_quota[tid] >= offload_demand[tid]) {
                    continue;
                }

                ++offload_quota[tid];
                --remaining_budget;
                cursor = (idx + 1) % requester_tids.size();
                nextStoreBufferOffloadTid = requester_tids[cursor];
                granted = true;
                break;
            }

            if (!granted) {
                break;
            }
        }
    }
    bool has_thread_offloaded = false;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        thread[(nextStoreBufferInsertTid + tid) % numThreads].offloadToStoreBuffer(offload_quota[(nextStoreBufferInsertTid + tid) % numThreads], offload_fail);
        has_thread_offloaded |= ((offload_quota[(nextStoreBufferInsertTid + tid) % numThreads] != 0) 
                                && !(offload_fail[(nextStoreBufferInsertTid + tid) % numThreads]));
        
    }

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (offload_fail[tid] && has_thread_offloaded) {
            nextStoreBufferInsertTid = tid;
        }
    }

    // A fence/flush only waits for the requesting thread's sbuffer domain.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!storeBufferFlushing(tid) ||
            !storeBufferEmpty(tid, _storeBufferFlushBeforeSeq[tid])) {
            continue;
        }

        clearStoreBufferFlushing(tid);
        cpu->activityThisCycle();
    }
}

void
LSQ::storeBufferWriteback()
{
    bool can_evict = true;

    // write request will stall one cycle
    // so 2 cycle send one write request
    if (getDcacheWriteStall()) {
        setDcacheWriteStall(false);
        can_evict = false;
    }

    // Replayed S2 exits are retried before pre-admission blocks and new
    // evictions.
    if (can_evict && retryReplayStoreBuffer()) {
        can_evict = false;
    }
    if (can_evict && retryBlockedStoreBuffer()) {
        can_evict = false;
    }

    if (can_evict && storeBuffer.unsentSize() != 0) {
        bool any_sq_will_full = false;
        for (ThreadID tid : *activeThreads) {
            if (thread[tid].storeBufferSQWillFull()) {
                any_sq_will_full = true;
            }
        }

        std::optional<StoreBufferEvictCause> cause;
        StoreBufferEntry *entry = nullptr;
        if (storeBufferFlushing()) {
            entry = storeBuffer.getEvict(
                _storeBufferFlushing, _storeBufferFlushBeforeSeq, numThreads);
            if (entry) {
                cause = StoreBufferEvictCause::Flush;
                DPRINTF(StoreBuffer, "sbuffer flushing\n");
            }
        }
        if (!entry && storeBuffer.unsentSize() > getSbufferEvictThreshold()) {
            cause = StoreBufferEvictCause::Full;
            DPRINTF(StoreBuffer, "sbuffer has reached threshold\n");
        } else if (!entry && any_sq_will_full) {
            cause = StoreBufferEvictCause::SQFull;
            DPRINTF(StoreBuffer, "sbuffer has reached SQ threshold\n");
        } else if (!entry && getStoreBufferInactiveCycles() >
                   getStoreBufferInactiveThreshold()) {
            cause = StoreBufferEvictCause::Timeout;
            DPRINTF(StoreBuffer, "sbuffer has reached timeout\n");
        } else if (!entry) {
            incStoreBufferInactiveCycles();
        }

        if (cause) {
            if (!entry) {
                entry = storeBuffer.getEvict();
            }
            if (!entry) {
                return;
            }
            auto &owner_unit = thread[entry->tid];
            recordStoreBufferEviction(*cause);
            DPRINTF(StoreBuffer, "Evicting sbuffer entry[%#x]\n",
                    entry->blockPaddr);
            if (debug::StoreBuffer) {
                DPRINTFR(StoreBuffer, "Dumping sbuffer entry data\n");
                for (int i = 0; i < owner_unit.cacheLineSize(); i++) {
                    DPRINTFR(StoreBuffer, "%s%d ",
                             entry->validMask[i] ? "" : "!",
                             (uint32_t)entry->blockDatas[i]);
                }
                DPRINTFR(StoreBuffer, "\n");
            }

            assert(entry->request == nullptr);
            entry->request = new SbufferRequest(cpu, &owner_unit,
                                                entry->blockPaddr,
                                                entry->blockDatas.data());
            entry->request->addReq(entry->blockVaddr, entry->blockPaddr,
                                   entry->validMask);
            entry->request->buildPackets();
            entry->request->sbuffer_entry = entry;
            bool success = entry->request->sendPacketToCache();
            if (!success) {
                setBlockedStoreBufferEntry(
                    entry,
                    lastSbufferSendBlockedByMainPipe ?
                        StoreBufferBlockCause::MainPipe :
                        StoreBufferBlockCause::CachePort);
                DPRINTF(StoreBuffer, "send packet fail\n");
            } else {
                DPRINTF(StoreBuffer, "enter dcache mainpipe successed\n");
                resetStoreBufferInactiveCycles();
            }
        }
    }
}

bool
LSQ::retryReplayStoreBuffer()
{
    if (sbufferMainPipeReplayQ.empty()) {
        return false;
    }

    // The front replay has priority, but a failed timing send must wait for the
    // cache retry before it can re-enter the fake pipe.
    if (cacheBlocked()) {
        return true;
    }

    auto *entry = sbufferMainPipeReplayQ.front();
    assert(entry);
    assert(entry->replayQueued);
    assert(!entry->inDcacheMainPipe);
    assert(!entry->sending);
    assert(entry->request);

    bool success = entry->request->sendPacketToCache();
    if (!success) {
        // Keep the front replay queued; it will retry from S0 later.
        return true;
    }

    entry->replayQueued = false;
    sbufferMainPipeReplayQ.pop_front();
    resetStoreBufferInactiveCycles();
    return true;
}

bool
LSQ::retryBlockedStoreBuffer()
{
    if (!blockedSbufferEntry) {
        return false;
    }

    bool success = blockedSbufferEntry->request->sendPacketToCache();
    if (!success) {
        setBlockedStoreBufferEntry(
            blockedSbufferEntry,
            lastSbufferSendBlockedByMainPipe ?
                StoreBufferBlockCause::MainPipe :
                StoreBufferBlockCause::CachePort);
        return true;
    }

    resetStoreBufferInactiveCycles();
    clearBlockedStoreBufferEntry();
    return true;
}

bool
LSQ::sbufferEnterDcacheMainPipe(PacketPtr data_pkt)
{
    lastSbufferSendBlockedByMainPipe = false;

    auto request = dynamic_cast<SbufferRequest *>(data_pkt->senderState);
    assert(request);
    assert(request->sbuffer_entry);
    assert(!request->sbuffer_entry->sending);
    assert(!request->sbuffer_entry->inDcacheMainPipe);

    if (!canEnterStoreBufferDcacheMainPipe(*request->sbuffer_entry)) {
        lastSbufferSendBlockedByMainPipe = true;
        ++stats.sbufferDcacheReqBlocked;
        ++stats.sbufferDcacheReqBlockedByMainPipe;
        return false;
    }

    enterStoreBufferDcacheMainPipe(*request->sbuffer_entry, data_pkt);
    return true;
}

LSQ::DcacheMainPipeS2Result
LSQ::issueSbufferPacketFromDcacheMainPipe(PacketPtr data_pkt, Tick issue_tick)
{
    DcacheMainPipeS2Result result = DcacheMainPipeS2Result::GoToS3;
    bool cache_got_blocked = false;

    auto request = dynamic_cast<SbufferRequest *>(data_pkt->senderState);
    assert(request);
    assert(request->sbuffer_entry);
    assert(request->_numOutstandingPackets == 0);
    assert(request->sbuffer_entry->inDcacheMainPipe);
    assert(!request->sbuffer_entry->sending);

    data_pkt->sendTick = issue_tick;
    data_pkt->clearDcacheMainPipeSbufferHit();
    data_pkt->setDcacheMainPipeSbufferReq();
    data_pkt->setLSQPtr(this);

    // Issue to the real classic cache only at fake S2 so StoreBuffer misses
    // cannot allocate or merge MSHRs at fake-pipe admission time.
    if (!cacheBlocked() && cachePortAvailable(false)) {
        if (!dcachePort.sendTimingReq(data_pkt)) {
            result = DcacheMainPipeS2Result::Blocked;
            cache_got_blocked = true;
        }
    } else {
        result = DcacheMainPipeS2Result::Blocked;
    }

    if (result == DcacheMainPipeS2Result::GoToS3) {
        stats.sbufferDcacheReqFire++;
        cachePortBusy(false);
        request->_numOutstandingPackets = 1;
        request->sbuffer_entry->inDcacheMainPipe = false;
        request->sbuffer_entry->sending = true;
        resetStoreBufferInactiveCycles();
        result = data_pkt->isDcacheMainPipeSbufferHit() ?
            DcacheMainPipeS2Result::GoToS3 :
            DcacheMainPipeS2Result::ExitPipe;
        if (result == DcacheMainPipeS2Result::ExitPipe) {
            ++stats.dcacheMainPipeStoreS2MissExit;
        }
    } else {
        auto *entry = request->sbuffer_entry;
        stats.sbufferDcacheReqBlocked++;
        ++stats.dcacheMainPipeStoreS2IssueBlocked;
        entry->inDcacheMainPipe = false;
        // S2 issue failed. Exit the fake pipe and let the StoreBuffer replay
        // this eviction from S0 through the replay queue.
        if (!entry->replayQueued) {
            entry->replayQueued = true;
            sbufferMainPipeReplayQ.push_back(entry);
        }
        if (cache_got_blocked) {
            cacheBlocked(true);

            request->_port.recordStoreBufferBlockedByCache();
        }
    }

    return result;
}

void
LSQ::completeSbufferEvict(PacketPtr pkt)
{
    auto request = dynamic_cast<SbufferRequest *>(pkt->senderState);
    cpu->consumeSyncVisibleStoreReplay(request->sbuffer_entry->tid);
    notifyOtherThreadsStoreVisible(request->sbuffer_entry->tid,
                                   request->mainReq()->getPaddr(),
                                   request->mainReq()->getByteEnable());
    if (cpu->goldenMemManager() &&
        cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr())) {
        Addr paddr = request->mainReq()->getPaddr();
        DPRINTF(LSQ, "StoreBuffer writing to golden memory at addr %#x\n",
                paddr);
        cpu->goldenMemManager()->updateGoldenMem(
            paddr, request->_data, request->mainReq()->getByteEnable(),
            request->_size);
    }

    storeBuffer.release(request->sbuffer_entry);
    DPRINTF(StoreBuffer,
            "finish entry[%#x] evict to cache, sbuffer size: %d, "
            "unsentsize: %d\n",
            pkt->getAddr(), storeBuffer.size(), storeBuffer.unsentSize());
}

void
LSQ::squash(const InstSeqNum &squashed_num, ThreadID tid)
{
    thread.at(tid).squash(squashed_num);
}

bool
LSQ::violation()
{
    /* Answers: Does Anybody Have a Violation?*/
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (thread[tid].violation())
            return true;
    }

    return false;
}

bool LSQ::violation(ThreadID tid) { return thread.at(tid).violation(); }

DynInstPtr
LSQ::getMemDepViolator(ThreadID tid)
{
    return thread.at(tid).getMemDepViolator();
}

int
LSQ::getLoadHead(ThreadID tid)
{
    return thread.at(tid).getLoadHead();
}

InstSeqNum
LSQ::getLoadHeadSeqNum(ThreadID tid)
{
    return thread.at(tid).getLoadHeadSeqNum();
}

int
LSQ::getStoreHead(ThreadID tid)
{
    return thread.at(tid).getStoreHead();
}

InstSeqNum
LSQ::getStoreHeadSeqNum(ThreadID tid)
{
    return thread.at(tid).getStoreHeadSeqNum();
}

int LSQ::getCount(ThreadID tid) { return thread.at(tid).getCount(); }

int LSQ::numLoads(ThreadID tid) const { return thread.at(tid).numLoads(); }
int LSQ::numRAREntries(ThreadID tid) const { return thread.at(tid).numRAREntries(); }
int LSQ::numRAWEntries(ThreadID tid) const { return thread.at(tid).numRAWEntries(); }

int LSQ::anyInflightLoadsNotComplete()
{
    int l1miss = 0, l2miss = 0, l3miss = 0, any = 0;
    for (auto it : thread.at(0).inflightLoads) {
        if (it->isAnyOutstandingRequest()) {
            if (it->mainReq()->depth == 1) {
                l1miss = 1;
            }
            if (it->mainReq()->depth == 2) {
                l2miss = 1 << 1;
            }
            if (it->mainReq()->depth == 3) {
                l3miss = 1 << 2;
            }
            any = 1 << 3;
        }
    }
    return l1miss | l2miss | l3miss | any;
}

bool
LSQ::anyStoreNotExecute()
{
    for (auto& it : thread.at(0).storeQueue) {
        if (!it.instruction()->isIssued()) {
            return true;
        }
    }
    return false;
}

int LSQ::numStores(ThreadID tid) const { return thread.at(tid).numStores(); }

int
LSQ::numHtmStarts(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].numHtmStarts();
}
int
LSQ::numHtmStops(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].numHtmStops();
}

void
LSQ::resetHtmStartsStops(ThreadID tid)
{
    if (tid != InvalidThreadID)
        thread[tid].resetHtmStartsStops();
}

uint64_t
LSQ::getLatestHtmUid(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].getLatestHtmUid();
}

void
LSQ::setLastRetiredHtmUid(ThreadID tid, uint64_t htmUid)
{
    if (tid != InvalidThreadID)
        thread[tid].setLastRetiredHtmUid(htmUid);
}

void
LSQ::recvReqRetry()
{
    iewStage->cacheUnblocked();
    cacheBlocked(false);

    if (!retryReplayStoreBuffer()) {
        retryBlockedStoreBuffer();
    }

    for (ThreadID tid : *activeThreads) {
        thread[tid].recvRetry();
    }
}


bool
LSQ::recvTimingResp(PacketPtr pkt)
{
    if (pkt->isError())
        DPRINTF(LSQ, "Got error packet back for address: %#X\n",
                pkt->getAddr());

    LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
    panic_if(!request, "Got packet back with unknown sender state\n");

    thread[request->_port.lsqID].recvTimingResp(pkt);

    if (pkt->isInvalidate()) {
        // This response also contains an invalidate; e.g. this can be the case
        // if cmd is ReadRespWithInvalidate.
        //
        // The calling order between completeDataAccess and checkSnoop matters.
        // By calling checkSnoop after completeDataAccess, we ensure that the
        // fault set by checkSnoop is not lost. Calling writeback (more
        // specifically inst->completeAcc) in completeDataAccess overwrites
        // fault, and in case this instruction requires squashing (as
        // determined by checkSnoop), the ReExec fault set by checkSnoop would
        // be lost otherwise.

        DPRINTF(LSQ, "received invalidation with response for addr:%#x\n",
                pkt->getAddr());

        for (ThreadID tid = 0; tid < numThreads; tid++) {
            thread[tid].checkSnoop(pkt);
        }
    }

    if (request->isNormalLd() &&
        !request->instruction()->cacheHit()) {
        // if cache miss, the packet must be delete
        assert(request->isReleased());
        assert(request->_numOutstandingPackets == 1);
    }

    request->packetReplied();

    if (waitingForStaleTranslation) {
        checkStaleTranslations();
    }

    return true;
}

void
LSQ::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(LSQ, "received pkt for addr:%#x %s\n", pkt->getAddr(),
            pkt->cmdString());

    // must be a snoop
    if (pkt->isInvalidate()) {
        DPRINTF(LSQ, "received invalidation for addr:%#x\n",
                pkt->getAddr());
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            thread[tid].checkSnoop(pkt);
        }
    } else if (pkt->req && pkt->req->isTlbiExtSync()) {
        DPRINTF(LSQ, "received TLBI Ext Sync\n");
        assert(!waitingForStaleTranslation);

        waitingForStaleTranslation = true;
        staleTranslationWaitTxnId = pkt->req->getExtraData();

        for (auto& unit : thread) {
            unit.startStaleTranslationFlush();
        }

        // In case no units have pending ops, just go ahead
        checkStaleTranslations();
    }
}

void
LSQ::recvFunctionalCustomSignal(PacketPtr pkt, int sig)
{
    if (sig <= 0) {
        return;
    }
    DPRINTF(LSQ, "recvFunctionalCustomSignal: Resp type: %d\n", sig);

    LSQRequest *request = nullptr;
    if (sig != DcacheRespType::Bus_Clear) {
        // Bus_Clear event does not need request info
        request = dynamic_cast<LSQRequest*>(pkt->getPrimarySenderState());
        panic_if(!request, "Got packet back with unknown sender state\n");
    }

    if (sig == DcacheRespType::Miss || sig == DcacheRespType::Block_Not_Ready) {
        DPRINTF(LSQ, "[sn:%ld] CacheMiss: %d, BlockUnready: %d\n",
                request->instruction()->seqNum,
                sig == DcacheRespType::Miss,
                sig == DcacheRespType::Block_Not_Ready);
    } else if (sig == DcacheRespType::Hint) {
        // get cache miss load replay hint
        request->recvFunctionalCustomSignal(pkt);
    } else if (sig == DcacheRespType::Bus_Clear) {
        assert(pkt->cmd == MemCmd::CustomBusClear);
        // Data block is ready in Dcache, data on bus can be cleared now
        Addr busClearBlkAddr = pkt->getAddr();
        DPRINTF(Hint, "Bus Clear\n");
        DPRINTF(LSQ, "Bus_Clear, clear address: %#lx, bus size: %d\n", busClearBlkAddr, bus.size());
        for (auto it = bus.begin(); it != bus.end();) {
            auto [seqNum, addr] = *it;
            if ((addr & ~((uint64_t)cpu->cacheLineSize() - 1)) == busClearBlkAddr) {
                it = bus.erase(it);
                DPRINTF(LSQ, " erased bus: [sn:%ld] addr: %#lx\n", seqNum, addr);
            } else {
                it++;
            }
        }
        panic_if(bus.size() > getLQEntries(), "elements on bus should never be greater than LQ size");
    } else {
        panic("unsupported sig %d in recvFunctionalCustomSignal\n", sig);
    }
}

void*
LSQ::getCPUPtr() {
    return (void *) cpu;
}

int
LSQ::getCount()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += getCount(tid);
    }

    return total;
}
int
LSQ::numLoads() const
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += numLoads(tid);
    }

    return total;
}

int
LSQ::numRAREntries() const
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += numRAREntries(tid);
    }

    return total;
}

int
LSQ::numStores() const
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numStores();
    }

    return total;
}

int
LSQ::numRAWEntries() const
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += numRAWEntries(tid);
    }

    return total;
}

bool
LSQ::sharedLSQMode() const
{
    return lsqMode == SMTLSQMode::Shared;
}

unsigned
LSQ::activeLSQThreads() const
{
    if (!activeThreads || activeThreads->empty()) {
        return numThreads;
    }
    return activeThreads->size();
}

unsigned
LSQ::sharedLSQAllocation(unsigned entries) const
{
    const unsigned active_threads = std::max(1U, activeLSQThreads());

    switch (lsqPolicy) {
      case SMTQueuePolicy::Dynamic:
      case SMTQueuePolicy::DynamicBorrowing:
        return entries;
      case SMTQueuePolicy::Partitioned:
        return entries / active_threads;
      case SMTQueuePolicy::Threshold:
        return active_threads == 1 ? entries :
            std::min(entries, smtLSQThreshold);
      default:
        panic("Invalid LSQ sharing policy. Options are: Dynamic, "
              "Partitioned, Threshold, DynamicBorrowing");
    }
}

unsigned
LSQ::logicalMaxLoadEntries(ThreadID tid) const
{
    return sharedLSQMode() ? sharedLSQAllocation(LQEntries) : LQEntries;
}

unsigned
LSQ::logicalMaxStoreEntries(ThreadID tid) const
{
    return sharedLSQMode() ? sharedLSQAllocation(SQEntries) : SQEntries;
}

unsigned
LSQ::logicalMaxRAREntries(ThreadID tid) const
{
    return sharedLSQMode() ? sharedLSQAllocation(RARQEntries) : RARQEntries;
}

unsigned
LSQ::logicalMaxRAWEntries(ThreadID tid) const
{
    return sharedLSQMode() ? sharedLSQAllocation(RAWQEntries) : RAWQEntries;
}

unsigned
LSQ::logicalFreeLoadEntries(ThreadID tid) const
{
    const unsigned thread_free = std::max(0,
        static_cast<int>(logicalMaxLoadEntries(tid)) - thread[tid].numLoads());
    if (!sharedLSQMode()) {
        return thread_free;
    }

    const unsigned shared_used = numLoads();
    const unsigned shared_free = std::max(
        0, static_cast<int>(LQEntries) - static_cast<int>(shared_used));
    return std::min(thread_free, shared_free);
}

unsigned
LSQ::logicalFreeStoreEntries(ThreadID tid) const
{
    const unsigned thread_free = std::max(0,
        static_cast<int>(logicalMaxStoreEntries(tid)) - thread[tid].numStores());
    if (!sharedLSQMode()) {
        return thread_free;
    }

    const unsigned shared_used = numStores();
    const unsigned shared_free = std::max(
        0, static_cast<int>(SQEntries) - static_cast<int>(shared_used));
    return std::min(thread_free, shared_free);
}

unsigned
LSQ::logicalFreeRAREntries(ThreadID tid) const
{
    const unsigned thread_free = std::max(0,
        static_cast<int>(logicalMaxRAREntries(tid)) - numRAREntries(tid));
    if (!sharedLSQMode()) {
        return thread_free;
    }

    const unsigned shared_used = numRAREntries();
    const unsigned shared_free = std::max(
        0, static_cast<int>(RARQEntries) - static_cast<int>(shared_used));
    return std::min(thread_free, shared_free);
}

unsigned
LSQ::logicalFreeRAWEntries(ThreadID tid) const
{
    const unsigned thread_free = std::max(0,
        static_cast<int>(logicalMaxRAWEntries(tid)) - numRAWEntries(tid));
    if (!sharedLSQMode()) {
        return thread_free;
    }

    const unsigned shared_used = numRAWEntries();
    const unsigned shared_free = std::max(
        0, static_cast<int>(RAWQEntries) - static_cast<int>(shared_used));
    return std::min(thread_free, shared_free);
}

unsigned
LSQ::numFreeLoadEntries()
{
    if (sharedLSQMode()) {
        const unsigned used = numLoads();
        return used < LQEntries ? LQEntries - used : 0;
    }

    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numFreeLoadEntries();
    }

    return total;
}

unsigned
LSQ::numFreeStoreEntries()
{
    if (sharedLSQMode()) {
        const unsigned used = numStores();
        return used < SQEntries ? SQEntries - used : 0;
    }

    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numFreeStoreEntries();
    }

    return total;
}

unsigned
LSQ::numFreeLoadEntries(ThreadID tid)
{
    return logicalFreeLoadEntries(tid);
}

unsigned
LSQ::numFreeStoreEntries(ThreadID tid)
{
    return logicalFreeStoreEntries(tid);
}

bool
LSQ::isFull()
{
    if (sharedLSQMode()) {
        return lqFull() || sqFull();
    }

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!(thread[tid].lqFull() || thread[tid].sqFull()))
            return false;
    }

    return true;
}

bool
LSQ::isFull(ThreadID tid)
{
    if (sharedLSQMode()) {
        return logicalFreeLoadEntries(tid) == 0 ||
               logicalFreeStoreEntries(tid) == 0;
    }

    return thread[tid].lqFull() || thread[tid].sqFull();
}

bool
LSQ::isEmpty() const
{
    return lqEmpty() && sqEmpty();
}

bool
LSQ::lqEmpty() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].lqEmpty())
            return false;
    }

    return true;
}

bool
LSQ::lqEmpty(ThreadID tid) const
{
    return thread[tid].lqEmpty();
}

bool
LSQ::sqEmpty() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].sqEmpty())
            return false;
    }

    return true;
}

bool
LSQ::sqEmpty(ThreadID tid) const
{
    return thread[tid].sqEmpty();
}

bool
LSQ::lqFull()
{
    if (sharedLSQMode()) {
        return numFreeLoadEntries() == 0;
    }

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].lqFull())
            return false;
    }

    return true;
}

bool
LSQ::lqFull(ThreadID tid)
{
    if (sharedLSQMode()) {
        return logicalFreeLoadEntries(tid) == 0;
    }

    return thread[tid].lqFull();
}

bool
LSQ::sqFull()
{
    if (sharedLSQMode()) {
        return numFreeStoreEntries() == 0;
    }

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!sqFull(tid))
            return false;
    }

    return true;
}

bool
LSQ::sqFull(ThreadID tid)
{
    if (sharedLSQMode()) {
        return logicalFreeStoreEntries(tid) == 0;
    }

    return thread[tid].sqFull();
}

const DynInstPtr&
LSQ::getLSQHeadInst(ThreadID tid, bool isLoad)
{
    if (isLoad) {
        assert(!thread[tid].loadQueue.empty());
        return thread[tid].loadQueue.front().instruction();
    } else {
        assert(!thread[tid].storeQueue.empty());
        return thread[tid].storeQueue.front().instruction();
    }
}

int
LSQ::getLoadPFSource(const DynInstPtr &inst) const
{
    if (!inst || !inst->isLoad() || inst->lqIdx < 0) {
        return -1;
    }

    const auto &entry = thread[inst->threadNumber].loadQueue[inst->lqIdx];
    auto *request = entry.request();
    if (!request) {
        return -1;
    }

    // A load can retire through a split request or after replay/discard has
    // detached some request state. Prefetch source is best-effort metadata, so
    // only query a live sub-request when one still exists.
    if (request->numReqs() == 0) {
        return -1;
    }

    return request->req()->getPFSource();
}

bool
LSQ::isStalled()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].isStalled())
            return false;
    }

    return true;
}

bool
LSQ::isStalled(ThreadID tid)
{
    if (lsqPolicy == SMTQueuePolicy::Dynamic ||
        lsqPolicy == SMTQueuePolicy::DynamicBorrowing)
        return isStalled();
    else
        return thread[tid].isStalled();
}

bool
LSQ::hasStoresToWB()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (hasStoresToWB(tid))
            return true;
    }

    return false;
}

bool
LSQ::hasStoresToWB(ThreadID tid)
{
    return thread.at(tid).hasStoresToWB();
}

bool
LSQ::hasStoresToWBBefore(ThreadID tid, InstSeqNum seq_num)
{
    return thread.at(tid).hasStoresToWBBefore(seq_num);
}

bool
LSQ::flushStores(ThreadID tid)
{
    _storeBufferFlushing[tid] = true;
    _storeBufferFlushBeforeSeq[tid] = static_cast<InstSeqNum>(-1);
    const bool has_stores = hasStoresToWB(tid);
    const bool sbuffer_empty =
        storeBufferEmpty(tid, _storeBufferFlushBeforeSeq[tid]);
    if (!has_stores && sbuffer_empty) {
        clearStoreBufferFlushing(tid);
        return true;
    }

    return false;
}

bool
LSQ::flushStores(ThreadID tid, InstSeqNum seq_num)
{
    _storeBufferFlushing[tid] = true;
    _storeBufferFlushBeforeSeq[tid] = seq_num;
    const bool has_older_stores = hasStoresToWBBefore(tid, seq_num);
    const bool sbuffer_empty = storeBufferEmpty(tid, seq_num);
    if (!has_older_stores && sbuffer_empty) {
        clearStoreBufferFlushing(tid);
        return true;
    }

    return false;
}

LSQ::StoreBufferEntry *
LSQ::findForwardingStoreBufferEntry(Addr block_paddr, ThreadID load_tid,
                                    InstSeqNum load_seq) const
{
    auto entry = storeBuffer.get(load_tid, block_paddr);
    if (!entry) {
        return nullptr;
    }

    if (entry->seqNum < load_seq ||
        (entry->vice && entry->vice->seqNum < load_seq)) {
        return entry;
    }

    return nullptr;
}

void
LSQ::notifyOtherThreadsStoreVisible(ThreadID tid, Addr store_paddr,
                                    const std::vector<bool> &byte_enable)
{
    if (numThreads <= 1) {
        return;
    }

    Request::Flags flags;
    const Addr cache_block_mask =
        ~((static_cast<Addr>(cpu->cacheLineSize())) - 1);
    RequestPtr req = std::make_shared<Request>(
        store_paddr & cache_block_mask, cpu->cacheLineSize(), flags,
        cpu->dataRequestorId());
    Packet pkt(req, MemCmd::InvalidateReq);

    for (ThreadID context_id = 0; context_id < numThreads; ++context_id) {
        gem5::ThreadContext *tc = cpu->getContext(context_id);
        bool no_squash = cpu->thread[context_id]->noSquashFromTC;
        cpu->thread[context_id]->noSquashFromTC = true;
        tc->getIsaPtr()->handleLockedSnoop(&pkt, cache_block_mask);
        cpu->thread[context_id]->noSquashFromTC = no_squash;
    }

    for (ThreadID other_tid = 0; other_tid < numThreads; ++other_tid) {
        if (other_tid == tid) {
            continue;
        }
        thread[other_tid].checkLocalStoreVisible(store_paddr, byte_enable);
    }
}

int
LSQ::numStoresToSbuffer(ThreadID tid)
{
    return thread.at(tid).numStoresToSbuffer();
}

bool
LSQ::willWB()
{
    if (!dcacheMainPipeRefillQ.empty()) {
        return true;
    }
    for (const auto &stage : dcacheMainPipe) {
        if (stage.valid) {
            return true;
        }
    }

    if (!sbufferMainPipeReplayQ.empty() && !cacheBlocked()) {
        return true;
    }

    if (blockedSbufferEntry && !cacheBlocked()) {
        return true;
    }

    if (storeBufferFlushing()) {
        return true;
    }

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (willWB(tid))
            return true;
    }

    return false;
}

bool
LSQ::willWB(ThreadID tid)
{
    return thread.at(tid).willWB();
}

void
LSQ::dumpInsts() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        thread[tid].dumpInsts();
    }
}

void
LSQ::dumpInsts(ThreadID tid) const
{
    thread.at(tid).dumpInsts();
}

void
LSQ::dumpStoreBufferState(ThreadID tid, InstSeqNum seq_num) const
{
    cprintf("Store buffer state for tid %i:\n", tid);
    cprintf("  flushing=%d flushBeforeSeq=%llu\n",
            _storeBufferFlushing[tid],
            static_cast<unsigned long long>(_storeBufferFlushBeforeSeq[tid]));
    cprintf("  storesToWB=%d hasStoresToWBBefore=%d\n",
            thread.at(tid).numStoresToSbuffer(),
            thread.at(tid).hasStoresToWBBefore(seq_num));
    cprintf("  sbufferSize(tid)=%llu sbufferSizeBeforeSeq=%llu\n",
            static_cast<unsigned long long>(storeBuffer.size(tid)),
            static_cast<unsigned long long>(storeBuffer.size(tid, seq_num)));
}

void
LSQ::dumpStoreBuffer(ThreadID tid) const
{
    cprintf("Store buffer entries for tid %i:\n", tid);
    const auto &entries = storeBuffer.entries();
    for (size_t index = 0; index < entries.size(); ++index) {
        if (!storeBuffer.valid(index)) {
            continue;
        }

        auto *entry = entries[index];
        if (!entry || entry->tid != tid) {
            continue;
        }

        cprintf("  idx:%d seq:%llu paddr:%#lx vaddr:%#lx sending=%d vice=%d request=%p\n",
                entry->index,
                static_cast<unsigned long long>(entry->seqNum),
                entry->blockPaddr,
                entry->blockVaddr,
                entry->sending,
                entry->vice != nullptr,
                entry->request);
    }
}

bool
LSQ::isMisaligned(const DynInstPtr& inst, Addr vaddr, int size)
{
    auto code = inst->isLoad() ? RiscvISA::ExceptionCode::LOAD_ADDR_MISALIGNED
                                              : RiscvISA::ExceptionCode::STORE_ADDR_MISALIGNED;
    if (!inst->isVector() && size > 1 &&
        vaddr % size != 0) {
        if (inst->staticInst->isFusion()) {
            auto fusedInst = dynamic_cast<RiscvISA::FusionInst*>(inst->staticInst.get());
            if (fusedInst->correctMisalign(vaddr)) {
                return false;
            }
        }
        DPRINTF(LSQUnit, "[sn:%lld] misaligned: size: %u, Addr: %#lx, code: %d\n",
                inst->seqNum, size, vaddr, code);
        inst->getFault() = std::make_shared<RiscvISA::AddressFault>(vaddr, 0, code);
        return true;
    }
    return false;
}

Fault
LSQ::pushRequest(const DynInstPtr& inst, bool isLoad, uint8_t *data,
        unsigned int size, Addr addr, Request::Flags flags, uint64_t *res,
        AtomicOpFunctorPtr amo_op, const std::vector<bool>& byte_enable)
{
    // This comming request can be either load, store or atomic.
    // Atomic request has a corresponding pointer to its atomic memory
    // operation
    [[maybe_unused]] bool isAtomic = !isLoad && amo_op;

    if (isMisaligned(inst, addr, size)) {
        // inst->getFault() is set in isMisaligned()
        return inst->getFault();
    }

    ThreadID tid = cpu->contextToThread(inst->contextId());
    auto cacheLineSize = cpu->cacheLineSize();
    bool needs_burst = transferNeedsBurst(addr, size, cacheLineSize);
    LSQRequest* request = nullptr;

    // Atomic requests that access data across cache line boundary are
    // currently not allowed since the cache does not guarantee corresponding
    // atomic memory operations to be executed atomically across a cache line.
    // For ISAs such as x86 that supports cross-cache-line atomic instructions,
    // the cache needs to be modified to perform atomic update to both cache
    // lines. For now, such cross-line update is not supported.
    assert(!isAtomic || (isAtomic && !needs_burst));

    const bool htm_cmd = isLoad && (flags & Request::HTM_CMD);
    const bool tlbi_cmd = isLoad && (flags & Request::TLBI_CMD);

    if (inst->translationStarted()) {
        request = inst->savedRequest;
        assert(request);
    } else {
        if (htm_cmd || tlbi_cmd) {
            assert(addr == 0x0lu);
            assert(size == 8);
            request = new UnsquashableDirectRequest(&thread[tid], inst, flags);
        } else if (needs_burst) {
            request = new SplitDataRequest(&thread[tid], inst, isLoad, addr, size, flags, data, res);
        } else {
            request = new SingleDataRequest(&thread[tid], inst, isLoad, addr, size, flags, data, res,
                                            std::move(amo_op));
        }
        assert(request);
        request->_byteEnable = byte_enable;
        inst->setRequest();
        request->taskId(cpu->taskId());

        // There might be fault from a previous execution attempt if this is
        // a strictly ordered load
        inst->getFault() = NoFault;

        request->initiateTranslation();
    }


    if (!isLoad && !isAtomic) {
        // store inst temporally saves its data in memData
        inst->memData = new uint8_t[size];
        memcpy(inst->memData, data, size);
    }

    /* This is the place were instructions get the effAddr. */
    /* Only atomic types can attempt to send requests to the cache at this stage.*/
    if (request->isTranslationComplete()) {
        if (request->isMemAccessRequired()) {
            inst->effAddr = request->getVaddr();
            inst->effSize = size;
            inst->effAddrValid(true);

            if (cpu->checker) {
                inst->reqToVerify = std::make_shared<Request>(*request->req());
            }

            if (inst->isAtomic()) {
                Fault fault;
                if (isLoad)
                    fault = read(request, inst->lqIdx);
                else
                    fault = write(request, data, inst->sqIdx);
                // inst->getFault() may have the first-fault of a
                // multi-access split request at this point.
                // Overwrite that only if we got another type of fault
                // (e.g. re-exec).
                if (fault != NoFault)
                    inst->getFault() = fault;
            }
        } else if (isLoad) {
            inst->setMemAccPredicate(false);
            // Commit will have to clean up whatever happened.  Set this
            // instruction as executed.
            inst->setExecuted();
        }
    }
    DPRINTF(LSQ, "[sn:%llu] isTranslationComplete %d, isMemAccessRequired %d, falut %d\n",
        inst->seqNum, request->isTranslationComplete(), request->isMemAccessRequired(), inst->faulted());

    if (inst->traceData)
        inst->traceData->setMem(addr, size, flags);

    return inst->getFault();
}

LSQ::SingleDataRequest::SingleDataRequest(
    LSQUnit* port, const DynInstPtr& inst,
    bool isLoad, const Addr& addr, const uint32_t& size,
    const Request::Flags& flags_, PacketDataPtr data,
    uint64_t* res, AtomicOpFunctorPtr amo_op) :
    LSQRequest(port, inst, isLoad, addr, size, flags_, data, res,
                std::move(amo_op)) {
    port->numSingleRequest++;
    singleList.push_back(this);
    assert(port->numSingleRequest <= 400);
}

LSQ::SingleDataRequest::~SingleDataRequest(){
    assert(_port.numSingleRequest > 0);
    _port.numSingleRequest--;
    singleList.remove(this);
}

void
LSQ::SingleDataRequest::finish(const Fault &fault, const RequestPtr &request,
        gem5::ThreadContext* tc, BaseMMU::Mode mode)
{
    _fault.push_back(fault);
    numInTranslationFragments = 0;
    numTranslatedFragments = 1;
    /* If the instruction has been squahsed, let the request know
     * as it may have to self-destruct. */
    _inst->translatedTick = curTick();
    if (_inst->isSquashed()) {
        squashTranslation();
    } else {
        _inst->strictlyOrdered(request->isStrictlyOrdered());

        flags.set(Flag::TranslationFinished);
        if (fault == NoFault) {
            _inst->physEffAddr = request->getPaddr();
            _inst->memReqFlags = request->getFlags();
            if (request->isCondSwap()) {
                assert(_res);
                request->setExtraData(*_res);
            }
            setState(State::Request);
        } else {
            setState(State::Fault);
        }

        LSQRequest::_inst->fault = fault;
        LSQRequest::_inst->translationCompleted(true);
        DPRINTF(LSQ, "Translation of inst %llu notified as %s\n",
                LSQRequest::_inst->seqNum, fault == NoFault ? "successful" : "faulty");
    }
}

LSQ::SplitDataRequest::SplitDataRequest(LSQUnit* port, const DynInstPtr& inst, bool isLoad, const Addr& addr,
                                        const uint32_t& size, const Request::Flags& flags_, PacketDataPtr data,
                                        uint64_t* res)
    : LSQRequest(port, inst, isLoad, addr, size, flags_, data, res, nullptr),
      numFragments(0),
      numReceivedPackets(0),
      _mainReq(nullptr),
      _mainPacket(nullptr)
{
    port->numSplitRequest++;
    assert(port->numSplitRequest <= 400);
    flags.set(Flag::IsSplit);
}

LSQ::SplitDataRequest::~SplitDataRequest()
{
    assert(_port.numSplitRequest > 0);
    _port.numSplitRequest--;
    if (_mainReq) {
        _mainReq = nullptr;
    }
    if (_mainPacket) {
        delete _mainPacket;
        _mainPacket = nullptr;
    }
}

void
LSQ::SplitDataRequest::finish(const Fault &fault, const RequestPtr &req,
        gem5::ThreadContext* tc, BaseMMU::Mode mode)
{
    int i;
    for (i = 0; i < _reqs.size() && _reqs[i] != req; i++);
    assert(i < _reqs.size());
    _fault[i] = fault;

    numInTranslationFragments--;
    numTranslatedFragments++;

    if (fault == NoFault)
        _mainReq->setFlags(req->getFlags());

    if (numTranslatedFragments == _reqs.size()) {
        _inst->translatedTick = curTick();
        if (_inst->isSquashed()) {
            squashTranslation();
        } else {
            _inst->strictlyOrdered(_mainReq->isStrictlyOrdered());
            flags.set(Flag::TranslationFinished);
            _inst->translationCompleted(true);

            for (i = 0; i < _fault.size() && _fault[i] == NoFault; i++);
            if (i > 0) {
                _inst->physEffAddr = LSQRequest::req()->getPaddr();
                _inst->memReqFlags = _mainReq->getFlags();
                if (_mainReq->isCondSwap()) {
                    assert (i == _fault.size());
                    assert(_res);
                    _mainReq->setExtraData(*_res);
                }
                if (i == _fault.size()) {
                    _inst->fault = NoFault;
                    setState(State::Request);
                } else {
                  _inst->fault = _fault[i];
                  setState(State::PartialFault);
                }
            } else {
                _inst->fault = _fault[0];
                setState(State::Fault);
            }
        }

    }
}

void
LSQ::SingleDataRequest::initiateTranslation()
{
    assert(_reqs.size() == 0);

    addReq(_addr, _size, _byteEnable);

    _inst->xsMeta->instAddr = _inst->pcState().instAddr();

    if (_reqs.size() > 0) {
        _reqs.back()->setReqInstSeqNum(_inst->seqNum);
        _reqs.back()->setXsMetadata(Request::XsMetadata(_inst->xsMeta));
        _reqs.back()->taskId(_taskId);
        _inst->translationStarted(true);
        setState(State::Translation);
        flags.set(Flag::TranslationStarted);

        _inst->savedRequest = this;
        sendFragmentToTranslation(0);
    } else {
        _inst->setMemAccPredicate(false);
    }
}

PacketPtr
LSQ::SplitDataRequest::mainPacket()
{
    return _mainPacket;
}

RequestPtr
LSQ::SplitDataRequest::mainReq()
{
    return _mainReq;
}

RequestPtr
LSQ::SplitDataRequest::mainReq() const
{
    return _mainReq;
}

void
LSQ::SplitDataRequest::initiateTranslation()
{
    auto cacheLineSize = _port.cacheLineSize();
    Addr base_addr = _addr;
    Addr next_addr = addrBlockAlign(_addr + cacheLineSize, cacheLineSize);
    Addr final_addr = addrBlockAlign(_addr + _size, cacheLineSize);
    uint32_t size_so_far = 0;

    _mainReq = std::make_shared<Request>(base_addr,
                _size, _flags, _inst->requestorId(),
                _inst->pcState().instAddr(), _inst->contextId());
    _mainReq->setByteEnable(_byteEnable);

    _inst->xsMeta->instAddr = _inst->pcState().instAddr();

    // Paddr is not used in _mainReq. However, we will accumulate the flags
    // from the sub requests into _mainReq by calling setFlags() in finish().
    // setFlags() assumes that paddr is set so flip the paddr valid bit here to
    // avoid a potential assert in setFlags() when we call it from  finish().
    _mainReq->setPaddr(0);

    /* Get the pre-fix, possibly unaligned. */
    auto it_start = _byteEnable.begin();
    auto it_end = _byteEnable.begin() + (next_addr - base_addr);
    addReq(base_addr, next_addr - base_addr,
                     std::vector<bool>(it_start, it_end));
    size_so_far = next_addr - base_addr;

    /* We are block aligned now, reading whole blocks. */
    base_addr = next_addr;
    while (base_addr != final_addr) {
        auto it_start = _byteEnable.begin() + size_so_far;
        auto it_end = _byteEnable.begin() + size_so_far + cacheLineSize;
        addReq(base_addr, cacheLineSize,
                         std::vector<bool>(it_start, it_end));
        size_so_far += cacheLineSize;
        base_addr += cacheLineSize;
    }

    /* Deal with the tail. */
    if (size_so_far < _size) {
        auto it_start = _byteEnable.begin() + size_so_far;
        auto it_end = _byteEnable.end();
        addReq(base_addr, _size - size_so_far,
                         std::vector<bool>(it_start, it_end));
    }

    if (_reqs.size() > 0) {
        /* Setup the requests and send them to translation. */
        for (auto& r: _reqs) {
            r->setReqInstSeqNum(_inst->seqNum);
            r->setXsMetadata(Request::XsMetadata(_inst->xsMeta));
            r->taskId(_taskId);
        }

        _inst->translationStarted(true);
        setState(State::Translation);
        flags.set(Flag::TranslationStarted);
        _inst->savedRequest = this;
        numInTranslationFragments = 0;
        numTranslatedFragments = 0;
        _fault.resize(_reqs.size());

        for (uint32_t i = 0; i < _reqs.size(); i++) {
            sendFragmentToTranslation(i);
        }
    } else {
        _inst->setMemAccPredicate(false);
    }
}

LSQ::SbufferRequest::SbufferRequest(CPU* cpu, LSQUnit* port, Addr blockpaddr, uint8_t* data)
    : LSQRequest(port, nullptr, false, 0, port->cacheLineSize(), 0, data,
                 nullptr, nullptr, false),
      cpu(cpu) {
    lsq = port->getLsq();
    port->numSBufferRequest++;
    assert(port->numSBufferRequest <= port->getLsq()->getSbufferEntries());
}

LSQ::SbufferRequest::~SbufferRequest() {
    assert(_port.numSBufferRequest > 0);
    _port.numSBufferRequest--;
}

void
LSQ::SbufferRequest::addReq(Addr blockVaddr, Addr blockPaddr, const std::vector<bool> byteEnable)
{
    auto req = std::make_shared<Request>(
        blockPaddr, _port.cacheLineSize(), Request::Flags(),
        cpu->dataRequestorId());
    req->setContext(cpu->getContext(_port.lsqID)->contextId());
    req->setByteEnable(byteEnable);

    _reqs.push_back(req);
}

LSQ::LSQRequest::LSQRequest(
        LSQUnit *port, const DynInstPtr& inst, bool isLoad) :
    _state(State::NotIssued),
    _port(*port), _inst(inst), _data(nullptr),
    _res(nullptr), _addr(0), _size(0), _flags(0),
    _numOutstandingPackets(0), _amo_op(nullptr),
    _sbufferBypass(false)
{

    flags.set(Flag::IsLoad, isLoad);
    if (_inst) {
        flags.set(Flag::WriteBackToRegister,
                _inst->isStoreConditional() || _inst->isAtomic() ||
                _inst->isLoad());
        flags.set(Flag::IsAtomic, _inst->isAtomic());
        install();
    }
}

LSQ::LSQRequest::LSQRequest(
        LSQUnit *port, const DynInstPtr& inst, bool isLoad,
        const Addr& addr, const uint32_t& size, const Request::Flags& flags_,
        PacketDataPtr data, uint64_t* res, AtomicOpFunctorPtr amo_op,
        bool stale_translation)
    : _state(State::NotIssued),
    numTranslatedFragments(0),
    numInTranslationFragments(0),
    _port(*port), _inst(inst), _data(data),
    _res(res), _addr(addr), _size(size),
    _flags(flags_),
    _numOutstandingPackets(0),
    _amo_op(std::move(amo_op)),
    _hasStaleTranslation(stale_translation),
    _sbufferBypass(false)
{

    flags.set(Flag::IsLoad, isLoad);
    if (_inst) {
        flags.set(Flag::WriteBackToRegister,
                _inst->isStoreConditional() || _inst->isAtomic() ||
                _inst->isLoad());
        flags.set(Flag::IsAtomic, _inst->isAtomic());
        flags.set(Flag::IsHInst, _inst->isHInst());
        install();
    }

}

void
LSQ::LSQRequest::install()
{
    if (isLoad()) {
        _port.loadQueue[_inst->lqIdx].setRequest(this);
    } else {
        // Store, StoreConditional, and Atomic requests are pushed
        // to this storeQueue
        _port.storeQueue[_inst->sqIdx].setRequest(this);
    }
}

bool LSQ::LSQRequest::squashed() const { return _inst->isSquashed(); }

void
LSQ::LSQRequest::addReq(Addr addr, unsigned size,
           const std::vector<bool>& byte_enable)
{
    if (isAnyActiveElement(byte_enable.begin(), byte_enable.end())) {
        auto req = std::make_shared<Request>(
                addr, size, _flags, _inst->requestorId(),
                _inst->pcState().instAddr(), _inst->contextId(),
                std::move(_amo_op));
        req->setByteEnable(byte_enable);

        /* If the request is marked as NO_ACCESS, setup a local access */
        if (_flags.isSet(Request::NO_ACCESS)) {
            req->setLocalAccessor(
                [this, req](gem5::ThreadContext *tc, PacketPtr pkt) -> Cycles
                {
                    if ((req->isHTMStart() || req->isHTMCommit())) {
                        auto& inst = this->instruction();
                        assert(inst->inHtmTransactionalState());
                        pkt->setHtmTransactional(
                            inst->getHtmTransactionUid());
                    }
                    return Cycles(1);
                }
            );
        }

        _reqs.push_back(req);
    }
}

void
LSQ::LSQRequest::forward()
{
    if (!isLoad() || !needWBToRegister()) return;
    DPRINTF(StoreBuffer, "sbuffer/storeQue forward data\n");
    for (auto& p : SBforwardPackets)
    {
        _sbufferBypass = true;
        _inst->memData[p.idx] = p.byte;
    }

    for (auto& p : SQforwardPackets) {
        _sbufferBypass = true;
        _inst->memData[p.idx] = p.byte;
    }
}

void
LSQ::LSQRequest::detachLSQEntry()
{
    if (!_inst) {
        return;
    }

    if (isLoad() && _inst->lqIdx >= 0 &&
        _port.loadQueue[_inst->lqIdx].request() == this) {
        DPRINTF(LSQ, "inst [sn:%llu] Detach LSQRequest from LQ entry\n",
                _inst->seqNum);
        _port.loadQueue[_inst->lqIdx].setRequest(nullptr);
    } else if ((isAtomic() || _inst->isStore()) && _inst->sqIdx >= 0 &&
               _port.storeQueue[_inst->sqIdx].request() == this) {
        DPRINTF(LSQ, "inst [sn:%llu] Detach LSQRequest from SQ entry\n",
                _inst->seqNum);
        _port.storeQueue[_inst->sqIdx].setRequest(nullptr);
    }
}

void
LSQ::LSQRequest::detachInflightLoad()
{
    if (!isLoad()) {
        return;
    }

    auto &inflight = _port.inflightLoads;
    auto it = std::find(inflight.begin(), inflight.end(), this);
    if (it != inflight.end()) {
        DPRINTF(LSQ, "inst [sn:%llu] Detach LSQRequest from inflightLoads\n",
                _inst ? _inst->seqNum : 0);
        inflight.erase(it);
    }
}

LSQ::LSQRequest::~LSQRequest()
{
    assert(!isAnyOutstandingRequest());
    detachLSQEntry();
    detachInflightLoad();
    if (_inst && _inst->savedRequest == this) {
        DPRINTF(LSQ, "inst [sn:%llu] Deleting LSQRequest, savedRequest\n", _inst->seqNum);
         _inst->savedRequest = nullptr;
    }

    for (auto r: _packets)
        delete r;
};

ContextID
LSQ::LSQRequest::contextId() const
{
    return _inst->contextId();
}

void
LSQ::LSQRequest::sendFragmentToTranslation(int i)
{
    numInTranslationFragments++;
    if (_inst->isHInst()){
        req(i)->setHInst(_inst->isHInst());
    }
    _port.getMMUPtr()->translateTiming(req(i), _inst->thread->getTC(),
            this, isLoad() ? BaseMMU::Read : BaseMMU::Write);
}

void
LSQ::SingleDataRequest::markAsStaleTranslation()
{
    // If this element has been translated and is currently being requested,
    // then it may be stale
    if ((!flags.isSet(Flag::Complete)) &&
        (!flags.isSet(Flag::Discarded)) &&
        (flags.isSet(Flag::TranslationStarted))) {
        _hasStaleTranslation = true;
    }

    DPRINTF(LSQ, "SingleDataRequest %d 0x%08x isBlocking:%d\n",
        (int)_state, (uint32_t)flags, _hasStaleTranslation);
}

void
LSQ::SplitDataRequest::markAsStaleTranslation()
{
    // If this element has been translated and is currently being requested,
    // then it may be stale
    if ((!flags.isSet(Flag::Complete)) &&
        (!flags.isSet(Flag::Discarded)) &&
        (flags.isSet(Flag::TranslationStarted))) {
        _hasStaleTranslation = true;
    }

    DPRINTF(LSQ, "SplitDataRequest %d 0x%08x isBlocking:%d\n",
        (int)_state, (uint32_t)flags, _hasStaleTranslation);
}

bool
LSQ::SbufferRequest::recvTimingResp(PacketPtr pkt)
{
    // Dump inst num, request addr, and packet addr
    DPRINTF(StoreBuffer,
            "Sbuffer Req::recvTimingResp: entry[%#x]\n",
            _packets[0]->getAddr());
    assert(_numOutstandingPackets == 1);
    flags.set(Flag::Complete);
    assert(pkt == _packets.front());
    lsq->completeSbufferEvict(pkt);
    discard();
    return true;
}

bool
LSQ::SingleDataRequest::recvTimingResp(PacketPtr pkt)
{
    LSQ* lsq = this->_port.getLsq();
    bool isNormalLd = this->isNormalLd();
    bool enableLdMissReplay = lsq->enableLdMissReplay();
    // All responses received in 1 cycle are cache hit.
    bool cacheHit = LSQRequest::_inst->getCpuPtr()->ticksToCycles(curTick() - pkt->sendTick) <= 1;
    // Dump inst num, request addr, and packet addr
    if (debug::LSQ) {
        uint64_t firstWord = 0;
        const size_t copySize =
            std::min<size_t>(pkt->getSize(), sizeof(firstWord));

        std::memcpy(
            &firstWord,
            pkt->getPtr<uint8_t>(),
            copySize);

        DPRINTF(LSQ,
                "Single Req::recvTimingResp: inst: %llu, pkt: %#lx, "
                "size: %u, isLoad: %d, isLLSC: %d, isUncache: %d, "
                "isCachehit: %d, firstData: %#llx\n",
                pkt->req->getReqInstSeqNum(),
                pkt->getAddr(),
                pkt->getSize(),
                isLoad(),
                mainReq()->isLLSC(),
                mainReq()->isUncacheable(),
                cacheHit,
                static_cast<unsigned long long>(firstWord));
    }

    if (isLoad()) {
        auto it = std::find(lsqUnit()->inflightLoads.begin(), lsqUnit()->inflightLoads.end(), this);
        if (it != lsqUnit()->inflightLoads.end()) {
            lsqUnit()->inflightLoads.erase(it);
        }
    }

    assert(_numOutstandingPackets == 1);
    if (enableLdMissReplay && isNormalLd) {
        DPRINTF(Hint, "[sn:%ld] Recv TimingResp\n", pkt->req->getReqInstSeqNum());
        if (cacheHit) {
            DPRINTF(LSQ, "[sn:%ld] %s hit\n", _inst->seqNum, "cache");
            // Cache hit, the subsequent processing will be carried out in s2.
            instruction()->setCacheHit();
        } else if (LSQRequest::_inst->waitingCacheRefill()) {
            // Missed Data is ready at lsq side data bus, wake up missed load in replay queue
            // Handle the missed early wake-up here.
            DPRINTF(LSQ, "[sn:%ld] waitingCacheRefill\n", pkt->req->getReqInstSeqNum());
            LSQRequest::_inst->waitingCacheRefill(false);
            discard();
        } else {
            DPRINTF(LSQ, "[sn:%ld] addToBus\n", _inst->seqNum);
            // Cache miss refill, make data stable on data bus
            lsq->bus[_inst->seqNum] = pkt->getAddr();
            _port.getStats()->busAppendTimes++;
            discard();
        }
    } else {
        // When enableLdMissReplay is false, the specific execution stage of
        // the instruction is unknown, so complete here.
        flags.set(Flag::Complete);
        assert(pkt == _packets.front());
        assert(pkt == mainPacket());
        assemblePackets();
        _hasStaleTranslation = false;
    }
    // Clear the pending cache request
    LSQRequest::_inst->hasPendingCacheReq(false);
    LSQRequest::_inst->pendingCacheReq = nullptr;
    return true;
}

bool
LSQ::SplitDataRequest::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(LSQ, "Spilt Req::recvTimingResp: inst: %llu, pkt: %#lx\n", pkt->req->getReqInstSeqNum(),
            pkt->getAddr());
    uint32_t pktIdx = 0;
    while (pktIdx < _packets.size() && pkt != _packets[pktIdx])
        pktIdx++;
    assert(pktIdx < _packets.size());
    numReceivedPackets++;
    if (numReceivedPackets == _packets.size()) {
        flags.set(Flag::Complete);
        assemblePackets();
        _hasStaleTranslation = false;
        LSQRequest::_inst->hasPendingCacheReq(false);
        LSQRequest::_inst->pendingCacheReq = nullptr;
    }
    return true;
}

void
LSQ::SbufferRequest::recvFunctionalCustomSignal(PacketPtr pkt) {}

void
LSQ::SingleDataRequest::recvFunctionalCustomSignal(PacketPtr pkt)
{
    LSQ* lsq = this->_port.getLsq();
    bool isNormalLd = this->isNormalLd();
    bool enableLdMissReplay = lsq->enableLdMissReplay();
    if (enableLdMissReplay && isNormalLd && LSQRequest::_inst->waitingCacheRefill()) {
        // Receive Custom Hint, wake up cache missed load earlier before recvTimingResp
        DPRINTF(LSQ, "SingleDataRequest::CustomResp: inst: %llu, pkt: %#lx\n", pkt->req->getReqInstSeqNum(),
            pkt->getAddr());
        DPRINTF(Hint, "[sn:%ld] Recv Hint\n", pkt->req->getReqInstSeqNum());
        LSQRequest::_inst->waitingCacheRefill(false);
    }
}

void
LSQ::SplitDataRequest::recvFunctionalCustomSignal(PacketPtr pkt) {}


void
LSQ::SingleDataRequest::assemblePackets()
{
    forward();
    _port.completeDataAccess(mainPacket());
}

void
LSQ::SplitDataRequest::assemblePackets()
{
    PacketPtr resp = isLoad()
        ? Packet::createRead(_mainReq)
        : Packet::createWrite(_mainReq);
    if (isLoad())
        resp->dataStatic(_inst->memData);
    else
        resp->dataStatic(_data);
    resp->senderState = this;
    forward();
    _port.completeDataAccess(resp);
    delete resp;
}

void
LSQ::SbufferRequest::buildPackets()
{
    if (_packets.size() == 0) {
        PacketPtr pkt = Packet::createWrite(_reqs[0]);
        pkt->dataStatic(_data);
        pkt->senderState = this;
        _packets.push_back(pkt);
    }
}

void
LSQ::SingleDataRequest::buildPackets()
{
    /* Retries do not create new packets. */
    if (_packets.size() == 0) {
        _packets.push_back(
                isLoad()
                    ?  Packet::createRead(req())
                    :  Packet::createWrite(req()));
        _packets.back()->dataStatic(_inst->memData);
        _packets.back()->senderState = this;
        DPRINTF(PacketSender, "Set packet %#lx senderState to %#lx\n", _packets.back(), this);

        // hardware transactional memory
        // If request originates in a transaction (not necessarily a HtmCmd),
        // then the packet should be marked as such.
        if (_inst->inHtmTransactionalState()) {
            _packets.back()->setHtmTransactional(
                _inst->getHtmTransactionUid());

            DPRINTF(HtmCpu,
              "HTM %s pc=0x%lx - vaddr=0x%lx - paddr=0x%lx - htmUid=%u\n",
              isLoad() ? "LD" : "ST",
              _inst->pcState().instAddr(),
              _packets.back()->req->hasVaddr() ?
                  _packets.back()->req->getVaddr() : 0lu,
              _packets.back()->getAddr(),
              _inst->getHtmTransactionUid());
        }
    }
    assert(_packets.size() == 1);
}

void
LSQ::SplitDataRequest::buildPackets()
{
    /* Extra data?? */
    Addr base_address = _addr;

    if (_packets.size() == 0) {
        /* New stuff */
        if (isLoad()) {
            _mainPacket = Packet::createRead(_mainReq);
            _mainPacket->dataStatic(_inst->memData);

            // hardware transactional memory
            // If request originates in a transaction,
            // packet should be marked as such
            if (_inst->inHtmTransactionalState()) {
                _mainPacket->setHtmTransactional(
                    _inst->getHtmTransactionUid());
                DPRINTF(HtmCpu,
                  "HTM LD.0 pc=0x%lx-vaddr=0x%lx-paddr=0x%lx-htmUid=%u\n",
                  _inst->pcState().instAddr(),
                  _mainPacket->req->hasVaddr() ?
                      _mainPacket->req->getVaddr() : 0lu,
                  _mainPacket->getAddr(),
                  _inst->getHtmTransactionUid());
            }
        }
        for (int i = 0; i < _reqs.size() && _fault[i] == NoFault; i++) {
            RequestPtr req = _reqs[i];
            PacketPtr pkt = isLoad() ? Packet::createRead(req)
                                     : Packet::createWrite(req);
            ptrdiff_t offset = req->getVaddr() - base_address;
            if (isLoad()) {
                pkt->dataStatic(_inst->memData + offset);
            } else {
                uint8_t* req_data = new uint8_t[req->getSize()];
                std::memcpy(req_data,
                        _inst->memData + offset,
                        req->getSize());
                pkt->dataDynamic(req_data);
            }
            pkt->senderState = this;
            _packets.push_back(pkt);

            // hardware transactional memory
            // If request originates in a transaction,
            // packet should be marked as such
            if (_inst->inHtmTransactionalState()) {
                _packets.back()->setHtmTransactional(
                    _inst->getHtmTransactionUid());
                DPRINTF(HtmCpu,
                  "HTM %s.%d pc=0x%lx-vaddr=0x%lx-paddr=0x%lx-htmUid=%u\n",
                  isLoad() ? "LD" : "ST",
                  i+1,
                  _inst->pcState().instAddr(),
                  _packets.back()->req->hasVaddr() ?
                      _packets.back()->req->getVaddr() : 0lu,
                  _packets.back()->getAddr(),
                  _inst->getHtmTransactionUid());
            }
        }
    }
    assert(_packets.size() > 0);
}

bool
LSQ::SbufferRequest::sendPacketToCache()
{
    assert(_numOutstandingPackets == 0);
    // This only admits the request into the fake DCache MainPipe. The real
    // classic-cache timing request is issued by the fake S2 callback.
    bool success = lsq->sbufferEnterDcacheMainPipe(_packets.at(0));
    DPRINTF(StoreBuffer,
            "Sbuffer Req::sendPacketToCache: entry[%#x] %s dcache "
            "mainpipe\n",
            _packets[0]->getAddr(), success ? "entered" : "blocked before");

    return success;
}

bool
LSQ::SingleDataRequest::sendPacketToCache()
{
    assert(_numOutstandingPackets == 0);
    bool bank_conflict = false;
    bool tag_read_fail = false;
    bool mshr_used = false;
    bool mshr_alias_fail = false;
    bool hit_in_write_buffer = false;
    bool success = lsqUnit()->trySendPacket(isLoad(), _packets.at(0), bank_conflict,
                                            tag_read_fail, mshr_used, mshr_alias_fail, hit_in_write_buffer);
    if (success) {
        _packets[0]->setLSQPtr(lsqUnit()->getLsq());
        if (isLoad()) {
            assert(lsqUnit()->inflightLoads.size() < lsqUnit()->numLoads() + 4);
            lsqUnit()->inflightLoads.emplace_back(this);
        }

        if (!bank_conflict) {
            _numOutstandingPackets = 1;
            LSQRequest::_inst->hasPendingCacheReq(true);
            LSQRequest::_inst->pendingCacheReq = this;
            DPRINTF(LSQ, "sendPacketToCache success [sn:%llu], pkt: %#lx\n",
                    _inst->seqNum, _packets[0]->getAddr());
        }
    }
    if (bank_conflict) {
        instruction()->setBankConflictReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setBankConflictReplay\n",
                _inst->seqNum);
    }
    if (mshr_used) {
        instruction()->setMshrArbFailReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setMshrArbFailReplay\n",
                _inst->seqNum);
    }
    if (mshr_alias_fail) {
        instruction()->setMshrAliasFailReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setMshrAliasReplay\n",
                _inst->seqNum);
    }
    if (hit_in_write_buffer) {
        instruction()->setHitInWriteBufferReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setHitInWriteBufferReplay\n",
                _inst->seqNum);
    }
    if (tag_read_fail) {
        DPRINTF(TagReadFail, "sendPacketToCache fails addr: %lx\n", _packets.at(0)->getAddr());
        lsqUnit()->tagReadFailReplaySchedule();
    }
    return success;
}

bool
LSQ::SplitDataRequest::sendPacketToCache()
{
    /* Try to send the packets. */
    bool bank_conflict = false;
    bool tag_read_fail = false;
    bool mshr_used = false;
    bool mshr_alias_fail = false;
    bool hit_in_write_buffer = false;
    while (numReceivedPackets + _numOutstandingPackets < _packets.size()) {
        const size_t pkt_idx =
            numReceivedPackets + _numOutstandingPackets;
        PacketPtr pkt = _packets.at(pkt_idx);

        bool success = lsqUnit()->trySendPacket(
            isLoad(), pkt,
            bank_conflict, tag_read_fail, mshr_used,
            mshr_alias_fail, hit_in_write_buffer);

        DPRINTF(LSQ,
                "Split send observe [sn:%llu] idx:%llu addr:%#lx "
                "success:%d bankConflict:%d tagReadFail:%d "
                "mshrUsed:%d mshrAliasFail:%d writeBuffer:%d "
                "received:%llu outstanding:%llu total:%llu\n",
                _inst->seqNum,
                static_cast<unsigned long long>(pkt_idx),
                pkt->getAddr(),
                success,
                bank_conflict,
                tag_read_fail,
                mshr_used,
                mshr_alias_fail,
                hit_in_write_buffer,
                static_cast<unsigned long long>(numReceivedPackets),
                static_cast<unsigned long long>(_numOutstandingPackets),
                static_cast<unsigned long long>(_packets.size()));

        if (success) {
            pkt->setLSQPtr(lsqUnit()->getLsq());
            _numOutstandingPackets++;
        } else {
            break;
        }
    }
    if (bank_conflict) {
        lsqUnit()->bankConflictReplaySchedule();
    }
    if (tag_read_fail) {
        lsqUnit()->tagReadFailReplaySchedule();
    }
    if (mshr_used) {
        instruction()->setMshrArbFailReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setMshrArbFailReplay\n",
                _inst->seqNum);
    }
    if (mshr_alias_fail){
        instruction()->setMshrAliasFailReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setMshrAliasFailReplay\n",
                _inst->seqNum);
    }
    if (hit_in_write_buffer) {
        instruction()->setHitInWriteBufferReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setHitInWriteBufferReplay\n",
                _inst->seqNum);
    }
    if (_numOutstandingPackets == _packets.size()) {
        LSQRequest::_inst->hasPendingCacheReq(true);
        LSQRequest::_inst->pendingCacheReq = this;
        return true;
    }
    return false;
}

Cycles
LSQ::SingleDataRequest::handleLocalAccess(
        gem5::ThreadContext *thread, PacketPtr pkt)
{
    return pkt->req->localAccessor(thread, pkt);
}

Cycles
LSQ::SplitDataRequest::handleLocalAccess(
        gem5::ThreadContext *thread, PacketPtr mainPkt)
{
    Cycles delay(0);
    unsigned offset = 0;

    for (auto r: _reqs) {
        PacketPtr pkt =
            new Packet(r, isLoad() ? MemCmd::ReadReq : MemCmd::WriteReq);
        pkt->dataStatic(mainPkt->getPtr<uint8_t>() + offset);
        Cycles d = r->localAccessor(thread, pkt);
        if (d > delay)
            delay = d;
        offset += r->getSize();
        delete pkt;
    }
    return delay;
}

bool
LSQ::SingleDataRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask)
{
    return ( (LSQRequest::_reqs[0]->getPaddr() & blockMask) == blockAddr);
}

/**
 * Caches may probe into the load-store queue to enforce memory ordering
 * guarantees. This method supports probes by providing a mechanism to compare
 * snoop messages with requests tracked by the load-store queue.
 *
 * Consistency models must enforce ordering constraints. TSO, for instance,
 * must prevent memory reorderings except stores which are reordered after
 * loads. The reordering restrictions negatively impact performance by
 * cutting down on memory level parallelism. However, the core can regain
 * performance by generating speculative loads. Speculative loads may issue
 * without affecting correctness if precautions are taken to handle invalid
 * memory orders. The load queue must squash under memory model violations.
 * Memory model violations may occur when block ownership is granted to
 * another core or the block cannot be accurately monitored by the load queue.
 */
bool
LSQ::SplitDataRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask)
{
    bool is_hit = false;
    for (auto &r: _reqs) {
       /**
        * The load-store queue handles partial faults which complicates this
        * method. Physical addresses must be compared between requests and
        * snoops. Some requests will not have a valid physical address, since
        * partial faults may have outstanding translations. Therefore, the
        * existence of a valid request address must be checked before
        * comparing block hits. We assume no pipeline squash is needed if a
        * valid request address does not exist.
        */
        if (r->hasPaddr() && (r->getPaddr() & blockMask) == blockAddr) {
            is_hit = true;
            break;
        }
    }
    return is_hit;
}

bool
LSQ::DcachePort::recvTimingResp(PacketPtr pkt)
{
    return lsq->recvTimingResp(pkt);
}

void
LSQ::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    for (ThreadID tid = 0; tid < cpu->numThreads; tid++) {
        if (cpu->getCpuAddrMonitor(tid)->doMonitor(pkt)) {
            cpu->wakeup(tid);
        }
    }
    lsq->recvTimingSnoopReq(pkt);
}

void
LSQ::DcachePort::recvFunctionalCustomSignal(PacketPtr pkt, int sig)
{
    lsq->recvFunctionalCustomSignal(pkt, sig);
}

void*
LSQ::DcachePort::recvGetCPUPtr()
{
    return (void *) (lsq->cpu);
}

void
LSQ::DcachePort::recvReqRetry()
{
    lsq->recvReqRetry();
}

LSQ::UnsquashableDirectRequest::UnsquashableDirectRequest(
    LSQUnit* port,
    const DynInstPtr& inst,
    const Request::Flags& flags_) :
    SingleDataRequest(port, inst, true, 0x0lu, 8, flags_,
        nullptr, nullptr, nullptr)
{
}

void
LSQ::UnsquashableDirectRequest::initiateTranslation()
{
    // Special commands are implemented as loads to avoid significant
    // changes to the cpu and memory interfaces
    // The virtual and physical address uses a dummy value of 0x00
    // Address translation does not really occur thus the code below

    assert(_reqs.size() == 0);

    addReq(_addr, _size, _byteEnable);

    _inst->xsMeta->instAddr = _inst->pcState().instAddr();

    if (_reqs.size() > 0) {
        _reqs.back()->setReqInstSeqNum(_inst->seqNum);
        _reqs.back()->setXsMetadata(Request::XsMetadata(_inst->xsMeta));
        _reqs.back()->taskId(_taskId);
        _reqs.back()->setPaddr(_addr);
        _reqs.back()->setInstCount(_inst->getCpuPtr()->totalInsts());

        _inst->strictlyOrdered(_reqs.back()->isStrictlyOrdered());
        _inst->fault = NoFault;
        _inst->physEffAddr = _reqs.back()->getPaddr();
        _inst->memReqFlags = _reqs.back()->getFlags();
        _inst->savedRequest = this;

        flags.set(Flag::TranslationStarted);
        flags.set(Flag::TranslationFinished);

        _inst->translationStarted(true);
        _inst->translationCompleted(true);

        setState(State::Request);
    } else {
        panic("unexpected behaviour in initiateTranslation()");
    }
}

void
LSQ::UnsquashableDirectRequest::markAsStaleTranslation()
{
    // HTM/TLBI operations do not translate,
    // so cannot have stale translations
    _hasStaleTranslation = false;
}

void
LSQ::UnsquashableDirectRequest::finish(const Fault &fault,
        const RequestPtr &req, gem5::ThreadContext* tc,
        BaseMMU::Mode mode)
{
    panic("unexpected behaviour - finish()");
}

void
LSQ::checkStaleTranslations()
{
    assert(waitingForStaleTranslation);

    DPRINTF(LSQ, "Checking pending TLBI sync\n");
    // Check if all thread queues are complete
    for (const auto& unit : thread) {
        if (unit.checkStaleTranslations())
            return;
    }
    DPRINTF(LSQ, "No threads have blocking TLBI sync\n");

    // All thread queues have committed their sync operations
    // => send a RubyRequest to the sequencer
    auto req = Request::createMemManagement(
        Request::TLBI_EXT_SYNC_COMP,
        cpu->dataRequestorId());
    req->setExtraData(staleTranslationWaitTxnId);
    PacketPtr pkt = Packet::createRead(req);

    // TODO - reserve some credit for these responses?
    if (!dcachePort.sendTimingReq(pkt)) {
        panic("Couldn't send TLBI_EXT_SYNC_COMP message");
    }

    waitingForStaleTranslation = false;
    staleTranslationWaitTxnId = 0;
}

Fault
LSQ::read(LSQRequest* request, ssize_t load_idx)
{
    assert(request->req()->contextId() == request->contextId());
    ThreadID tid = cpu->contextToThread(request->req()->contextId());

    return thread.at(tid).read(request, load_idx);
}

Fault
LSQ::write(LSQRequest* request, uint8_t *data, ssize_t store_idx)
{
    ThreadID tid = cpu->contextToThread(request->req()->contextId());

    return thread.at(tid).write(request, data, store_idx);
}

} // namespace o3
} // namespace gem5

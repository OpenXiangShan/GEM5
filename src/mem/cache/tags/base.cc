/*
 * Copyright (c) 2013,2016,2018-2019 ARM Limited
 * All rights reserved.
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
 * Copyright (c) 2003-2005 The Regents of The University of Michigan
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

/**
 * @file
 * Definitions of BaseTags.
 */

#include "mem/cache/tags/base.hh"

#include <cassert>

#include "base/types.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/base.hh"
#include "mem/request.hh"
#include "sim/core.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

#include <iostream>
#include <zlib.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <iomanip>
#include "sim/root.hh"

namespace gem5
{

BaseTags::BaseTags(const Params &p)
    : ClockedObject(p), blkSize(p.block_size), blkMask(blkSize - 1),
      size(p.size), lookupLatency(p.tag_latency),
      system(p.system), indexingPolicy(p.indexing_policy),
      warmupBound((p.warmup_percentage/100.0) * (p.size / p.block_size)),
      warmedUp(false), numBlocks(p.size / p.block_size),
      dataBlks(new uint8_t[p.size]), // Allocate data storage in one big chunk
      stats(*this)
{
    registerExitCallback([this]() { cleanupRefs(); });
}

ReplaceableEntry*
BaseTags::findBlockBySetAndWay(int set, int way) const
{
    return indexingPolicy->getEntry(set, way);
}

CacheBlk*
BaseTags::findBlock(Addr addr, bool is_secure) const
{
    // Extract block tag
    Addr tag = extractTag(addr);

    // Find possible entries that may contain the given address
    const std::vector<ReplaceableEntry*> entries =
        indexingPolicy->getPossibleEntries(addr);

    // Search for block
    for (const auto& location : entries) {
        CacheBlk* blk = static_cast<CacheBlk*>(location);
        int way = location->getWay();
        if (blk->matchTag(tag, is_secure)) {
            if ((blk->getWay() != way) && (blk->getWay() != DEFAULTWAYPRE))
                panic("Unexpected way %d\n", blk->getWay());
            blk->setHitWay(way);
            return blk;
        }
    }

    // Did not find block
    return nullptr;
}

void
BaseTags::insertBlock(const PacketPtr pkt, CacheBlk *blk)
{
    assert(!blk->isValid());

    // Previous block, if existed, has been removed, and now we have
    // to insert the new one

    // Deal with what we are bringing in
    RequestorID requestor_id = pkt->req->requestorId();
    assert(requestor_id < system->maxRequestors());
    stats.occupancies[requestor_id]++;

    // Insert block with tag, src requestor id and task id
    if (pkt->req->hasXsMetadata())
    {
        blk->insert(extractTag(pkt->getAddr()), pkt->isSecure(), requestor_id,
                    pkt->req->taskId(),
                    pkt->req->getXsMetadata());
    }
    else {
        blk->insert(extractTag(pkt->getAddr()), pkt->isSecure(), requestor_id,
                    pkt->req->taskId());
    }

    // Check if cache warm up is done
    if (!warmedUp && stats.tagsInUse.value() >= warmupBound) {
        warmedUp = true;
        stats.warmupTick = curTick();
    }

    // We only need to write into one tag and one data block.
    stats.tagAccesses += 1;
    stats.dataAccesses += 1;
}

void
BaseTags::moveBlock(CacheBlk *src_blk, CacheBlk *dest_blk)
{
    assert(!dest_blk->isValid());
    assert(src_blk->isValid());

    // Move src's contents to dest's
    *dest_blk = std::move(*src_blk);

    assert(dest_blk->isValid());
    assert(!src_blk->isValid());
}

Addr
BaseTags::extractTag(const Addr addr) const
{
    return indexingPolicy->extractTag(addr);
}

void
BaseTags::cleanupRefsVisitor(CacheBlk &blk)
{
    if (blk.isValid()) {
        stats.totalRefs += blk.getRefCount();
        ++stats.sampledRefs;
    }
}

void
BaseTags::cleanupRefs()
{
    forEachBlk([this](CacheBlk &blk) { cleanupRefsVisitor(blk); });
}

void
BaseTags::computeStatsVisitor(CacheBlk &blk)
{
    if (blk.isValid()) {
        const uint32_t task_id = blk.getTaskId();
        assert(task_id < context_switch_task_id::NumTaskId);
        stats.occupanciesTaskId[task_id]++;
        Tick age = blk.getAge();

        int age_index;
        if (age / sim_clock::as_int::us < 10) { // <10us
            age_index = 0;
        } else if (age / sim_clock::as_int::us < 100) { // <100us
            age_index = 1;
        } else if (age / sim_clock::as_int::ms < 1) { // <1ms
            age_index = 2;
        } else if (age / sim_clock::as_int::ms < 10) { // <10ms
            age_index = 3;
        } else
            age_index = 4; // >10ms

        stats.ageTaskId[task_id][age_index]++;
    }
}

void
BaseTags::computeStats()
{
    for (unsigned i = 0; i < context_switch_task_id::NumTaskId; ++i) {
        stats.occupanciesTaskId[i] = 0;
        for (unsigned j = 0; j < 5; ++j) {
            stats.ageTaskId[i][j] = 0;
        }
    }

    forEachBlk([this](CacheBlk &blk) { computeStatsVisitor(blk); });
}

std::string
BaseTags::print()
{
    std::string str;

    auto print_blk = [&str](CacheBlk &blk) {
        if (blk.isValid())
            str += csprintf("\tBlock: %s\n", blk.print());
    };
    forEachBlk(print_blk);

    if (str.empty())
        str = "no valid tags\n";

    return str;
}

BaseTags::BaseTagStats::BaseTagStats(BaseTags &_tags)
    : statistics::Group(&_tags),
    tags(_tags),

    ADD_STAT(tagsInUse, statistics::units::Rate<
                statistics::units::Tick, statistics::units::Count>::get(),
             "Average ticks per tags in use"),
    ADD_STAT(totalRefs, statistics::units::Count::get(),
             "Total number of references to valid blocks."),
    ADD_STAT(sampledRefs, statistics::units::Count::get(),
             "Sample count of references to valid blocks."),
    ADD_STAT(avgRefs, statistics::units::Rate<
                statistics::units::Count, statistics::units::Count>::get(),
             "Average number of references to valid blocks."),
    ADD_STAT(warmupTick, statistics::units::Tick::get(),
             "The tick when the warmup percentage was hit."),
    ADD_STAT(occupancies, statistics::units::Rate<
                statistics::units::Count, statistics::units::Tick>::get(),
             "Average occupied blocks per tick, per requestor"),
    ADD_STAT(avgOccs, statistics::units::Rate<
                statistics::units::Ratio, statistics::units::Tick>::get(),
             "Average percentage of cache occupancy"),
    ADD_STAT(occupanciesTaskId, statistics::units::Count::get(),
             "Occupied blocks per task id"),
    ADD_STAT(ageTaskId, statistics::units::Count::get(),
             "Occupied blocks per task id, per block age"),
    ADD_STAT(ratioOccsTaskId, statistics::units::Ratio::get(),
             "Ratio of occupied blocks and all blocks, per task id"),
    ADD_STAT(tagAccesses, statistics::units::Count::get(),
             "Number of tag accesses"),
    ADD_STAT(dataAccesses, statistics::units::Count::get(),
             "Number of data accesses")
{
}

void
BaseTags::BaseTagStats::regStats()
{
    using namespace statistics;

    statistics::Group::regStats();

    System *system = tags.system;

    avgRefs = totalRefs / sampledRefs;

    occupancies
        .init(system->maxRequestors())
        .flags(nozero | nonan)
        ;
    for (int i = 0; i < system->maxRequestors(); i++) {
        occupancies.subname(i, system->getRequestorName(i));
    }

    avgOccs.flags(nozero | total);
    for (int i = 0; i < system->maxRequestors(); i++) {
        avgOccs.subname(i, system->getRequestorName(i));
    }

    avgOccs = occupancies / statistics::constant(tags.numBlocks);

    occupanciesTaskId
        .init(context_switch_task_id::NumTaskId)
        .flags(nozero | nonan)
        ;

    ageTaskId
        .init(context_switch_task_id::NumTaskId, 5)
        .flags(nozero | nonan)
        ;

    ratioOccsTaskId.flags(nozero);

    ratioOccsTaskId = occupanciesTaskId / statistics::constant(tags.numBlocks);
}

void
BaseTags::BaseTagStats::preDumpStats()
{
    statistics::Group::preDumpStats();

    tags.computeStats();
}


// restore L3 cache microarchitecture states based on memtrace
void
BaseTags::warmupState(const std::string &pmem_file,const std::string &memtrace_file)
{
     std::ifstream file(memtrace_file);         // the file contains the microarchitecture states from memtrace
     if(!file.is_open())
     {
        std::cout << "File open failed:" << memtrace_file << std::endl;
     };
     std::string line;
    std::string taskid;
    std::string requestorid;
    std::string rank;
     int line_max = this->size / this->blkSize; //compute the number of cache lines
     volatile int offset_num=0;
     int num = this->blkMask;
     while(num)                                 //compute tag+set bits
     {
        offset_num += num & 1;
        num >>= 1;
     }

     int total_num =64;                            //compute set bits

     int assoc = this->indexingPolicy->getAssoc();

     num = size/assoc;
     num = num/blkSize;
     volatile int set_num=0;
     while(num)
     {
        num >>= 1;
        set_num++;
     }
     set_num--;
     volatile int tag_num= total_num-set_num-offset_num;

     std::vector<char> decompressed_data = decompress_gz_to_memory(pmem_file);
    for(int line_num=0;line_num<line_max;line_num++)
    {
        std::getline(file,line);
        std::getline(file,taskid);
        std::getline(file,requestorid);
        std::getline(file,rank);
        int memtrace_priority = std::stoi(rank);
        char myvalid = line[0];
        char myhit = line[1];
        if( myhit == '1')
        {
        std::string mytag = line.substr(3,tag_num); // paddr
        int myset = line_num / assoc; // paddr
        std::bitset<32> setbin(myset);
        std::string myset_str = setbin.to_string();

        myset_str=myset_str.substr(myset_str.size()-set_num,set_num);
        std::string myaddr = mytag+myset_str;
        myaddr.append(offset_num,'0');//paddr

        const Addr p_addr = std::stoull(myaddr, nullptr, 2);
        const Addr h_addr = p_addr - 0x100000000 + 0x80000000;// host
        Addr tag = std::stoull(mytag, nullptr, 2);
        uint32_t set = indexingPolicy->myextractSet(p_addr);
        const bool is_secure = false;

    std::size_t blk_size_bits = blkSize*8;


    // Find replacement victim
    std::vector<CacheBlk*> evict_blks;
    CacheBlk *victim = this->findVictim(p_addr, is_secure, blk_size_bits,
                                            evict_blks);
    this->updateRp(victim,memtrace_priority);//replacement policy state update
    victim->insert(tag, is_secure);
    victim->setSrcRequestorId_pub(static_cast<uint16_t>(std::stoul(requestorid)));
    victim->setTaskId_pub(static_cast<uint32_t>(std::stoul(taskid)));
    victim->setTickInserted_pub();
    victim->setCoherenceBits(CacheBlk::WritableBit);
    victim->setCoherenceBits(CacheBlk::ReadableBit);

    Addr offset = p_addr & Addr (blkSize - 1);
    unsigned size = this->blkSize;
    char result_buffer[size + 1];
    size_t bytes_read = query_in_memory(decompressed_data, h_addr, result_buffer, size);
    std::memcpy(victim->data + offset, result_buffer, size);
    victim->setWhenReady(curTick());
        }
        else if(myvalid != '1')
        {
            break;
        }
    }
    file.close();
}


std::vector<char>
BaseTags::decompress_gz_to_memory(const std::string& gz_path) {
    std::vector<char> decompressed_data;
    const size_t CHUNK_SIZE = 32 * 1024;
    std::vector<char> in_buffer(CHUNK_SIZE);
    std::vector<char> out_buffer(CHUNK_SIZE * 2);
    std::ifstream gz_file(gz_path, std::ios_base::binary);
    if (!gz_file.is_open()) {
        throw std::runtime_error("File open failed: " + gz_path);
    }
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int ret = inflateInit2(&strm, MAX_WBITS | 16);
    if (ret != Z_OK) {
        throw std::runtime_error("inflateInit2 falied: " + std::to_string(ret));
    }

    while (true) {
        gz_file.read(in_buffer.data(), in_buffer.size());
        std::streamsize bytes_read = gz_file.gcount();

        strm.avail_in = static_cast<uInt>(bytes_read);
        strm.next_in = reinterpret_cast<Bytef*>(in_buffer.data());

        if (strm.avail_in == 0 && strm.avail_out == 0) {
            break;
        }

        do {
            strm.avail_out = static_cast<uInt>(out_buffer.size());
            strm.next_out = reinterpret_cast<Bytef*>(out_buffer.data());

            ret = inflate(&strm, Z_NO_FLUSH);

            if (ret == Z_STREAM_ERROR) {
                inflateEnd(&strm);
                gz_file.close();
                throw std::runtime_error("inflate failed: " + std::to_string(ret));
            }

            size_t bytes_decompressed = out_buffer.size() - strm.avail_out;
            if (bytes_decompressed > 0) {
                decompressed_data.insert(decompressed_data.end(),
                                         out_buffer.begin(),
                                         out_buffer.begin() + bytes_decompressed);
            }

        } while (strm.avail_out == 0);

        if (bytes_read == 0) {
            break;
        }
    }

    if (ret != Z_STREAM_END) {
         inflateEnd(&strm);
         gz_file.close();
         throw std::runtime_error("Decompression did not complete normally, the file may be corrupted. zlib return code: " + std::to_string(ret));
    }

    inflateEnd(&strm);
    gz_file.close();
    return decompressed_data;
}
size_t
BaseTags::query_in_memory(const std::vector<char>& data,
                       uint64_t target_address,
                       char* result,
                       size_t max_result_length) {
    if (max_result_length == 0 || result == nullptr) {
        return 0;
    }

    if (target_address >= data.size()) {
        throw std::out_of_range("The destination address 0x "+ std::to_string(target_address) + " is out of the unzipped data range.");
    }

    size_t bytes_to_copy = std::min(max_result_length, data.size() - static_cast<size_t>(target_address));

    std::memcpy(result, data.data() + target_address, bytes_to_copy);

    return bytes_to_copy;
}

} // namespace gem5

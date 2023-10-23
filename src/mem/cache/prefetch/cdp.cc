/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

#include "mem/cache/prefetch/cdp.hh"

#include <queue>

#include "debug/CDPUseful.hh"
#include "debug/CDPdebug.hh"
#include "debug/CDPdepth.hh"
#include "debug/WorkerPref.hh"
#include "mem/cache/base.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CDP.hh"
#include "sim/byteswap.hh"

// similar to x[hi:lo] in verilog

namespace gem5
{

    GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
    namespace prefetch
    {
        CDP::CDP(const CDPParams &p)
            : Queued(p), depth_threshold(3),
                byteOrder(p.sys->getGuestByteOrder()),
                pfLRUFilter(128)
        {
            transfer_event = new EventFunctionWrapper([this](){
                transfer();
            },name(),false);
        }
        void
        CDP::calculatePrefetch(const PrefetchInfo &pfi,
                                  std::vector<AddrPriority> &addresses)
        {
            // This prefetcher requires a PC
            if (!pfi.hasPC()) {
                return;
            }
            bool is_secure = pfi.isSecure();
            Addr pc = pfi.getPC();
            Addr addr = pfi.getAddr();
            bool miss = pfi.isCacheMiss();
            int page_offset,vpn0,vpn1,vpn2;
            if (!miss&&pfi.getDataPtr()!=nullptr){
                // std::cout<<"HIT depth:"<<int(pfi.getXsMetadata().prefetchDepth)<<std::endl;
                DPRINTF(CDPdepth,"HIT Depth: %d\n",pfi.getXsMetadata().prefetchDepth);
                if (pfi.getXsMetadata().prefetchDepth==4||pfi.getXsMetadata().prefetchDepth==2){
                    uint64_t* test_addrs=pfi.getDataPtr();
                    std::queue<std::pair<CacheBlk*,Addr>> pt_blks;
                    std::vector<uint64_t> addrs;
                    switch (byteOrder) {
                    case ByteOrder::big:
                        for (int of=0;of<8;of++){
                            addrs.push_back(Addr(
                                betoh(*(uint64_t*)(test_addrs+of))));
                        }
                        break;

                    case ByteOrder::little:
                        for (int of=0;of<8;of++){
                            addrs.push_back(Addr(
                                letoh(*(uint64_t*)(test_addrs+of))));
                        }
                        break;

                    default:
                        panic("Illegal byte order in \
                            CDP::notifyFill(const PacketPtr &pkt)\n");
                    }
                    for (Addr pt_addr:scanPointer(addr,addrs)){
                        CacheBlk* blk=cache->findBlock(pt_addr, is_secure);
                        AddrPriority addrprio=AddrPriority(pt_addr, 1, PrefetchSourceType::CDP);
                        addrprio.depth=1;
                        if (blk==nullptr)
                            addresses.push_back(addrprio);
                        else
                            pt_blks.push(std::pair<CacheBlk*,Addr>(blk,pt_addr));
                    }
                    for (int d=2;d<4;d++){
                        if (pt_blks.empty()){
                            break;
                        }
                        int size=pt_blks.size();
                        for (int i=0;i<size;i++){
                            CacheBlk * blk=pt_blks.front().first;
                            addr=pt_blks.front().second;
                            pt_blks.pop();
                            addrs = std::vector<uint64_t>(blk->data,
                                blk->data + (blkSize / sizeof(uint64_t)));
                            for (Addr pt_addr:scanPointer(addr,addrs)){
                                CacheBlk* blk2=cache->findBlock(pt_addr, is_secure);
                                AddrPriority addrprio=AddrPriority(pt_addr, 1, PrefetchSourceType::CDP);
                                addrprio.depth=d;
                                if (blk2==nullptr)
                                    addresses.push_back(addrprio);
                                else
                                    pt_blks.push(std::pair<CacheBlk*,Addr>(blk2,pt_addr));
                            }

                        }
                    }

                }
            }
            else if (miss){
                DPRINTF(CDPUseful,"Miss addr: %#llx\n",addr);
                vpn2=BITS(addr, 38, 30);
                vpn1=BITS(addr, 29, 21);
                vpn0=BITS(addr, 20, 12);
                page_offset=BITS(addr, 11, 0);
                vpnTable.add(vpn2,vpn1);
                vpnTable.resetConfidence();
                DPRINTF(CDPdebug,
                    "Sv39,PC:#%llx ADDR:%#llx, vpn2:%#llx, \
                    vpn1:%#llx, vpn0:%#llx, page offset:%#llx\n"
                    ,pc ,addr, Addr(vpn2), Addr(vpn1), Addr(vpn0),
                    Addr(page_offset));
            }
            return;

        }

        void
        CDP::notifyFill(const PacketPtr &pkt)
        {

            float trueAccuracy =
                (prefetchStats.pfUseful.total()
                / (prefetchStats.pfIssued.total()
                    - prefetchStats.pfLate.total()));
            float coverage = prefetchStats.pfUseful.total() /
                (prefetchStats.pfUseful.total() +
                prefetchStats.demandMshrMisses.total());
            // if (trueAccuracy<0.09&&prefetchStats.pfIssued.total()>1000){
            //     depth_threshold=1;
            // }
            // else{
            //     depth_threshold=3;
            // }
            uint64_t test_addr = 0;

            std::vector<uint64_t> addrs;
            if (pkt->hasData()&&pkt->req->hasVaddr()){
                int pf_depth = cache->getHitBlkXsMetadata(pkt).prefetchDepth;
                uint64_t* test_addrs=(uint64_t*)pkt->getPtr<uint64_t>();
                switch (byteOrder) {
                case ByteOrder::big:
                    for (int of=0;of<8;of++){
                        addrs.push_back(Addr(
                            betoh(*(uint64_t*)(test_addrs+of))));
                    }
                    break;

                case ByteOrder::little:
                    for (int of=0;of<8;of++){
                        addrs.push_back(Addr(
                            letoh(*(uint64_t*)(test_addrs+of))));
                    }
                    break;

                default:
                    panic("Illegal byte order in \
                        CDP::notifyFill(const PacketPtr &pkt)\n");
                };

                for (int of=0;of<8;of++){
                    test_addr=addrs[of];
                    int align_bit = BITS(test_addr, 1, 0);
                    int filter_bit = BITS(test_addr, 5, 0);
                    int page_offset,vpn0,vpn1,vpn1_addr,
                        vpn2,vpn2_addr,check_bit;
                    check_bit=BITS(test_addr, 63, 39);
                    vpn2=BITS(test_addr, 38, 30);
                    vpn2_addr=BITS(pkt->req->getVaddr(), 38, 30);
                    vpn1=BITS(test_addr, 29, 21);
                    vpn1_addr=BITS(pkt->req->getVaddr(), 29, 21);
                    vpn0=BITS(test_addr, 20, 12);
                    page_offset=BITS(test_addr, 11, 0);
                    bool flag=true;
                    bool next_line_enbale=false;
                    if ((check_bit != 0) || (!vpnTable.search(vpn2,vpn1))||
                        (vpn0==0) || (align_bit != 0))
                        flag=false;
                    if (trueAccuracy<0.1&&filter_bit!=0){
                        flag=false;
                    }
                    if (trueAccuracy>0.2){
                        next_line_enbale=true;
                    }
                    Addr test_addr2=Addr(test_addr);
                    PrefetchInfo pfi(pkt, pkt->req->getVaddr(), false);
                    size_t max_pfs = getMaxPermittedPrefetches(8);
                    size_t num_pfs=0;
                    if (flag){
                        if (pf_depth>=depth_threshold){
                            return;
                        }
                        int next_depth=0;
                        if (pf_depth==0){
                            next_depth=4;
                            next_line_enbale=false;
                        }
                        else next_depth=pf_depth+1;
                        PrefetchInfo new_pfi(pfi, blockAddress(test_addr2));
                        AddrPriority addrprio=AddrPriority(blockAddress(test_addr2), 1, PrefetchSourceType::CDP);
                        addrprio.depth=next_depth;
                        insert(pkt, new_pfi,addrprio);
                        if ((test_addr2-blockAddress(test_addr2))>0x20){
                            next_line_enbale=true;
                        }
                        if (next_line_enbale){
                            addrprio=AddrPriority(blockAddress(test_addr2)+0x40, 1, PrefetchSourceType::CDP);
                            addrprio.depth=next_depth;
                            PrefetchInfo new_pfi2(pfi, blockAddress(test_addr2)+0x40);
                            insert(pkt, new_pfi2, addrprio);
                        }
                        num_pfs += 1;
                        if (num_pfs == max_pfs) {
                            break;
                        }
                    }
                }
                DPRINTF(CDPdepth,"Fill Depth: %d\n",pf_depth);
            }
            if (!first_call) {
                first_call = true;
                schedule(transfer_event, nextCycle());
            }
            return;

        }
        void
        CDP::rxHint(BaseMMU::Translation *dpp)
        {
            auto ptr = reinterpret_cast<DeferredPacket *>(dpp);

            // ignore if pfahead_host > itself level
            if ((ptr->pfahead ? (ptr->pfahead_host <= cache->level()) : true)
                && (ptr->pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::SStream)) {
                if (pfLRUFilter.contains(ptr->pfInfo.getAddr())) {
                    DPRINTF(WorkerPref, "Worker: offload: [%lx, %d] skip recently in localBuffer\n", ptr->pfInfo.getAddr(), ptr->pfahead_host);
                    return;
                }
                pfLRUFilter.insert(ptr->pfInfo.getAddr(),0);
            }

            DPRINTF(WorkerPref, "Worker: put [%lx, %d] into localBuffer(size:%lu)\n", ptr->pfInfo.getAddr(), ptr->pfahead_host,localBuffer.size());
            localBuffer.push_back(*ptr);
        }
        void
        CDP::transfer()
        {
            // ignore information of pfi, grab the information from the local buffer
            unsigned count = 0;
            auto dpp_it = localBuffer.begin();
            while (count < depth && !localBuffer.empty()) {
                if (queueFilter) {
                    if (alreadyInQueue(pfq, dpp_it->pfInfo.getAddr(), dpp_it->pfInfo.isSecure(), dpp_it->priority)) {
                        DPRINTF(WorkerPref, "Worker: [%lx, %d] was already in pfq\n", dpp_it->pfInfo.getAddr(), dpp_it->pfahead_host);
                    }
                    else if (alreadyInQueue(pfqMissingTranslation, dpp_it->pfInfo.getAddr(), dpp_it->pfInfo.isSecure(), dpp_it->priority)) {
                        DPRINTF(WorkerPref, "Worker: [%lx, %d] was already in pfq\n", dpp_it->pfInfo.getAddr(), dpp_it->pfahead_host);
                    }
                    else {
                        addToQueue(pfq, *dpp_it);
                        DPRINTF(WorkerPref, "Worker: put [%lx, %d] into local pfq\n", dpp_it->pfInfo.getAddr(), dpp_it->pfahead_host);
                    }
                }
                else {
                    addToQueue(pfq, *dpp_it);
                    DPRINTF(WorkerPref, "Worker: put [%lx, %d] into local pfq\n", dpp_it->pfInfo.getAddr(), dpp_it->pfahead_host);
                }
                dpp_it = localBuffer.erase(dpp_it);
                count++;
            }
            schedule(transfer_event,nextCycle());
        }

    } // namespace prefetch
} // namespace gem5

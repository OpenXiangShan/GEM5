/*
 * Copyright (c) 2014, 2019 ARM Limited
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

#include "mem/cache/prefetch/multi.hh"

#include "params/MultiPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

Multi::Multi(const MultiPrefetcherParams &p)
  : Base(p),
    prefetchers(p.prefetchers.begin(), p.prefetchers.end()),
    lastChosenPf(0),hasBeenChoose(false),firstChoose(0)
{
}

void
Multi::setCache(BaseCache *_cache)
{
    for (auto pf : prefetchers)
        pf->setCache(_cache);
}

Tick
Multi::nextPrefetchReadyTime() const
{
    Tick next_ready = MaxTick;

    for (auto pf : prefetchers)
        next_ready = std::min(next_ready, pf->nextPrefetchReadyTime());

    return next_ready;
}

PacketPtr
Multi::getPacket()
{
    lastChosenPf = (lastChosenPf + 1) % prefetchers.size();
    uint8_t pf_turn = lastChosenPf;
    uint64_t issuenum[prefetchers.size()];
    uint64_t UnusedRemovePre[prefetchers.size()];
    double precision[prefetchers.size()];
    // double precision[prefetchers.size()];
    for (int pf_c = 0; pf_c < prefetchers.size(); pf_c++)
    {
        issuenum[pf_c] = prefetchers[pf_c]->printPreIssuenum();
        // UnusedRemovePre[pf_c] =
        //  printf("pf_c %d %ld\n",pf_c,issuenum[pf_c]);
    }
    UnusedRemovePre[0] = Base::UnUsedRemovePreNum0();
    UnusedRemovePre[1] = Base::UnUsedRemovePreNum1();

    precision[0] =
            (double)(issuenum[0] - UnusedRemovePre[0]) / (issuenum[0] + 1);
    precision[1]
            = (double)(issuenum[1] - UnusedRemovePre[1]) / (issuenum[1] + 1);
    // printf("precision 0 %f 1 %f\n",precision[0],precision[1]);

    // if ((UnusedRemovePre[0]>10000)&&(UnusedRemovePre[1]>10000)){
    if (!hasBeenChoose)
    {
        if ((UnusedRemovePre[0] > 100) && (UnusedRemovePre[1] > 100))
        {
            // if ((UnusedRemovePre[0]>10000)&&(UnusedRemovePre[1]>10000)){
            if (precision[1] > precision[0])
            {
                firstChoose = 1;
            }
            else
            {
                firstChoose = 0;
            }
            hasBeenChoose = true;
            // printf("first choose %d\n",firstChoose);
        }
    }

    if (hasBeenChoose)
    {
        pf_turn = firstChoose;
        /*if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick()) {
            PacketPtr pkt = prefetchers[pf_turn]->getPacket();
            panic_if(!pkt, "Prefetcher is ready but didn't return a packet.");
            pkt->req->setpreNum(pf_turn);
            prefetchStats.pfIssued++;
            issuedPrefetches++;
            return pkt;

        }
        return nullptr;*/
        for (int pf_c = 0; pf_c < prefetchers.size(); pf_c++)
        {
            if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick())
            {
                PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                panic_if(!pkt,
                 "Prefetcher is ready but didn't return a packet.");
                pkt->req->setpreNum(pf_turn);
                prefetchStats.pfIssued++;
                issuedPrefetches++;
                // assert(0);
                return pkt;
            }
            pf_turn = (pf_turn + 1) % prefetchers.size();
        }
        return nullptr;
    }
    else
    {
        for (int pf = 0; pf < prefetchers.size(); pf++)
        {
            if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick())
            {
                PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                panic_if(!pkt,
                         "Prefetcher is ready but didn't return a packet.");
                pkt->req->setpreNum(pf_turn);
                prefetchStats.pfIssued++;
                issuedPrefetches++;
                return pkt;
            }
            pf_turn = (pf_turn + 1) % prefetchers.size();
        }
        return nullptr;
    }

    /*    for (int pf = 0 ;  pf < prefetchers.size(); pf++) {
            //issuenum[]
            //printf("nextPrefetchReadyTime0\n");
            if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick()) {
                Base::UnUsedRemovePreNum0();
                Base::UnUsedRemovePreNum1();
               // printf("after printf\n");
                printf("0 pf_turn %d unused %ld %ld\n",pf_turn,
                Base::UnUsedRemovePreNum0(),
                prefetchers[pf_turn]->Base::UnUsedRemovePreNum0());
                printf("1 pf_turn %d unused %ld %ld\n",pf_turn,
                Base::UnUsedRemovePreNum1(),
                prefetchers[pf_turn]->Base::UnUsedRemovePreNum1());
                PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                printf("2 pf_turn %d usefulPrefetches %ld\n",
                pf_turn,prefetchers[pf_turn]->printPrenum());
                printf("22 pf_turn %d usefulPrefetches %ld\n",
                pf_turn,printPrenum());
                printf("3 pf_turn %d issuedPrefetches %ld\n",
                pf_turn,prefetchers[pf_turn]->printPreIssuenum());
                printf("33 pf_turn %d issuedPrefetches %ld\n",
                pf_turn,printPreIssuenum());
                panic_if(!pkt,
                "Prefetcher is ready but didn't return a packet.");
                pkt->req->setpreNum(pf_turn);
                printf("4 pf_turn %d\n",pkt->req->printpreNum());
                prefetchStats.pfIssued++;
                issuedPrefetches++;
                return pkt;
            }
            pf_turn = (pf_turn + 1) % prefetchers.size();
        }

        //for (int)

        return nullptr;*/
}
void
Multi::addTLB(BaseTLB *_t)
{
    Base::addTLB(_t);
    for (auto pf : prefetchers)
        pf->addTLB(_t);
}

} // namespace prefetch
} // namespace gem5

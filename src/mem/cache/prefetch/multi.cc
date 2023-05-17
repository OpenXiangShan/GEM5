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
    for (int pf_f = 0; pf_f < prefetchers.size(); pf_f++){
        Base::addreg();
        lastremove.push_back(0);
    }

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
    uint64_t PreHitInCacheNum[prefetchers.size()];
    uint64_t PreHitInMshrNum[prefetchers.size()];
    uint64_t PreHitInWbNum[prefetchers.size()];
    uint64_t PreusefulNum[prefetchers.size()];
    uint64_t UnusedRemovePre[prefetchers.size()];

    double precision[prefetchers.size()];
    double recall[prefetchers.size()];
    double f1[prefetchers.size()];

    // double precision[prefetchers.size()];
    for (int pf_c = 0; pf_c < prefetchers.size(); pf_c++)
    {
        issuenum[pf_c] = prefetchers[pf_c]->printPreIssuenum();
        PreHitInCacheNum[pf_c] = Base::PreHitInCacheNum(pf_c);
        PreHitInMshrNum[pf_c] = Base::PreHitInMshrNum(pf_c);
        PreHitInWbNum[pf_c] = Base::PreHitInWbNum(pf_c);
        PreusefulNum[pf_c] = Base::Pre_UsefulNum(pf_c);
    }
    uint64_t demandMshrMissesNum = Base::Pre_demandMshrMissesNum();
    UnusedRemovePre[0] = Base::UnUsedRemovePreNum0();
    UnusedRemovePre[1] = Base::UnUsedRemovePreNum1();

    precision[0] = (double)(issuenum[0] - UnusedRemovePre[0] -
                            PreHitInCacheNum[0] - PreHitInMshrNum[0]) /
                   (issuenum[0] + 1);
    precision[1] = (double)(issuenum[1] - UnusedRemovePre[1] -
                            PreHitInCacheNum[1] - PreHitInMshrNum[1]) /
                   (issuenum[1] + 1);

    recall[0] =
        (double)(issuenum[0] - UnusedRemovePre[0] - PreHitInCacheNum[0] -
                 PreHitInMshrNum[0] - PreHitInWbNum[0]) /
        (PreusefulNum[0] + PreHitInCacheNum[0] + PreHitInMshrNum[0] +
         demandMshrMissesNum + 1);

    recall[1] =
        (double)(issuenum[1] - UnusedRemovePre[1] - PreHitInCacheNum[1] -
                 PreHitInMshrNum[1] - PreHitInWbNum[1]) /
        (PreusefulNum[1] + PreHitInCacheNum[1] + PreHitInMshrNum[1] +
         demandMshrMissesNum + 1);
    //printf("")

    f1[0] = 2*precision[0]*recall[0]/(precision[0]+recall[0]);
    f1[1] = 2*precision[1]*recall[1]/(precision[1]+recall[1]);


    int pf_clean;
    if (!hasBeenChoose){
        // if ((UnusedRemovePre[0] > 2000) && (UnusedRemovePre[1] > 2000)
        //&& (fabs(f1[1]-f1[0])>0.04)){
        if ((UnusedRemovePre[0] > 2000) && (UnusedRemovePre[1] > 2000) &&
            (fabs(precision[1] - precision[0]) > 0.04)) {
            //if (f1[1] > f1[0])
            if (precision[1] > precision[0])
            {
                firstChoose = 1;
            }
            else
            {
                firstChoose = 0;
            }
            hasBeenChoose = true;

            printf("f1 0 %f 1 %f\n", f1[0], f1[1]);
            lastremove[0] = UnusedRemovePre[0];
            lastremove[1] = UnusedRemovePre[1];
            for (pf_clean = 0; pf_clean < prefetchers.size(); pf_clean++)
            {
                Base::cleanMultiNum(pf_clean);
                prefetchers[pf_clean]->cleanPreIssuenum();
            }
            Base::cleanMultiDemand();
        }
    }
    else
    {
        if ((UnusedRemovePre[0] > 1000) && (UnusedRemovePre[1] > 1000)) {
            // if (fabs(f1[1]-f1[0])>0.02){
            //     if (f1[1] > f1[0]){
            if (fabs(precision[1] - precision[0]) > 0.02)
            {
                if (precision[1] > precision[0])
                {
                    firstChoose = 1;
                }
                else
                {
                    firstChoose = 0;
                }
                hasBeenChoose = true;
                // hasBeenChoose = false;
                lastremove[0] = UnusedRemovePre[0];
                lastremove[1] = UnusedRemovePre[1];
                for (pf_clean = 0; pf_clean < prefetchers.size(); pf_clean++)
                {
                    Base::cleanMultiNum(pf_clean);
                    prefetchers[pf_clean]->cleanPreIssuenum();
                }
                Base::cleanMultiDemand();
            }
            else
            {
                hasBeenChoose = false;
            }
        }
    }

    if (hasBeenChoose)
    {
        pf_turn = firstChoose;
        for (int pf_c = 0; pf_c < prefetchers.size(); pf_c++)
        {
            if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick())
            {
                PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                panic_if(!pkt,
                 "Prefetcher is ready but didn't return a packet.");
                pkt->req->setpreNum(pf_turn);
                prefetchStats.pfIssued++;
                if (pf_turn == 0)
                    prefetchStats.pfIssued0++;
                else if (pf_turn == 1)
                    prefetchStats.pfIssued1++;
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
                if (pf_turn == 0)
                    prefetchStats.pfIssued0++;
                else if (pf_turn == 1)
                    prefetchStats.pfIssued1++;
                issuedPrefetches++;
                return pkt;
            }
            pf_turn = (pf_turn + 1) % prefetchers.size();
        }
        return nullptr;
    }

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

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
      lastChosenPf(0),
      lastChosenPf_m(0),
      hasBeenChoose(false),
      firstChoose(0),
      prefetcher_size(3)
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
   // printf("get packet\n");
    //uint64_t addr111 = 0x80000000;
    //lookupCachePre(addr111);
    lastChosenPf = (lastChosenPf + 1) % prefetchers.size();
    uint8_t pf_turn = lastChosenPf;
    //uint8_t pf_turn;
    uint64_t issuenum[prefetchers.size()];
    uint64_t PreHitInCacheNum[prefetchers.size()];
    uint64_t PreHitInMshrNum[prefetchers.size()];
    uint64_t PreHitInWbNum[prefetchers.size()];
    uint64_t PreusefulNum[prefetchers.size()];
    uint64_t UnusedRemovePre[prefetchers.size()];
   // int choose_order[prefetchers.size()];

    double precision[prefetchers.size()];
    double recall[prefetchers.size()];
    double f1[prefetchers.size()];
    //int prefetcher_size = prefetchers.size();
    //printf("prefetcher_size = %d\n",prefetcher_size);
    //assert(0);
    uint64_t useful_blk = 0;

    // double precision[prefetchers.size()];
    for (int pf_c = 0; pf_c < prefetchers.size(); pf_c++)
    {
        issuenum[pf_c] = prefetchers[pf_c]->printPreIssuenum();
        PreHitInCacheNum[pf_c] = Base::PreHitInCacheNum(pf_c);
        PreHitInMshrNum[pf_c] = Base::PreHitInMshrNum(pf_c);
        PreHitInWbNum[pf_c] = Base::PreHitInWbNum(pf_c);
        PreusefulNum[pf_c] = Base::Pre_UsefulNum(pf_c);
        UnusedRemovePre[pf_c] = Base::UnUsedRemovePreNum(pf_c);
        useful_blk = PreusefulNum[pf_c] + useful_blk;
        /*  printf("prefetcher %d insert_num
          %ld\n",pf_c,prefetchers[pf_c]->count_insert_num); printf("prefetcher
          %d preNoUse %ld\n",pf_c,prefetchers[pf_c]->PreNoUse);
          printf("prefetcher %d usefulPreNum
          %ld\n",pf_c,prefetchers[pf_c]->usefulPreNum);*/
        // printf("printPreIss %d
        // %ld\n",pf_c,prefetchers[pf_c]->printPreIssuenum());

        /* issuenum[pf_c] = prefetchers[pf_c]->count_insert_num;
         PreusefulNum[pf_c] = prefetchers[pf_c]->usefulPreNum;
         UnusedRemovePre[pf_c] = prefetchers[pf_c]->PreNoUse;
         useful_blk = PreusefulNum[pf_c] + useful_blk;*/
    }
    uint64_t demandMshrMissesNum = Base::Pre_demandMshrMissesNum();
   // UnusedRemovePre[0] = Base::UnUsedRemovePreNum0();
   // UnusedRemovePre[1] = Base::UnUsedRemovePreNum1();

   /* precision[0] = (double)(issuenum[0] - UnusedRemovePre[0] -
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
         demandMshrMissesNum + 1);*/
    //printf("")
    double all_f1 = 0;
    bool unusedNum = true;



    for (int pf_n = 0; pf_n < prefetchers.size(); pf_n++) {
        // precision[pf_n] =
        precision[pf_n] =
            (double)(issuenum[pf_n] - UnusedRemovePre[pf_n] -
                     PreHitInCacheNum[pf_n] - PreHitInMshrNum[pf_n] -
                     PreHitInWbNum[pf_n]) /
            (issuenum[pf_n] - PreHitInCacheNum[pf_n] - PreHitInMshrNum[pf_n] -
             PreHitInWbNum[pf_n] + 1);
        // precision[pf_n] =
        // (double)(issuenum[pf_n]-UnusedRemovePre[pf_n])/(issuenum[pf_n]+1);
        // recall[pf_n] =
        // (double)(issuenum[pf_n]-UnusedRemovePre[pf_n]
        //-PreHitInCacheNum[pf_n]-PreHitInMshrNum[pf_n]+PreHitInWbNum[pf_n])/(
        //     PreusefulNum[pf_n] + PreHitInCacheNum[pf_n] +
        //     PreHitInMshrNum[pf_n] + demandMshrMissesNum + 1);
        recall[pf_n] = (double)(issuenum[pf_n] - UnusedRemovePre[pf_n]) /
                       (useful_blk + demandMshrMissesNum + 1);
        f1[pf_n] = 2 * precision[pf_n] * recall[pf_n] /
                   (precision[pf_n] + recall[pf_n]);
        all_f1 = all_f1 + f1[pf_n];
        // unusedNum = unusedNum &&(UnusedRemovePre[pf_n] >2000);
        // unusedNum = unusedNum &&(issuenum[pf_n] >20000);
        // unusedNum = unusedNum &&(issuenum[pf_n] >30000);
        unusedNum = unusedNum && (issuenum[pf_n] > 20000);
        // unusedNum = unusedNum &&(issuenum[pf_n] >512);

        // printf(" %d UnusedRemovePre[pf_n]
        // %ld\n",pf_n,UnusedRemovePre[pf_n]);
        // printf(" %d issuenum[pf_n] %ld\n",pf_n,issuenum[pf_n]);
        // printf("1111\n");
        // printf("pf_n %d\n",pf_n);
        chooseorder_m[pf_n].pre_num = pf_n;
        chooseorder_m[pf_n].f1 = f1[pf_n];
    }
    // printf("111-11\n");
    int printf_i = 0;


    /*if (unusedNum){
         for (printf_i = 0 ;printf_i<prefetchers.size();printf_i++){
             printf("printf_i %d f1 %f\n",printf_i,f1[printf_i]);

         }
     }*/

    // f1[0] = 2*precision[0]*recall[0]/(precision[0]+recall[0]);
    // f1[1] = 2*precision[1]*recall[1]/(precision[1]+recall[1]);

    // uint64_t average_f1 = all_f1/(prefetchers.size());
    // struct{}

    // int pf_clean;
    int m_pre_num = 0;
    double m_f1 = 0;


    // if (!hasBeenChoose){
    // printf("222\n");
    if (unusedNum) {
        int c_i;
        /* for (c_i =0;c_i <prefetchers.size();c_i++){
             printf("prefetcher %d issued %ld\n",c_i,
         prefetchers[c_i]->count_insert_num);
         }*/
        for (c_i = 0; c_i < prefetchers.size() - 1; c_i++) {
            for (int c_j = 0; c_j < prefetchers.size() - 1 - c_i; c_j++) {
                if (chooseorder_m[c_j].f1 < chooseorder_m[c_j + 1].f1) {
                    m_pre_num = chooseorder_m[c_j].pre_num;
                    m_f1 = chooseorder_m[c_j].f1;
                    chooseorder_m[c_j].pre_num =
                        chooseorder_m[c_j + 1].pre_num;
                    chooseorder_m[c_j].f1 = chooseorder_m[c_j + 1].f1;
                    chooseorder_m[c_j + 1].pre_num = m_pre_num;
                    chooseorder_m[c_j + 1].f1 = m_f1;
                }
            }
        }

        for (c_i = 0; c_i < prefetchers.size(); c_i++) {
            chooseorder[c_i].pre_num = chooseorder_m[c_i].pre_num;
            chooseorder[c_i].f1 = chooseorder_m[c_i].f1;
            if (chooseorder[c_i].f1 < 0.07) {
                prefetcher_size = 2;
            } else {
                prefetcher_size = 3;
            }
        }
        hasBeenChoose = true;
        // hasBeenChoose = false;
        cleanCachePreCount();

        lastChosenPf_m = 0;
        if ((chooseorder[0].f1 - chooseorder[1].f1 < 0.01) &&
            (chooseorder[1].f1 - chooseorder[2].f1 < 0.02)) {
            hasBeenChoose = false;
        } else {
            pf_turn = chooseorder[0].pre_num;
        }
        // unusedNum = false;
    }
    //}
    // printf("333\n");



    /*if (unusedNum){
        for (printf_i = 0 ;printf_i<prefetchers.size();printf_i++){
            printf("printf_i %d choose_pre %d f1
    %f\n",printf_i,chooseorder[printf_i].pre_num,chooseorder[printf_i].f1);
           // printf("prefetcher %d insert_num
    %ld\n",printf_i,prefetchers[printf_i]->count_insert_num);
           // printf("prefetcher %d preNoUse
    %ld\n",printf_i,prefetchers[printf_i]->PreNoUse);
           // printf("prefetcher %d usefulPreNum
    %ld\n",printf_i,prefetchers[printf_i]->usefulPreNum);
        }
        //assert(0);
    }*/




    int pf = 0;
    if (hasBeenChoose) {
        // pf_turn = firstChoose;
        // for (int pf_c = 0; pf_c < prefetchers.size(); pf_c++)
        if ((chooseorder[0].f1 - chooseorder[1].f1) < 0.01) {
            // printf("1111111111\n");
            for (pf = 0; pf < prefetchers.size() - 1; pf++) {
                // pf_turn = chooseorder[lastChosenPf_m].pre_num;
                // printf("0 f1 %f\n",chooseorder[0].f1);
                // printf("1 f1 %f\n",chooseorder[1].f1);
                // printf("2 f1 %f\n",chooseorder[2].f1);

                if (prefetchers[pf_turn]->nextPrefetchReadyTime() <=
                    curTick()) {
                    PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                    // printf("success pre %d %lx\n",pf_turn,pkt->getAddr());
                    panic_if(
                        !pkt,
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
                if (lastChosenPf_m == 0) {
                    pf_turn = chooseorder[1].pre_num;
                    lastChosenPf_m = 1;
                } else {
                    pf_turn = chooseorder[0].pre_num;
                    lastChosenPf_m = 0;
                }
            }
            if (prefetcher_size == 2) {
                return nullptr;
            } else {
                pf_turn = chooseorder[2].pre_num;
                lastChosenPf_m = 2;
                if (prefetchers[pf_turn]->nextPrefetchReadyTime() <=
                    curTick()) {
                    PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                    // printf("success pre %d %lx\n",pf_turn,pkt->getAddr());
                    panic_if(
                        !pkt,
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
                return nullptr;
            }

            // return nullptr;
        }



        else {
            for (int pf_c = 0; pf_c < prefetcher_size; pf_c++) {
                pf_turn = chooseorder[pf_c].pre_num;
                if (prefetchers[pf_turn]->nextPrefetchReadyTime() <=
                    curTick()) {
                    PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                    panic_if(
                        !pkt,
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
                // pf_turn = (pf_turn + 1) % prefetchers.size();
            }
            return nullptr;
        }

    } else {
        //for (pf = 0; pf < prefetchers.size(); pf++)
        for (pf = 0; pf < prefetcher_size; pf++)
        {
            if (prefetchers[pf_turn]->nextPrefetchReadyTime() <= curTick())
            {

                PacketPtr pkt = prefetchers[pf_turn]->getPacket();
                //printf("success pre %d %lx\n",pf_turn,pkt->getAddr());
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
Multi::lookupCachePre(uint64_t addr)
{
    for (auto pf : prefetchers)
        pf->lookupCachePre(addr);
}
void
Multi::cleanCachePreCount()
{
    for (auto pf : prefetchers)
        pf->cleanCachePreCount();
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

//
// Created by xim on 5/8/21.
//

#include "cpu/pred/ITTAGE.hh"

#include "base/intmath.hh"
#include "debug/Indirect.hh"

namespace gem5
{

namespace branch_prediction
{

ITTAGE::ITTAGE(
        const ITTAGEParams &params)
        : IndirectPredictor(params),
          ghrMask((1UL << params.indirectGHRBits)-1),
          pathLength(params.indirectPathLength),
          numPredictors(params.numPredictors),
          ghrNumBits(params.indirectGHRBits),
          baseBTBSize(params.simpleBTBSize)
{
    std::cout<<"ITTAGE parameters:"<<std::endl;
    std::cout<<"numThreads="<<params.numThreads<<std::endl;
    std::cout<<"numPredictors="<<params.numPredictors<<std::endl;
    threadInfo.resize(params.numThreads);

    targetCache.resize(params.numThreads);
    previous_target.resize(params.numThreads);
    base_predictor.resize(params.numThreads);
    for (unsigned int i = 0; i < params.numThreads; ++i) {
        base_predictor[i].resize(baseBTBSize);
        threadInfo[i].ghr.resize(4096);
    }

    for (unsigned i = 0; i < params.numThreads; i++) {
        targetCache[i].resize(numPredictors);
        for (unsigned j = 0; j < numPredictors; ++j) {
            targetCache[i][j].resize((1 << 8));
        }
    }
    for (unsigned i = 0; i < 16; i++) {
        historyLenTable[i] = std::min(i*4 + 4, 63U);
        std::cout << "historyLenTable[" << i << "]=" << historyLenTable[i] << std::endl;
    }
    use_alt = 8;
    reset_counter = 128;
    fatal_if(ghrNumBits > (sizeof(ThreadInfo::ghr)*8), "ghr_size is too big");
    warn("GHR mask: %#lx\n", ghrMask);
}

void
ITTAGE::genIndirectInfo(ThreadID tid,
                                         void* & indirect_history)
{
    // record the GHR as it was before this prediction
    // It will be used to recover the history in case this prediction is
    // wrong or belongs to bad path
    indirect_history = new bitset(threadInfo[tid].ghr);
}

void
ITTAGE::updateDirectionInfo(
        ThreadID tid, bool actually_taken)
{
    //DPRINTF(Indirect, "Update GHR from %#lx to %#lx (speculative update, direction)\n", threadInfo[tid].ghr,(threadInfo[tid].ghr << 1) | actually_taken);
    threadInfo[tid].ghr <<= 1;
    //threadInfo[tid].ghr |= actually_taken;
    threadInfo.at(tid).ghr.set(0, actually_taken);
    //threadInfo[tid].ghr &= ghrMask;
}

void
ITTAGE::changeDirectionPrediction(ThreadID tid, void * indirect_history, bool actually_taken)
{
    bitset* previousGhr = static_cast<bitset *>(indirect_history);
    threadInfo.at(tid).ghr = ((*previousGhr) << 1);
    threadInfo.at(tid).ghr.set(0, actually_taken);
    DPRINTF(Indirect, "Recover GHR to %#lx\n", threadInfo[tid].ghr);
    //threadInfo[tid].ghr &= ghrMask;
    // maybe we should update hash here?
    // No: CSRs are calculated at use-time
}

bool
ITTAGE::lookup_helper(Addr br_addr, PCStateBase &target,
                      PCStateBase &alt_target, ThreadID tid, int &predictor,
                      int &predictor_index, int &alt_predictor,
                      int &alt_predictor_index, int &pred_count,
                      bool &use_alt_pred)
{
    // todo: adjust according to ITTAGE
    DPRINTF(Indirect, "Looking up %x\n", br_addr);

    unsigned index = getAddrFold(br_addr);
    int pred_counts = 0;
    RiscvISA::PCState target_1, target_2;
    int predictor_1, predictor_2, predictor_index_1, predictor_index_2;

    for (int i = numPredictors - 1; i >= 0; --i) {
        unsigned csr1 = getCSR1(threadInfo[tid].ghr, i);
        unsigned csr2 = getCSR2(threadInfo[tid].ghr, i);
        DPRINTF(Indirect,
                "ITTAGE Predictor %i predict pc %#lx with ghr %#lx\n", i,
                br_addr, threadInfo[tid].ghr);
        unsigned tmp_index = index ^ csr1;
        unsigned tmp_tag = (br_addr & 0xff) ^ csr1 ^ (csr2 << 1);
        if (targetCache[tid][i][tmp_index].tag == tmp_tag) {
            DPRINTF(Indirect, "tag %#lx is found in predictor %i\n", tmp_tag, i);
            if (pred_counts == 0) {
                set(target_1, targetCache[tid][i][tmp_index].target);
                predictor_1 = i;
                predictor_index_1 = tmp_index;
                ++pred_counts;
            }
            if (pred_counts == 1) {
                set(target_2, targetCache[tid][i][tmp_index].target);
                predictor_2 = i;
                predictor_index_2 = tmp_index;
                ++pred_counts;
                break;
            }
        }
    }
    // decide whether use altpred or not
    if (pred_counts > 0) {
        if (use_alt > 7 && targetCache[tid][predictor_1][predictor_index_1].counter == 1 &&
                targetCache[tid][predictor_1][predictor_index_1].useful == 0 && pred_counts == 2 &&
                targetCache[tid][predictor_2][predictor_index_2].counter > 0
        ) {
            use_alt_pred = true;
        } else {
            use_alt_pred = false;
        }
        set(target, target_1);
        set(alt_target, target_2);
        predictor = predictor_1;
        predictor_index = predictor_index_1;
        alt_predictor = predictor_2;
        alt_predictor_index = predictor_index_2;
        pred_count = pred_counts;
        return true;
    } else {
        use_alt_pred = false;
        set(target, base_predictor[tid][(br_addr ^ previous_target[tid].instAddr()) % baseBTBSize]);
        // no need to set
        pred_count = pred_counts;

        DPRINTF(Indirect, "Miss on %#lx, return target %#lx from base table\n", br_addr, target.instAddr());
        return true;
    }
    // may not reach here
    return false;
}

bool ITTAGE::lookup(Addr br_addr, PCStateBase& target, ThreadID tid) {
    RiscvISA::PCState alt_target;
    int predictor, predictor_index, alt_predictor, alt_predictor_index, pred_count; // no use
    bool use_alt_pred;
    lookup_helper(br_addr, target, alt_target, tid, predictor, predictor_index, alt_predictor, alt_predictor_index, pred_count, use_alt_pred);
    if (use_alt_pred) {
        set(target, alt_target);
    }
    if (pred_count == 0) {
        DPRINTF(Indirect, "Hit %#lx target:%#lx with base_predictor\n", br_addr, target.instAddr());
    } else {
        DPRINTF(Indirect, "Hit %#lx target:%#lx\n", br_addr, target.instAddr());
    }
    // std::cout<<"DEBUG: target.instAddr="<<target.instAddr()<<std::endl;
    RiscvISA::PCState &cpTarget = target.as<RiscvISA::PCState>();
    cpTarget.set(target.instAddr());
    return true;
}

void
ITTAGE::recordIndirect(Addr br_addr, Addr tgt_addr,
                                        InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Indirect, "Recording %x seq:%d\n", br_addr, seq_num);
    HistoryEntry entry(br_addr, tgt_addr, seq_num);
    threadInfo[tid].pathHist.push_back(entry);
}

void
ITTAGE::commit(InstSeqNum seq_num, ThreadID tid,
                                void * indirect_history)
{
    DPRINTF(Indirect, "Committing seq:%d\n", seq_num);
    ThreadInfo &t_info = threadInfo[tid];

    // we do not need to recover the GHR, so delete the information
    bitset *previousGhr = static_cast<bitset *>(indirect_history);
    delete previousGhr;

    if (t_info.pathHist.empty()) return;

    if (t_info.headHistEntry < t_info.pathHist.size() &&
        t_info.pathHist[t_info.headHistEntry].seqNum <= seq_num) {
        if (t_info.headHistEntry >= pathLength) {
            t_info.pathHist.pop_front();
        } else {
            ++t_info.headHistEntry;
        }
    }
}

void
ITTAGE::squash(InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Indirect, "Squashing seq:%d\n", seq_num);
    ThreadInfo &t_info = threadInfo[tid];
    auto squash_itr = t_info.pathHist.begin();
    int valid_count = 0;
    while (squash_itr != t_info.pathHist.end()) {
        if (squash_itr->seqNum > seq_num) {
            break;
        }
        ++squash_itr;
        ++valid_count;
    }
    if (squash_itr != t_info.pathHist.end()) {
        DPRINTF(Indirect, "Squashing series starting with sn:%d\n",
                squash_itr->seqNum);
    }
    int queue_size = t_info.pathHist.size();
    for (int i = 0; i < queue_size - valid_count; ++i) {
        t_info.ghr >>=1;
    }
    t_info.pathHist.erase(squash_itr, t_info.pathHist.end());
}

void
ITTAGE::deleteIndirectInfo(ThreadID tid, void * indirect_history)
{
    bitset * previousGhr = static_cast<bitset *>(indirect_history);
    threadInfo[tid].ghr = *previousGhr;

    delete previousGhr;
}

void
ITTAGE::recordTarget(
        InstSeqNum seq_num, void * indirect_history, const PCStateBase& target,
        ThreadID tid)
{
    // here ghr was appended one more
    bitset ghr_last = threadInfo.at(tid).ghr.set(0, 1);
    threadInfo[tid].ghr >>= 1;
    DPRINTF(Indirect, "record with target:%s\n", target);
    // todo: adjust according to ITTAGE
    ThreadInfo &t_info = threadInfo[tid];

    // Should have just squashed so this branch should be the oldest
    auto hist_entry = *(t_info.pathHist.rbegin());
    // Temporarily pop it off the history so we can calculate the set
    t_info.pathHist.pop_back();

    // we have lost the original lookup info, so we need to lookup again
    int predictor, predictor_index, alt_predictor, alt_predictor_index, pred_count, predictor_sel, predictor_index_sel;
    bool use_alt_pred;
    RiscvISA::PCState target_1, target_2, target_sel;
    lookup_helper(hist_entry.pcAddr, target_1, target_2, tid, predictor,
                  predictor_index, alt_predictor, alt_predictor_index,
                  pred_count, use_alt_pred);
    if (use_alt_pred) {
        target_sel = target_2;
        predictor_sel = alt_predictor;
        predictor_index_sel = alt_predictor_index;
    } else {
        target_sel = target_1;
        predictor_sel = predictor;
        predictor_index_sel = predictor_index;
    }
    // update base predictor
    set(base_predictor[tid][(hist_entry.pcAddr ^ previous_target[tid].instAddr()) % baseBTBSize], target);

    // update global history anyway
    hist_entry.targetAddr = target.instAddr();
    t_info.pathHist.push_back(hist_entry);

    bool allocate_values = true;


    if (pred_count > 0 && target_sel == target) {
        // the prediction was from predictor tables and correct
        // increment the counter
        DPRINTF(Indirect, "Prediction for %#lx => %#lx is correct\n", hist_entry.pcAddr, target.instAddr());
        if (targetCache[tid][predictor_sel][predictor_index_sel].counter <= 2) {
            ++targetCache[tid][predictor_sel][predictor_index_sel].counter;
        }
    } else {
        // a misprediction
        DPRINTF(Indirect, "Prediction for %#lx => %#lx is incorrect\n", hist_entry.pcAddr, target.instAddr());
        if (pred_count > 0) {
            if (targetCache[tid][predictor][predictor_index].target ==
                    target &&
                pred_count == 2 &&
                targetCache[tid][alt_predictor][alt_predictor_index].target !=
                    target) {
                // if pred was right and alt_pred was wrong
                DPRINTF(Indirect, "Alt pred was wrong, but pred was right\n");
                targetCache[tid][predictor][predictor_index].useful = 1;
                allocate_values = false;
                if (use_alt > 0) {
                    --use_alt;
                }
            }
            if (targetCache[tid][predictor][predictor_index].target !=
                    target &&
                pred_count == 2 &&
                targetCache[tid][alt_predictor][alt_predictor_index].target ==
                    target) {
                // if pred was wrong and alt_pred was right
                DPRINTF(Indirect, "Alt pred was right, but pred was wrong\n");
                if (use_alt < 15) {
                    ++use_alt;
                }
            }
            // if counter > 0 then decrement, else replace
            if (targetCache[tid][predictor_sel][predictor_index_sel].counter > 0) {
                --targetCache[tid][predictor_sel][predictor_index_sel].counter;
            } else {
                set(targetCache[tid][predictor_sel][predictor_index_sel].target, target);
                targetCache[tid][predictor_sel][predictor_index_sel].tag = (hist_entry.pcAddr & 0xff) ^ getCSR1(threadInfo[tid].ghr, predictor_sel) ^ (getCSR2(threadInfo[tid].ghr, predictor_sel) << 1);
                targetCache[tid][predictor_sel][predictor_index_sel].counter = 1;
                targetCache[tid][predictor_sel][predictor_index_sel].useful = 0;
            }
        }
        DPRINTF(Indirect, "Pred count: %d, allocate values: %u\n", pred_count,
                allocate_values);
        if (pred_count == 0 || allocate_values) {
            int allocated = 0;
            int start_pos;
            if (pred_count > 0){
                start_pos = predictor_sel + 1;
            } else {
                start_pos = 0;
            }
            DPRINTF(Indirect, "Start pos: %d\n", start_pos);
            for (; start_pos < numPredictors; ++start_pos) {
                int new_index = getAddrFold(hist_entry.pcAddr);
                new_index ^= getCSR1(threadInfo[tid].ghr, start_pos);
                if (targetCache[tid][start_pos][new_index].useful == 0) {
                    if (reset_counter < 255) reset_counter++;
                    set(targetCache[tid][start_pos][new_index].target, target);
                    DPRINTF(Indirect, "Allocating table %u [%u] for br %#lx + hist %#lx, target: %#lx\n",
                            start_pos, new_index, hist_entry.pcAddr, threadInfo[tid].ghr, target.instAddr());
                    targetCache[tid][start_pos][new_index].tag =
                        (hist_entry.pcAddr & 0xff) ^
                        getCSR1(threadInfo[tid].ghr, start_pos) ^
                        (getCSR2(threadInfo[tid].ghr, start_pos) << 1);
                    targetCache[tid][start_pos][new_index].counter = 1;
                    ++allocated;
                    ++start_pos; // do not allocate on consecutive predictors
                    if (allocated == 2) {
                        break;
                    }
                } else {
                    // reset useful bits
                    DPRINTF(Indirect,
                            "Allocating table %u [%u] is still usefull, u:%u, "
                            "target: %#lx\n",
                            start_pos, new_index,
                            targetCache[tid][start_pos][new_index].useful,
                            targetCache[tid][start_pos][new_index]
                                .target.instAddr());
                    if (reset_counter > 0) --reset_counter;
                    if (reset_counter == 0) {
                        for (int i = 0; i < numPredictors; ++i) {
                            for (int j = 0; j < (1 << 8); ++j) {
                                targetCache[tid][i][j].useful = 0;
                            }
                        }
                        reset_counter = 128;
                    }
                }
            }
        }

    }

    set(previous_target[tid], target);

    //DPRINTF(Indirect, "Update GHR from %#lx to %#lx (miss path with indirect?)\n", threadInfo[tid].ghr,(threadInfo[tid].ghr << 1) | ghr_last);
    threadInfo[tid].ghr = (threadInfo[tid].ghr << 1) | ghr_last;
}

int ITTAGE::getTableGhrLen(int table) {
    return historyLenTable[table];
}

uint64_t ITTAGE::getCSR1(bitset& ghr, int table) {
    uint64_t ghrLen = getTableGhrLen(table);
    bitset ghr_cpy(ghr);
    ghr_cpy.resize(ghrLen);
    bitset mask(ghrLen,0x7f);
    bitset ret(ghrLen,0);
    //DPRINTF(Indirect, "CSR1 using GHR: %#lx, ghrLen: %d\n", ghr, ghrLen);
    uint64_t i = 0;
    while (i + 7 < ghrLen) {
        ret = ret ^ (ghr_cpy & mask);
        ghr_cpy >>= 7;
        i += 7;
    }
    ret = ret ^ (ghr_cpy & mask);
    ret.resize(7);
    return ret.to_ulong();
}

uint64_t ITTAGE::getCSR2(bitset& ghr, int table) {
    uint64_t ghrLen = getTableGhrLen(table);
    bitset ghr_cpy(ghr);
    ghr_cpy.resize(ghrLen);
    bitset mask(ghrLen,0xff);
    bitset ret(ghrLen,0);
    //DPRINTF(Indirect, "CSR1 using GHR: %#lx, ghrLen: %d\n", ghr, ghrLen);
    uint64_t i = 0;
    while (i + 8 < ghrLen) {
        ret = ret ^ (ghr_cpy & mask);
        ghr_cpy >>= 8;
        i += 8;
    }
    ret = ret ^ (ghr_cpy & mask);
    ret.resize(8);
    return ret.to_ulong();
}

uint8_t ITTAGE::getAddrFold(int address) {
    uint8_t folded_address, k;
    folded_address = 0;
    for (k = 0; k < 3; k++) {
        folded_address ^= ((address % (1 << ((k + 1) * 8))) / (1 << (k * 8)));
    }
    folded_address ^= address / (1 << (24));
    return folded_address;
}

} // namespace branch_prediction
} // namespace gem5
//
// Created by xim on 5/8/21.
//

#include "cpu/pred/ITTAGE.hh"

#include "base/intmath.hh"
#include "debug/Indirect.hh"
#include "sim/core.hh"
#include <algorithm>

namespace gem5
{

namespace branch_prediction
{

using boost::to_string;

ITTAGE::ITTAGE(const ITTAGEParams &params):
    IndirectPredictor(params),
    pathLength(params.indirectPathLength),
    numPredictors(params.numPredictors),
    simpleBTBSize(params.simpleBTBSize),
    pathHistLength(params.pathHistLength),
    tableSizes(params.tableSizes),
    TTagBitSizes(params.TTagBitSizes),
    TTagPcShifts(params.TTagPcShifts),
    histLengths(params.histLengths),
    ittagestats(this)
{
    uint32_t max_histlength = *std::max_element(histLengths.begin(), histLengths.end());
    threadInfo.resize(params.numThreads);
    targetCache.resize(params.numThreads);
    previous_target.resize(params.numThreads);
    base_predictor.resize(params.numThreads);
    for (unsigned int i = 0; i < params.numThreads; ++i) {
        //initialize ghr
        threadInfo[i].ghr.resize(max_histlength);
        threadInfo[i].mark.resize(max_histlength);
        //initialize base predictor
        base_predictor[i].resize(simpleBTBSize);
        //initialize ittage predictor
        targetCache[i].resize(numPredictors);
        for (unsigned j = 0; j < numPredictors; ++j) {
            targetCache[i][j].resize(tableSizes[j]);
        }
    }
    use_alt = 8;
    reset_counter = 128;
    registerExitCallback([this]() {
        std::ofstream ofs("build/tmp/altuseCnt.txt", std::ios::out);
        ofs << "use_alt" << " " << "cnt" << std::endl;
        for (auto& it : ittagestats.usealtCounter) {
            ofs << it.first << " " << it.second << std::endl;
        }
        ofs.close();

        std::ofstream ofs1("build/tmp/TableHitCnt.txt", std::ios::out);
        ofs1 << "table" << " " << "lookupcnt" << " " << "predhit" << " " << "predmiss" << std::endl;
        for (auto& it : ittagestats.THitCnt) {
            ofs1 << it.first << " " << it.second.lookuphit <<" "<< it.second.predhit <<" "<< it.second.predmiss << std::endl;
        }
        ofs1.close();


    });
}

ITTAGE::ITTAGEStats::ITTAGEStats(statistics::Group* parent):
    statistics::Group(parent),
    ADD_STAT(mainlookupHit, statistics::units::Count::get(), "ittage the provider component lookup hit"),
    ADD_STAT(altlookupHit, statistics::units::Count::get(), "ittage the alternate prediction lookup hit"),
    ADD_STAT(mainpredHit, statistics::units::Count::get(), "ittage the provider component pred hit"),
    ADD_STAT(altpredHit, statistics::units::Count::get(), "ittage the alternate prediction pred hit")
{
    mainpredHit.prereq(mainpredHit);
    altpredHit.prereq(altpredHit);
    mainlookupHit.prereq(mainlookupHit);
    altlookupHit.prereq(altlookupHit);
}

void
ITTAGE::genIndirectInfo(ThreadID tid,
                                         void* & indirect_history)
{
    // record the GHR as it was before this prediction
    // It will be used to recover the history in case this prediction is
    // wrong or belongs to bad path
    indirect_history = new ThreadInfo(threadInfo[tid]);
}

void
ITTAGE::updateDirectionInfo(
        ThreadID tid, bool actually_taken)
{
    if (GEM5_UNLIKELY(TRACING_ON && gem5::debug::Indirect)) {
        to_string(threadInfo[tid].ghr, prBuf1);
    }
    threadInfo[tid].ghr <<= 1;
    threadInfo[tid].ghr.set(0, actually_taken);
    threadInfo[tid].mark <<= 1;

    if (GEM5_UNLIKELY(TRACING_ON && gem5::debug::Indirect)) {
        to_string(threadInfo[tid].ghr, prBuf2);
    }
    DPRINTF(Indirect,
            "Update GHR from %s to %s (speculative update, direction)\n",
            prBuf1, prBuf2);
}

void
ITTAGE::changeDirectionPrediction(ThreadID tid, void * indirect_history, bool actually_taken)
{
    ThreadInfo* previousThreadInfo = static_cast<ThreadInfo*>(indirect_history);
    threadInfo[tid].ghr = ((previousThreadInfo->ghr) << 1);
    threadInfo[tid].ghr.set(0, actually_taken);
    threadInfo[tid].mark = ((previousThreadInfo->mark) << 1);
    to_string(threadInfo[tid].ghr, prBuf1);
    DPRINTF(Indirect, "Recover GHR to %s\n", prBuf1);
}

bool
ITTAGE::lookup_helper(Addr br_addr, bitset& ghr, PCStateBase& target, PCStateBase& alt_target, ThreadID tid, int& predictor, int& predictor_index, int& alt_predictor, int& alt_predictor_index, int& pred_count, bool& use_alt_pred)
{
    // todo: adjust according to ITTAGE
    DPRINTF(Indirect, "Looking up %x\n", br_addr);
    int pred_counts = 0;
    std::unique_ptr<PCStateBase> target_1, target_2;
    int predictor_1 = 0;
    int predictor_2 = 0;
    int predictor_index_1 = 0;
    int predictor_index_2 = 0;

    for (int i = numPredictors - 1; i >= 0; --i) {
        uint32_t csr1 = getCSR1(ghr, i);
        to_string(ghr, prBuf1);
        DPRINTF(Indirect, "ITTAGE Predictor %i predict pc %#lx with ghr %s\n",
                i, br_addr, prBuf1);
        uint32_t index = getAddrFold(br_addr, i);
        uint32_t tmp_index = index ^ csr1;
        uint32_t tmp_tag = getTag(br_addr, ghr, i);
        const auto &way = targetCache[tid][i][tmp_index];
        if (way.tag == tmp_tag && way.target) {
            DPRINTF(Indirect, "tag %#lx is found in predictor %i\n", tmp_tag,
                    i);
            if (pred_counts == 0) {//第一次命中
                set(target_1, way.target);
                predictor_1 = i;
                predictor_index_1 = tmp_index;
                ++pred_counts;
            }
            if (pred_counts == 1) {//第二次命中
                set(target_2, way.target);
                predictor_2 = i;
                predictor_index_2 = tmp_index;
                ++pred_counts;
                break;
            }
        }
    }
    pred_count = pred_counts;
    if (pred_counts > 0) {
        const auto& way1 = targetCache[tid][predictor_1][predictor_index_1];
        const auto& way2 = targetCache[tid][predictor_2][predictor_index_2];
        if ((use_alt > 10) && (way1.counter == 1) && (way1.useful == 0) && (pred_counts == 2) && (way2.counter > 0)) {
            use_alt_pred = true;
            ittagestats.altlookupHit++;
        } else {
            use_alt_pred = false;
            ittagestats.mainlookupHit++;
        }
        set(target, *target_1);
        if (use_alt_pred) {
            set(alt_target, *target_2);
        }
        predictor = predictor_1;
        predictor_index = predictor_index_1;
        alt_predictor = predictor_2;
        alt_predictor_index = predictor_index_2;
        pred_count = pred_counts;
        return true;
    } else {
        use_alt_pred = false;
        const PCStateBase *prev_target = previous_target[tid].get();
        if (prev_target) {
            const PCStateBase *base_target =
                base_predictor[tid][(br_addr ^ prev_target->instAddr()) % simpleBTBSize].get();
            if (base_target) {
                set(target, *base_predictor[tid][(br_addr ^ prev_target->instAddr()) % simpleBTBSize]);
                // no need to set
                pred_count = pred_counts;
                DPRINTF(Indirect, "Miss on %#lx, return target %#lx from base table\n", br_addr, target.instAddr());
                return true;
            }
        }
    }
    // may not reach here
    DPRINTF(Indirect, "Miss %x\n", br_addr);
    return false;
}

bool ITTAGE::lookup(Addr br_addr, PCStateBase& target, ThreadID tid) {
    PCStateBase *alt_target = target.clone();
    int predictor = 0;
    int predictor_index = 0;
    int alt_predictor = 0;
    int alt_predictor_index = 0;
    int pred_count = 0; // no use
    bool use_alt_pred = false;
    //bool lookupResult =
    lookup_helper(br_addr,threadInfo[tid].ghr,target, *alt_target, tid, predictor, predictor_index, alt_predictor, alt_predictor_index, pred_count, use_alt_pred);
    
    // if (!lookupResult) {
    //     return false;
    // }
    if (use_alt_pred) {
        assert(alt_target);
        set(target, *alt_target);
    }
    if (pred_count == 0) {
        DPRINTF(Indirect, "Hit %#lx (target:%#lx) with base_predictor\n", br_addr, target.instAddr());
    } else {
        DPRINTF(Indirect, "Hit %#lx (target:%#lx)\n", br_addr, target.instAddr());
    }
    // std::cout<<"DEBUG: target.instAddr="<<target.instAddr()<<std::endl;
    // target.set(target.instAddr());
    return true;
}

void
ITTAGE::recordIndirect(Addr br_addr, Addr tgt_addr,
                                        InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Indirect, "Recording %x seq:%d\n", br_addr, seq_num);
    HistoryEntry entry(br_addr, tgt_addr, seq_num);
    threadInfo[tid].pathHist.push_back(entry);
    for (int i = 0;i < pathHistLength;i++) {
        bool pathBit = ((br_addr >> i) ^ (tgt_addr >> i)) & 1ULL;
        threadInfo[tid].ghr <<= 1;
        threadInfo[tid].ghr.set(0, pathBit);
        threadInfo[tid].mark <<= 1;
        threadInfo[tid].mark.set(0, 1);
    }
}

void
ITTAGE::commit(InstSeqNum seq_num, ThreadID tid,
                                void * indirect_history)
{
    DPRINTF(Indirect, "Committing seq:%d\n", seq_num);
    ThreadInfo &t_info = threadInfo[tid];

    // we do not need to recover the GHR, so delete the information
    ThreadInfo* previousThreadInfo = static_cast<ThreadInfo*>(indirect_history);
    delete previousThreadInfo;

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
        t_info.mark >>=1;
        if (t_info.mark.test(0)) {
            t_info.ghr >>= pathHistLength;
            t_info.mark >>= pathHistLength;
        }
    }
    t_info.pathHist.erase(squash_itr, t_info.pathHist.end());
}

void
ITTAGE::deleteIndirectInfo(ThreadID tid, void * indirect_history)
{
    ThreadInfo* previousThreadInfo = static_cast<ThreadInfo*>(indirect_history);
    threadInfo[tid].ghr = previousThreadInfo->ghr;
    threadInfo[tid].mark = previousThreadInfo->mark;

    delete previousThreadInfo;
}

void
ITTAGE::recordTarget(
        InstSeqNum seq_num, void * indirect_history, const PCStateBase& target,
        ThreadID tid)
{
    bitset& ghr = *static_cast<bitset*>(indirect_history);
    // here ghr was appended one more
    bitset ghr_last = threadInfo[tid].ghr.set(0, 1);// | 1;
    threadInfo[tid].ghr >>= 1;
    if (threadInfo[tid].mark.test(1)) {
        threadInfo[tid].ghr >>= pathHistLength;
    }
    DPRINTF(Indirect, "record with target:%s\n", target);
    // todo: adjust according to ITTAGE
    ThreadInfo &t_info = threadInfo[tid];

    // Should have just squashed so this branch should be the oldest
    auto hist_entry = *(t_info.pathHist.rbegin());
    // Temporarily pop it off the history so we can calculate the set
    t_info.pathHist.pop_back();

    // we have lost the original lookup info, so we need to lookup again
    int predictor = 0;
    int predictor_index = 0;
    int alt_predictor = 0;
    int alt_predictor_index = 0;
    int pred_count = 0; // no use
    int predictor_sel = 0;
    int predictor_index_sel = 0;
    bool use_alt_pred = false;
    PCStateBase *target_1 = target.clone();
    PCStateBase *target_2 = target.clone();
    std::unique_ptr<PCStateBase> target_sel;
    bool predictor_found = lookup_helper(hist_entry.pcAddr,ghr, *target_1, *target_2, tid, predictor, predictor_index, alt_predictor, alt_predictor_index, pred_count, use_alt_pred);
    ittagestats.usealtCounter[use_alt]++;
    if (predictor_found && use_alt_pred) {
        set(target_sel, target_2);
        predictor_sel = alt_predictor;
        predictor_index_sel = alt_predictor_index;
    } else if (predictor_found) {
        set(target_sel, target_1);
        predictor_sel = predictor;
        predictor_index_sel = predictor_index;
    } else {
        predictor_sel = predictor;
        predictor_index_sel = predictor_index;
    }
    if(predictor_found){
        if(pred_count==1){
            ittagestats.THitCnt[predictor].lookuphit++;
            if(targetCache[tid][predictor][predictor_index].target->equals(target)){
                ittagestats.THitCnt[predictor].predhit++;
            }
            else{
                ittagestats.THitCnt[predictor].predmiss++;
            }
        }
        if(pred_count==2){
            ittagestats.THitCnt[alt_predictor].lookuphit++;
            if(targetCache[tid][alt_predictor][alt_predictor_index].target->equals(target)){
                ittagestats.THitCnt[alt_predictor].predhit++;
            }
            else{
                ittagestats.THitCnt[alt_predictor].predmiss++;
            }
        }
    }

    // update previous target
    set(previous_target[tid], target);
    DPRINTF(Indirect, "update previous target: %s\n", *previous_target[tid]);
    // update base predictor
    const PCStateBase *prev_target = previous_target[tid].get();
    if (prev_target) {
        set(base_predictor[tid][(hist_entry.pcAddr ^ previous_target[tid]->instAddr()) %simpleBTBSize],
            target);
        DPRINTF(Indirect, "Update base predictor: %s\n",
                *base_predictor[tid][(hist_entry.pcAddr ^ previous_target[tid]->instAddr()) % simpleBTBSize]);
    }
    
    // update global history anyway
    hist_entry.targetAddr = target.instAddr();
    t_info.pathHist.push_back(hist_entry);

    bool allocate_values = true;

    auto& way_sel = targetCache[tid][predictor_sel][predictor_index_sel];
    if (pred_count > 0 && target_sel->equals(target)) {//pred hit
        // the prediction was from predictor tables and correct
        // increment the counter
        DPRINTF(Indirect, "Prediction for %#lx => %#lx is correct\n", hist_entry.pcAddr, target.instAddr());
        if (way_sel.counter <= 2) {
            ++way_sel.counter;
        }
        if((predictor_sel==alt_predictor) && (pred_count==2)){
            ittagestats.altpredHit++;
        }else if(pred_count>0){
            ittagestats.mainpredHit++;
        }
    } else {
        // a misprediction
        DPRINTF(Indirect, "Prediction for %#lx => %#lx is incorrect\n", hist_entry.pcAddr, target.instAddr());
        auto& way1 = targetCache[tid][predictor][predictor_index];
        auto& way2 = targetCache[tid][alt_predictor][alt_predictor_index];
        if (pred_count > 0) {
            if (way1.target->equals(target) &&
                pred_count == 2 &&
                !way2.target->equals(target)) {
                ittagestats.mainpredHit++;
                // if pred was right and alt_pred was wrong
                way1.useful = 1;
                DPRINTF(Indirect, "Alt pred was wrong, but pred was right\n");
                allocate_values = false;
                if (use_alt > 0) {
                    --use_alt;
                }
            }
            if (!way1.target->equals(target) &&
                pred_count == 2 &&
                way2.target->equals(target)) {
                ittagestats.altpredHit++;
                // if pred was wrong and alt_pred was right
                DPRINTF(Indirect, "Alt pred was right, but pred was wrong\n");
                if (use_alt < 15) {
                    ++use_alt;
                }
            }
            // if counter > 0 then decrement, else replace

            if (way_sel.counter > 0) {
                --way_sel.counter;
            } else {
                set(way_sel.target, target);
                way_sel.tag =
                    getTag(hist_entry.pcAddr,
                           ghr,
                           predictor_sel);
                way_sel.counter = 1;
                way_sel.useful = 0;
            }
        }
        //update the tag
        DPRINTF(Indirect, "Pred count: %d, allocate values: %u\n", pred_count, allocate_values);
        if (pred_count == 0 || allocate_values) {
            int allocated = 0;
            uint32_t start_pos;
            if (pred_count > 0){
                start_pos = predictor_sel + 1;
            } else {
                start_pos = 0;
            }
            DPRINTF(Indirect, "Start pos: %d\n", start_pos);
            for (; start_pos < numPredictors; ++start_pos) {
                uint32_t new_index = getAddrFold(hist_entry.pcAddr, start_pos);
                new_index ^= getCSR1(ghr, start_pos);
                auto& way_new = targetCache[tid][start_pos][new_index];
                if (way_new.useful == 0) {
                    if (reset_counter < 255) reset_counter++;
                    set(way_new.target, target);
                    way_new.tag =
                        getTag(hist_entry.pcAddr,
                               ghr,
                               start_pos);
                    way_new.counter = 1;

                    to_string(ghr, prBuf1);
                    DPRINTF(Indirect,
                            "Allocating table %u [%u] for br %#lx + hist "
                            "%s, target: %#lx\n",
                            start_pos, new_index, hist_entry.pcAddr, prBuf1,
                            target.instAddr());
                    ++allocated;
                    //++start_pos; // do not allocate on consecutive predictors
                    if (allocated == 2) {
                        break;
                    }
                } else {
                    // reset useful bits
                    if (reset_counter > 0) {
                        DPRINTF(
                            Indirect,
                            "Allocating table %u [%u] is still usefull, u:%u, "
                            "target: %#lx\n",
                            start_pos, new_index,
                            way_new.useful,
                            way_new.target->instAddr());

                        --reset_counter;
                    }
                    if (reset_counter == 0) {
                        for (int i = 0; i < numPredictors; ++i) {
                            for (int j = 0; j < tableSizes[i]; ++j) {
                                targetCache[tid][i][j].useful = 0;
                            }
                        }
                        reset_counter = 128;
                    }
                }
            }
        }

    }

    if (GEM5_UNLIKELY(TRACING_ON && gem5::debug::Indirect)) {
        to_string(ghr, prBuf1);
    }
    unsigned shift = threadInfo[tid].mark.test(1) ? pathHistLength + 1 : 1;
    threadInfo[tid].ghr = (threadInfo[tid].ghr << shift) | ghr_last;
    if (GEM5_UNLIKELY(TRACING_ON && gem5::debug::Indirect)) {
        to_string(ghr, prBuf1);
    }
    DPRINTF(Indirect,
            "Update GHR from %#lx to %#lx (miss path with indirect?)\n",
            prBuf1, prBuf2);
}

uint64_t ITTAGE::getTableGhrLen(int table) {
    return histLengths[table];
}

uint64_t ITTAGE::getCSR1(bitset& ghr, int table) {
    uint64_t ghrLen = getTableGhrLen(table);
    bitset ghr_cpy(ghr); // remove unnecessary data on higher position
    to_string(ghr, prBuf1);
    DPRINTF(Indirect, "CSR1 using GHR: %s, ghrLen: %d\n", prBuf1, ghrLen);
    ghr_cpy.resize(ghrLen);
    bitset ret(ghrLen, 0);
    int i = 0;
    while (i + (ceilLog2(tableSizes[table])-1) < ghrLen) {
        ret = ghr_cpy ^ ret;
        ghr_cpy >>= (ceilLog2(tableSizes[table])-1);
        i += (ceilLog2(tableSizes[table])-1);
    }
    ret = ret ^ ghr_cpy;
    ret.resize(ceilLog2(tableSizes[table])-1);
    DPRINTF(Indirect, "CSR1: %#lx\n", ret.to_ulong());
    return ret.to_ulong();
}

uint64_t ITTAGE::getCSR2(bitset& ghr, int table) {
    uint64_t ghrLen = getTableGhrLen(table);
    bitset ghr_cpy(ghr); // remove unnecessary data on higher position
    to_string(ghr, prBuf1);
    DPRINTF(Indirect, "CSR2 using GHR: %s, ghrLen: %d\n", prBuf1, ghrLen);
    ghr_cpy.resize(ghrLen);
    bitset ret(ghrLen, 0);
    int i = 0;
    while (i + ceilLog2(tableSizes[table]) < ghrLen) {
        ret = ghr_cpy ^ ret;
        ghr_cpy >>= ceilLog2(tableSizes[table]);
        i += ceilLog2(tableSizes[table]);
    }
    ret = ret ^ ghr_cpy;
    ret.resize(ceilLog2(tableSizes[table]));
    DPRINTF(Indirect, "CSR2: %#lx\n", ret.to_ulong());
    return ret.to_ulong();
}

uint64_t ITTAGE::getAddrFold(uint64_t address, int table) {
    uint64_t folded_address, k;
    folded_address = 0;
    for (k = 0; k < 8; k++) {
        folded_address ^= address;
        address >>= 8;
    }
    //folded_address ^= address / (1 << (24));
    return folded_address & ((1 << ceilLog2(tableSizes[table])) - 1);
}
uint64_t ITTAGE::getTag(Addr pc, bitset& ghr, int table) {
    uint64_t csr1 = getCSR1(ghr, table);
    uint64_t csr2 = getCSR2(ghr, table);
    return ((pc >> TTagPcShifts[table]) ^ csr1 ^ (csr2 << 1)) & ((1 << TTagBitSizes[table]) - 1);
}




} // namespace branch_prediction
} // namespace gem5


//337767
//336489
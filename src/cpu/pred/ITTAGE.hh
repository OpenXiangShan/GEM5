//
// Created by xim on 5/8/21.
//
/*
 * This file provides a basic implementation of ITTAGE
 * Note that altpred is NOT utilized in this implementation.
 *
 * */

#ifndef __CPU_PRED_ITTAGE_HH__
#define __CPU_PRED_ITTAGE_HH__

#include <deque>

#include "arch/generic/pcstate.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/indirect.hh"
#include "params/ITTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

class ITTAGE : public IndirectPredictor {
public:

    ITTAGE(const ITTAGEParams &params);

    bool lookup(Addr br_addr, PCStateBase& br_target, ThreadID tid);
    void recordIndirect(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num, ThreadID tid);
    void commit(InstSeqNum seq_num, ThreadID tid, void * indirect_history);
    void squash(InstSeqNum seq_num, ThreadID tid);
    void recordTarget(InstSeqNum seq_num, void * indirect_history, const PCStateBase &target, ThreadID tid);
    void genIndirectInfo(ThreadID tid, void* & indirect_history);
    void updateDirectionInfo(ThreadID tid, bool actually_taken);
    void deleteIndirectInfo(ThreadID tid, void * indirect_history);
    void changeDirectionPrediction(ThreadID tid, void * indirect_history, bool actually_taken);

private:
    bool lookup_helper(Addr, PCStateBase&, PCStateBase&, ThreadID, int&, int&, int&, int&, int&, bool&);
    unsigned getCSR1(unsigned ghr, int table);
    unsigned getCSR2(unsigned ghr, int table);
    uint32_t getAddrFold(int address, int table);
    uint32_t getTag(Addr pc, uint32_t csr1, uint32_t csr2, int table);
    int getTableGhrLen(int table);
    const unsigned ghrMask;
    const unsigned pathLength;
    const unsigned numPredictors;
    const unsigned ghrNumBits;
    const unsigned numTageBits;
    std::vector<int> histBitSizes;
    std::vector<int> histTagBitSizes;
    std::vector<int> histTagPcShifts;
    int use_alt; // min:0 max: 15
    int reset_counter;
    std::vector<std::unique_ptr<PCStateBase>> previous_target;
    std::vector<std::vector<std::unique_ptr<PCStateBase>> >base_predictor;
    struct IPredEntry {
        Addr tag = 0;
        std::unique_ptr<PCStateBase> target;
        int counter = 0;
        int useful = 0;
    };
    // the first level: thread
    // the second level: predictor
    // the third level: index
    std::vector<std::vector<std::vector<IPredEntry> > >targetCache;

    struct HistoryEntry {
        HistoryEntry(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num) : pcAddr(br_addr), targetAddr(tgt_addr), seqNum(seq_num) {}
        Addr pcAddr;
        Addr targetAddr;
        InstSeqNum seqNum;
    };

    struct ThreadInfo {
        ThreadInfo() : headHistEntry(0), ghr(0) {}

        std::deque<HistoryEntry> pathHist;
        unsigned headHistEntry;
        unsigned ghr;
    };

    std::vector<ThreadInfo> threadInfo;

};

} // namespace branch_prediction
} // namespace gem5

#endif //__CPU_PRED_ITTAGE_HH__
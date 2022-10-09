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

#include <unordered_map>
#include <boost/dynamic_bitset.hpp>

#include <deque>
#include "base/types.hh"
#include "base/statistics.hh"
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
    using bitset = boost::dynamic_bitset<>;
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
    bool lookup_helper(Addr,bitset&, PCStateBase&, PCStateBase&, ThreadID, int&, int&, int&, int&, int&, bool&);
    uint64_t getCSR1(bitset& ghr, int table);
    uint64_t getCSR2(bitset& ghr, int table);
    uint64_t getAddrFold(uint64_t address, int table);
    uint64_t getTag(Addr pc, bitset& ghr, int table);
    uint64_t getTableGhrLen(int table);
    const unsigned pathLength;
    const unsigned numPredictors;
    const unsigned simpleBTBSize;
    std::vector<int> tableSizes;
    std::vector<int> TTagBitSizes;
    std::vector<int> TTagPcShifts;
    std::vector<int> histLengths;
    int use_alt; // min:0 max: 15
    int reset_counter;
    std::vector<std::unique_ptr<PCStateBase>> previous_target;
    std::vector<std::vector<std::unique_ptr<PCStateBase>> >base_predictor;


    struct TCnts{
        uint32_t lookuphit;
        uint32_t predmiss;
        uint32_t predhit;
    };
    struct ITTAGEStats : public statistics::Group {
        statistics::Scalar mainlookupHit;
        statistics::Scalar altlookupHit;
        statistics::Scalar mainpredHit;
        statistics::Scalar altpredHit;
        std::map<uint32_t, uint32_t> usealtCounter;
        std::map<uint32_t, TCnts>  THitCnt;
        ITTAGEStats(statistics::Group* parent);
    }ittagestats;

    
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
        HistoryEntry(Addr br_addr, Addr tgt_addr, InstSeqNum seq_num)
            : pcAddr(br_addr),
              targetAddr(tgt_addr),
              seqNum(seq_num)
        {
        }
        Addr pcAddr;
        Addr targetAddr;
        InstSeqNum seqNum;
    };

    void commitHistoryEntry(const HistoryEntry &entry, bitset *ghr, ThreadID tid);

    struct ThreadInfo {
        ThreadInfo() : headHistEntry(0), ghr(0) {}

        std::deque<HistoryEntry> pathHist;
        unsigned headHistEntry;
        bitset ghr;
    };

    const unsigned observeHistLen{27};
    std::unordered_map<unsigned, uint64_t> missHistMap;

    std::vector<ThreadInfo> threadInfo;

    std::string prBuf1, prBuf2;

    Addr lastIndirectBrAddr;
};

} // namespace branch_prediction
} // namespace gem5

#endif //__CPU_PRED_ITTAGE_HH__
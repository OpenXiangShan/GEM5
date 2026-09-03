#ifndef __CPU_PRED_BTB_RAS_HH__
#define __CPU_PRED_BTB_RAS_HH__

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/common.hh"

// Conditional includes based on build mode
#ifdef UNIT_TEST
    // Test mode includes
    #include "cpu/pred/btb/test/test_dprintf.hh"
    #include "cpu/pred/btb/timed_base_pred.hh"

#else
    // Production mode includes
    #include "cpu/pred/btb/timed_base_pred.hh"
    #include "debug/RAS.hh"
    #include "params/BTBRAS.hh"
#endif

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

// Class definition with conditional inheritance and constructors
#ifdef UNIT_TEST
    namespace test {
        class BTBRAS : public TimedBaseBTBPredictor
        {
        public:
            // Test constructor for unit testing mode
            BTBRAS(unsigned numEntries, unsigned ctrWidth, unsigned numInflightEntries);
#else
    class BTBRAS : public TimedBaseBTBPredictor
    {
    public:
        // Production constructor
        typedef BTBRASParams Params;
        BTBRAS(const Params &p);
#endif

        typedef struct RASEssential
        {
            Addr retAddr;
            unsigned ctr;
        }RASEssential;

        typedef struct RASEntry
        {
            RASEssential data;
            RASEntry(Addr retAddr, unsigned ctr)
            {
                data.retAddr = retAddr;
                data.ctr = ctr;
            }
            RASEntry(Addr retAddr)
            {
                data.retAddr = retAddr;
                data.ctr = 0;
            }
            RASEntry()
            {
                data.retAddr = 0;
                data.ctr = 0;
            }
        }RASEntry;

        typedef struct RASInflightEntry
        {
            RASEssential data;
            int nos; // parent node pointer
        }RASInflightEntry;

        typedef struct RASMeta {
            int ssp;
            int sctr;
            // RASEntry tos; // top of stack
            int TOSR;
            int TOSW;
            bool willPush;
            Addr target;
            // RASInflightEntry inflight; // inflight top of stack
        }RASMeta;

        void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                          std::vector<FullBTBPrediction> &stagePreds) override;
        
        std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
        void refreshPredictionMeta(Addr startAddr,
                                   const boost::dynamic_bitset<> &history,
                                   FullBTBPrediction &pred) override;

        void specUpdateState(FullBTBPrediction &pred);

        void recoverState(const HistoryRecoveryContext &context,
                          const BranchInfo &actualBranch,
                          bool actuallyTaken);

        void update(const PredictionUpdateContext &context,
                    const PreparedUpdate &update) override;

        // RAS prediction statistics require a concrete DynInst in production.
#ifndef UNIT_TEST
        void commitBranch(const FetchTarget &stream, const DynInstPtr &inst) override;
#endif

        Addr getTopAddrFromMetas(const FetchTarget &stream);

    private:
        struct ThreadRASState
        {
            int TOSW = 0; // inflight pointer to the write top of stack
            int TOSR = 0; // inflight pointer to the read top of stack
            int BOS = 0;  // inflight pointer to the bottom of stack
            int ssp = 0;  // speculative stack pointer
            int nsp = 0;  // committed stack pointer
            int sctr = 0;
            std::vector<RASEntry> stack;
            std::vector<RASInflightEntry> inflightStack;
            std::shared_ptr<RASMeta> meta;
        };

        void initThreadState(ThreadRASState &state);

        void push(ThreadID tid, Addr retAddr);

        void pop(ThreadID tid);

        void push_stack(ThreadID tid, Addr retAddr);

        void pop_stack(ThreadID tid);

        void ptrInc(int &ptr);

        void ptrDec(int &ptr);

        void inflightPtrInc(int &ptr);
        
        void inflightPtrDec(int &ptr);

        bool inflightInRange(const ThreadRASState &state, int ptr);

        int inflightPtrPlus1(int ptr);

        void checkCorrectness(ThreadID tid);

        RASEssential getTop(ThreadID tid);

        RASEssential getTop_meta(ThreadID tid);

        void printStack(const char *when, ThreadID tid) {
            auto &state = threadStates[tid];
            DPRINTF(RAS, "[tid:%u] printStack when %s: \n", tid, when);
            for (int i = 0; i < numEntries; i++) {
                DPRINTFR(RAS, "entry [%d], retAddr %#lx, ctr %d", i,
                         state.stack[i].data.retAddr, state.stack[i].data.ctr);
                if (state.ssp == i) {
                    DPRINTFR(RAS, " <-- SSP");
                }
                if (state.nsp == i) {
                    DPRINTFR(RAS, " <-- NSP");
                }
                DPRINTFR(RAS, "\n");
            }
            DPRINTFR(RAS, "non-volatile stack:\n");
            for (int i = 0; i < numInflightEntries; i++) {
                DPRINTFR(RAS, "entry [%d] retAddr %#lx, ctr %u nos %d", i,
                         state.inflightStack[i].data.retAddr,
                         state.inflightStack[i].data.ctr,
                         state.inflightStack[i].nos);
                if (state.TOSW == i) {
                    DPRINTFR(RAS, " <-- TOSW");
                }
                if (state.TOSR == i) {
                    DPRINTFR(RAS, " <-- TOSR");
                }
                if (state.BOS == i) {
                    DPRINTFR(RAS, " <-- BOS");
                }
                DPRINTFR(RAS, "\n");
            }
            /*
            DPRINTFR(RAS, "non-volatile stack current data:\n");
            int a = TOSR;
            int inflightCurrentSz = 0;
            while (inflightInRange(a)) {
                DPRINTFR(RAS, "retAddr %#lx, ctr %d\n", inflightStack[a].data.retAddr, inflightStack[a].data.ctr);
                ++inflightCurrentSz;
                a = inflightStack[a].nos;
                if (inflightCurrentSz > 30) {
                    DPRINTFR(RAS, "...\n");
                    break;
                }
            }
            */
            //if (ssp > nsp && (ssp - nsp != inflightCurrentSz)) {
            //    DPRINTFR(RAS, "inflight size mismatch!\n");
            //}
        }

        unsigned numEntries;

        unsigned ctrWidth;

        unsigned numInflightEntries;

        int maxCtr;

        unsigned numThreads;

        std::vector<ThreadRASState> threadStates;

#ifdef UNIT_TEST
    typedef uint64_t Scalar;
#else
    typedef statistics::Scalar Scalar;
#endif

#ifdef UNIT_TEST
        struct RASStats
        {
#else
    struct RASStats : public statistics::Group
    {
#endif
        Scalar PredWrong;
        Scalar MispredWithSctr;
        Scalar PredCorrect;
        Scalar CorrectWithSctr;

        Scalar Pushes;
        Scalar Pops;

#ifndef UNIT_TEST
        RASStats(statistics::Group* parent);
#endif
        } rasStats;


}; // class BTBRAS

// Close conditional namespaces
#ifdef UNIT_TEST
    } // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_RAS_HH__

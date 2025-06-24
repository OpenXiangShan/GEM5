#ifndef __CPU_PRED_BTB_RAS_HH__
#define __CPU_PRED_BTB_RAS_HH__

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/RAS.hh"
#include "params/BTBRAS.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

class BTBRAS : public TimedBaseBTBPredictor
{
    public:
    
        typedef BTBRASParams Params;
        BTBRAS(const Params &p);

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
        
        std::shared_ptr<void> getPredictionMeta() override;

        void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;

        void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;

        void update(const FetchStream &entry) override;

        void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

        Addr getTopAddrFromMetas(const FetchStream &stream);

    private:

        void push(Addr retAddr);

        void pop();

        void push_stack(Addr retAddr);
        
        void pop_stack();

        void ptrInc(int &ptr);

        void ptrDec(int &ptr);

        void inflightPtrInc(int &ptr);
        
        void inflightPtrDec(int &ptr);

        bool inflightInRange(int &ptr);

        int inflightPtrPlus1(int ptr);

        void checkCorrectness();

        RASEssential getTop();

        RASEssential getTop_meta();

        void printStack(const char *when) {
            DPRINTF(RAS, "printStack when %s: \n", when);
            for (int i = 0; i < numEntries; i++) {
                DPRINTFR(RAS, "entry [%d], retAddr %#lx, ctr %d", i, stack[i].data.retAddr, stack[i].data.ctr);
                if (ssp == i) {
                    DPRINTFR(RAS, " <-- SSP");
                }
                if (nsp == i) {
                    DPRINTFR(RAS, " <-- NSP");
                }
                DPRINTFR(RAS, "\n");
            }
            DPRINTFR(RAS, "non-volatile stack:\n");
            for (int i = 0; i < numInflightEntries; i++) {
                DPRINTFR(RAS, "entry [%d] retAddr %#lx, ctr %u nos %d", i, inflightStack[i].data.retAddr, inflightStack[i].data.ctr, inflightStack[i].nos);
                if (TOSW == i) {
                    DPRINTFR(RAS, " <-- TOSW");
                }
                if (TOSR == i) {
                    DPRINTFR(RAS, " <-- TOSR");
                }
                if (BOS == i) {
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

        int TOSW; // inflight pointer to the write top of stack

        int TOSR; // inflight pointer to the read top of stack

        int BOS; // inflight pointer to the bottom of stack

        int maxCtr;

        int ssp; // spec sp
        
        int nsp; // non-spec sp

        int sctr;

        //int ndepth;

        std::vector<RASEntry> stack;
        
        std::vector<RASInflightEntry> inflightStack;

        std::shared_ptr<RASMeta> meta;


};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_RAS_HH__

#ifndef __CPU_PRED_FTB_RAS_HH__
#define __CPU_PRED_FTB_RAS_HH__

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "cpu/pred/ftb/timed_base_pred.hh"
#include "debug/FTBRAS.hh"
#include "params/RAS.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

class RAS : public TimedBaseFTBPredictor
{
    public:
    
        typedef RASParams Params;
        RAS(const Params &p);

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
                          std::vector<FullFTBPrediction> &stagePreds) override;
        
        std::shared_ptr<void> getPredictionMeta() override;

        void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

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
            DPRINTF(FTBRAS, "printStack when %s: \n", when);
            for (int i = 0; i < numEntries; i++) {
                DPRINTFR(FTBRAS, "entry [%d], retAddr %#lx, ctr %d", i, stack[i].data.retAddr, stack[i].data.ctr);
                if (ssp == i) {
                    DPRINTFR(FTBRAS, " <-- SSP");
                }
                if (nsp == i) {
                    DPRINTFR(FTBRAS, " <-- NSP");
                }
                DPRINTFR(FTBRAS, "\n");
            }
            DPRINTFR(FTBRAS, "non-volatile stack:\n");
            for (int i = 0; i < numInflightEntries; i++) {
                DPRINTFR(FTBRAS, "entry [%d] retAddr %#lx, ctr %u nos %d", i, inflightStack[i].data.retAddr, inflightStack[i].data.ctr, inflightStack[i].nos);
                if (TOSW == i) {
                    DPRINTFR(FTBRAS, " <-- TOSW");
                }
                if (TOSR == i) {
                    DPRINTFR(FTBRAS, " <-- TOSR");
                }
                if (BOS == i) {
                    DPRINTFR(FTBRAS, " <-- BOS");
                }
                DPRINTFR(FTBRAS, "\n");
            }
            /*
            DPRINTFR(FTBRAS, "non-volatile stack current data:\n");
            int a = TOSR;
            int inflightCurrentSz = 0;
            while (inflightInRange(a)) {
                DPRINTFR(FTBRAS, "retAddr %#lx, ctr %d\n", inflightStack[a].data.retAddr, inflightStack[a].data.ctr);
                ++inflightCurrentSz;
                a = inflightStack[a].nos;
                if (inflightCurrentSz > 30) {
                    DPRINTFR(FTBRAS, "...\n");
                    break;
                }
            }
            */
            //if (ssp > nsp && (ssp - nsp != inflightCurrentSz)) {
            //    DPRINTFR(FTBRAS, "inflight size mismatch!\n");
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

        RASMeta meta;


};

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_RAS_HH__

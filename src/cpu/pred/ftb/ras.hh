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

        typedef struct RASEntry
        {
            Addr retAddr;
            unsigned ctr;
            RASEntry(Addr retAddr, unsigned ctr) : retAddr(retAddr), ctr(ctr) {}
            RASEntry(Addr retAddr) : retAddr(retAddr), ctr(0) {}
            RASEntry() : retAddr(0), ctr(0) {}
        }RASEntry;

        typedef struct RASMeta {
            int sp;
            RASEntry tos; // top of stack
        }RASMeta;

        void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                          std::vector<FullFTBPrediction> &stagePreds) override;
        
        std::shared_ptr<void> getPredictionMeta() override;

        void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

        unsigned getDelay() override {return 1;}

        void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken) override;

    private:

        void push(Addr retAddr);

        void pop();

        void ptrInc(int &ptr);

        void ptrDec(int &ptr);

        void printStack(const char *when) {
            DPRINTF(FTBRAS, "printStack when %s: \n", when);
            for (int i = 0; i < numEntries; i++) {
                DPRINTFR(FTBRAS, "entry [%d], retAddr %#lx, ctr %d", i, stack[i].retAddr, stack[i].ctr);
                if (sp == i) {
                    DPRINTFR(FTBRAS, " <-- SP");
                }
                DPRINTFR(FTBRAS, "\n");
            }
        }

        unsigned numEntries;

        unsigned ctrWidth;

        int maxCtr;

        int sp;

        std::vector<RASEntry> stack;

        RASMeta meta;


};

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_RAS_HH__

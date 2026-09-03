#ifndef __CPU_PRED_BTB_URAS_HH__
#define __CPU_PRED_BTB_URAS_HH__

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/URAS.hh"
#include "params/BTBuRAS.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

// Forward declare test class as friend
class URASTest;

class BTBuRAS : public TimedBaseBTBPredictor
{
    friend class URASTest;  // Allow test class to access private members

    public:
    
        typedef BTBuRASParams Params;
        BTBuRAS(const Params &p);

        typedef struct uRASEntry
        {
            Addr retAddr;
            unsigned ctr;
            uRASEntry(Addr retAddr, unsigned ctr) : retAddr(retAddr), ctr(ctr) {}
            uRASEntry(Addr retAddr) : retAddr(retAddr), ctr(0) {}
            uRASEntry() : retAddr(0), ctr(0) {}
        }uRASEntry;

        typedef struct uRASMeta {
            int sp;
            uRASEntry tos; // top of stack
        }uRASMeta;

        void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                          std::vector<FullBTBPrediction> &stagePreds) override;
        
        std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;

        void specUpdateState(FullBTBPrediction &pred);

        unsigned getDelay() override {return 0;}

        void recoverState(const FetchTarget &entry);

        void update(const PredictionUpdateContext &context,
                    const PreparedUpdate &update) override;

        int getSp() {return specSp;}

        int getNumEntries() {return numEntries;}

        int getMaxCtr() {return maxCtr;}

        std::vector<uRASEntry> getNonSpecStack() {return nonSpecStack;}

        int getNonSpecSp() {return nonSpecSp;}

        enum When {
            SPECULATIVE,
            REDIRECT,
            COMMIT
        };

        enum RAS_OP {
            PUSH,
            POP,
            RECOVER
            // PUSH_AND_POP
        };

        void push(Addr retAddr, std::vector<uRASEntry> &stack, int &sp);

        void pop(std::vector<uRASEntry> &stack, int &sp);

    private:

        void ptrInc(int &ptr);

        void ptrDec(int &ptr);

        void printStack(const char *when, std::vector<uRASEntry> &stack, int &sp) {
            DPRINTF(URAS, "printStack when %s: \n", when);
            for (int i = 0; i < numEntries; i++) {
                DPRINTFR(URAS, "entry [%d], retAddr %#lx, ctr %d", i, stack[i].retAddr, stack[i].ctr);
                if (sp == i) {
                    DPRINTFR(URAS, " <-- SP");
                }
                DPRINTFR(URAS, "\n");
            }
        }

        void setTrace() override;

        unsigned numEntries;

        unsigned ctrWidth;

        int maxCtr;

        int specSp;

        int nonSpecSp;

        std::vector<uRASEntry> specStack;

        std::vector<uRASEntry> nonSpecStack;

        uRASMeta meta;

        TraceManager *specRasTrace;
        TraceManager *nonSpecRasTrace;

};

struct SpecRASTrace : public Record {
    SpecRASTrace(BTBuRAS::When when, BTBuRAS::RAS_OP op, Addr startPC, Addr brPC,
        Addr retAddr, int sp, Addr tosAddr, unsigned tosCtr)
    {
        _tick = curTick();
        _uint64_data["condition"] = when;
        _uint64_data["op"] = op;
        _uint64_data["startPC"] = startPC;
        _uint64_data["brPC"] = brPC;
        _uint64_data["retAddr"] = retAddr;
        _uint64_data["sp"] = sp;
        _uint64_data["tosAddr"] = tosAddr;
        _uint64_data["tosCtr"] = tosCtr;
    }
};

struct NonSpecRASTrace : public Record {
    NonSpecRASTrace(BTBuRAS::RAS_OP op, Addr startPC, Addr brPC, Addr retAddr,
        int predSp, Addr predTosAddr, unsigned predTosCtr,
        int sp, Addr tosAddr, unsigned tosCtr, bool miss)
    {
        _tick = curTick();
        _uint64_data["op"] = op;
        _uint64_data["startPC"] = startPC;
        _uint64_data["brPC"] = brPC;
        _uint64_data["retAddr"] = retAddr;
        _uint64_data["predSp"] = predSp;
        _uint64_data["predTosAddr"] = predTosAddr;
        _uint64_data["predTosCtr"] = predTosCtr;
        _uint64_data["sp"] = sp;
        _uint64_data["tosAddr"] = tosAddr;
        _uint64_data["tosCtr"] = tosCtr;
        _uint64_data["miss"] = miss;
    }
};

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_BTB_URAS_HH__

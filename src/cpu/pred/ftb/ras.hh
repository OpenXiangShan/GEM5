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

        void update(const FetchStream &entry) override;

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

    private:

        void push(Addr retAddr, std::vector<RASEntry> &stack, int &sp);

        void pop(std::vector<RASEntry> &stack, int &sp);

        void ptrInc(int &ptr);

        void ptrDec(int &ptr);

        void printStack(const char *when, std::vector<RASEntry> &stack, int &sp) {
            DPRINTF(FTBRAS, "printStack when %s: \n", when);
            for (int i = 0; i < numEntries; i++) {
                DPRINTFR(FTBRAS, "entry [%d], retAddr %#lx, ctr %d", i, stack[i].retAddr, stack[i].ctr);
                if (sp == i) {
                    DPRINTFR(FTBRAS, " <-- SP");
                }
                DPRINTFR(FTBRAS, "\n");
            }
        }

        void setTrace() override;

        unsigned numEntries;

        unsigned ctrWidth;

        int maxCtr;

        int specSp;

        int nonSpecSp;

        std::vector<RASEntry> specStack;

        std::vector<RASEntry> nonSpecStack;

        RASMeta meta;

        TraceManager *specRasTrace;
        TraceManager *nonSpecRasTrace;

};

struct SpecRASTrace : public Record {
    SpecRASTrace(RAS::When when, RAS::RAS_OP op, Addr startPC, Addr brPC,
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
    NonSpecRASTrace(RAS::RAS_OP op, Addr startPC, Addr brPC, Addr retAddr,
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

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
#endif  // __CPU_PRED_FTB_RAS_HH__

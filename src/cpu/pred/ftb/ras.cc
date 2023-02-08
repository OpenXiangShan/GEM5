#include "cpu/pred/ftb/ras.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

RAS::RAS(const Params &p)
    : TimedBaseFTBPredictor(p),
    numEntries(p.numEntries),
    ctrWidth(p.ctrWidth)
{
    sp = 0;
    stack.resize(numEntries);
    maxCtr = (1 << ctrWidth) - 1;
    for (auto &entry : stack) {
        entry.ctr = 0;
        entry.retAddr = 0x80000000L;
    }
}

void
RAS::setTrace()
{
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("condition", UINT64),
            std::make_pair("op", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("brPC", UINT64),
            std::make_pair("retAddr", UINT64),
            // before op
            std::make_pair("sp", UINT64),
            std::make_pair("tosAddr", UINT64),
            std::make_pair("tosCtr", UINT64)
        };
        rasTrace = _db->addAndGetTrace("RASTRACE", fields_vec);
        rasTrace->init_table();
    }
}

void
RAS::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullFTBPrediction> &stagePreds)
{
    assert(getDelay() < stagePreds.size());
    for (int i = getDelay(); i < stagePreds.size(); i++) {
        stagePreds[i].returnTarget = stack[sp].retAddr;
    }
    meta.sp = sp;
    meta.tos = stack[sp];
    printStack("putPCHistory");
}

std::shared_ptr<void>
RAS::getPredictionMeta()
{
    std::shared_ptr<void> meta_void_ptr = std::make_shared<RASMeta>(meta);
    return meta_void_ptr;
}

void
RAS::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred)
{
    // do push & pops on prediction
    pred.returnTarget = stack[sp].retAddr;
    auto takenSlot = pred.getTakenSlot();
    if (takenSlot.isCall) {
        Addr retAddr = takenSlot.pc + takenSlot.size;
        RASTrace rec(When::SPECULATIVE, RAS_OP::PUSH, pred.bbStart, takenSlot.pc, retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
        rasTrace->write_record(rec);
        push(retAddr);
    }
    if (takenSlot.isReturn) {
        RASTrace rec(When::SPECULATIVE, RAS_OP::POP, pred.bbStart, takenSlot.pc, stack[sp].retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
        rasTrace->write_record(rec);
        // do pop
        pop();
    }
    printStack("after specUpdateHist");
}

void
RAS::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    printStack("before recoverHist");
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    auto takenSlot = entry.exeBranchInfo;
    RASTrace rec(When::REDIRECT, RAS_OP::RECOVER, entry.startPC, takenSlot.pc, 0, sp, stack[sp].retAddr, stack[sp].ctr);
    rasTrace->write_record(rec);
    sp = meta_ptr->sp;
    stack[sp] = meta_ptr->tos;

    // do push & pops on control squash
    if (takenSlot.isCall) {
        Addr retAddr = takenSlot.pc + takenSlot.size;
        RASTrace rec(When::REDIRECT, RAS_OP::PUSH, entry.startPC, takenSlot.pc, retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
        rasTrace->write_record(rec);
        push(retAddr);
    }
    if (takenSlot.isReturn) {
        RASTrace rec(When::REDIRECT, RAS_OP::POP, entry.startPC, takenSlot.pc, stack[sp].retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
        rasTrace->write_record(rec);
        pop();
    }
    printStack("after recoverHist");
}

void
RAS::push(Addr retAddr)
{
    auto tos = stack[sp];
    if (tos.retAddr == retAddr && tos.ctr < maxCtr) {
        tos.ctr++;
    } else {
        // push new entry
        ptrInc(sp);
        stack[sp].retAddr = retAddr;
        stack[sp].ctr = 0;
    }
}

void
RAS::pop()
{
    auto tos = stack[sp];
    if (tos.ctr > 0) {
        tos.ctr--;
    } else {
        ptrDec(sp);
    }
}

void
RAS::ptrInc(int &ptr)
{
    ptr = (ptr + 1) % numEntries;
}

void
RAS::ptrDec(int &ptr)
{
    if (ptr > 0) {
        ptr--;
    } else {
        assert(ptr == 0);
        ptr = numEntries - 1;
    }
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
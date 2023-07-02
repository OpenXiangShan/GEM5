#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/ftb/ras.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

RAS::RAS(const Params &p)
    : TimedBaseFTBPredictor(p),
    numEntries(p.numEntries),
    ctrWidth(p.ctrWidth),
    numInflightEntries(p.numInflightEntries)
{
    //ssp = numEntries - 1;
    ssp = 0;
    nsp = 0;
    sctr = 0;
    stack.resize(numEntries);
    maxCtr = (1 << ctrWidth) - 1;
    TOSW = 0;
    TOSR = 0;
    inflightPtrDec(TOSR);
    BOS = 0;
    inflightStack.resize(numInflightEntries);
    for (auto &entry : stack) {
        entry.data.ctr = 0;
        entry.data.retAddr = 0x80000000L;
    }
    for (auto &entry : inflightStack) {
        entry.data.ctr = 0;
        entry.data.retAddr = 0x80000000L;
    }
    //ndepth = 0;
}

void
RAS::checkCorrectness() {
    /*
    auto tosr = TOSR;
    int checkssp = ssp;
    while (inflightInRange(tosr)) {
        if (!inflightStack[tosr].data.ctr) {
            checkssp = (checkssp - 1 + numEntries) % numEntries;
        } else {
            // just dec sctr, fixme here
        }
        tosr = inflightStack[tosr].nos;
    }
    if (checkssp != (nsp + numEntries - 1) % numEntries) {
        DPRINTF(FTBRAS, "NSP and SSP check failed\n");
        printStack("checkCorrectness");
    }*/
}

void
RAS::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullFTBPrediction> &stagePreds)
{
    assert(getDelay() < stagePreds.size());
    DPRINTFR(FTBRAS, "putPC startAddr %x", startAddr);
    // checkCorrectness();
    for (int i = getDelay(); i < stagePreds.size(); i++) {
        stagePreds[i].returnTarget = getTop_meta().retAddr; // stack[sp].retAddr;
    }
    /*
    if (stagePreds.back().ftbEntry.slots[0].isCall || stagePreds.back().ftbEntry.slots[0].isReturn || stagePreds.back().ftbEntry.slots[1].isCall || stagePreds.back().ftbEntry.slots[1].isReturn) {
        printStack("putPCHistory");
    }
    */
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
    // pred.returnTarget = stack[sp].retAddr;
    auto takenSlot = pred.getTakenSlot();
    DPRINTFR(FTBRAS, "Do specUpdate for PC %x pred target %x ", pred.bbStart, pred.returnTarget);

    if (takenSlot.isCall) {
        Addr retAddr = takenSlot.pc + takenSlot.size;
        push(retAddr);
    }
    if (takenSlot.isReturn) {
        // do pop
        pop();
    }
    if (takenSlot.isCall) {
        DPRINTFR(FTBRAS, "IsCall spec PC %x\n", takenSlot.pc);
    }
    if (takenSlot.isReturn) {
        DPRINTFR(FTBRAS, "IsRet spec PC %x\n", takenSlot.pc);
    }
    
    if (takenSlot.isCall || takenSlot.isReturn)
        printStack("after specUpdateHist");
    DPRINTFR(FTBRAS, "meta TOSR %d TOSW %d\n", meta.TOSR, meta.TOSW);
}

void
RAS::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    auto takenSlot = entry.exeBranchInfo;
    /*
    if (takenSlot.isCall || takenSlot.isReturn) {
        printStack("before recoverHist");
    }*/
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    DPRINTF(FTBRAS, "recover called, meta TOSR %d TOSW %d ssp %d sctr %u entry PC %x end PC %x\n", meta_ptr->TOSR, meta_ptr->TOSW, meta_ptr->ssp, meta_ptr->sctr, entry.startPC, entry.predEndPC);

    TOSR = meta_ptr->TOSR;
    TOSW = meta_ptr->TOSW;
    ssp = meta_ptr->ssp;
    sctr = meta_ptr->sctr;
    Addr retAddr = takenSlot.pc + takenSlot.size;

    // do push & pops on control squash
    if (entry.exeTaken) {
        if (takenSlot.isCall) {
            push(retAddr);
        }
        if (takenSlot.isReturn) {
            pop();
            //TOSW = (TOSR + 1) % numInflightEntries;
        }
    }

    
    if (entry.exeTaken) {
        DPRINTF(FTBRAS, "isCall %d, isRet %d\n", takenSlot.isCall, takenSlot.isReturn);
        if (takenSlot.isReturn) {
            DPRINTF(FTBRAS, "IsRet expect target %llx, preded %llx, pred taken %d pred target %llx\n", takenSlot.target, meta_ptr->target, entry.predTaken, entry.predBranchInfo.target);
        }
        printStack("after recoverHist");
    }
    
}

void
RAS::update(const FetchStream &entry)
{
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    auto takenSlot = entry.exeBranchInfo;
    if (entry.exeTaken) {
        if (meta_ptr->ssp != nsp || meta_ptr->sctr != stack[nsp].data.ctr) {
            DPRINTF(FTBRAS, "ssp and nsp mismatch, recovering, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n", meta_ptr->ssp, meta_ptr->sctr, nsp, stack[nsp].data.ctr);
            nsp = meta_ptr->ssp;
        } else
            DPRINTF(FTBRAS, "ssp and nsp match, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n", meta_ptr->ssp, meta_ptr->sctr, nsp, stack[nsp].data.ctr);
        if (takenSlot.isCall) {
            DPRINTF(FTBRAS, "real update call FTB hit %d meta TOSR %d TOSW %d\n entry PC %x", entry.isHit, meta_ptr->TOSR, meta_ptr->TOSW, entry.startPC);
            Addr retAddr = takenSlot.pc + takenSlot.size;
            push_stack(retAddr);
            BOS = inflightPtrPlus1(meta_ptr->TOSW);
        }
        if (takenSlot.isReturn) {
            DPRINTF(FTBRAS, "update ret entry PC %x\n", entry.startPC);
            pop_stack();
        }
    }
    if (takenSlot.isCall || takenSlot.isReturn) {
        printStack("after update(commit)");
    }
}

void
RAS::push_stack(Addr retAddr)
{
    auto tos = stack[nsp];
    if (tos.data.retAddr == retAddr && tos.data.ctr < maxCtr) {
        stack[nsp].data.ctr++;
    } else {
        // push new entry
        ptrInc(nsp);
        stack[nsp].data.retAddr = retAddr;
        stack[nsp].data.ctr = 0;
    }
    // ++ndepth;
}

void
RAS::push(Addr retAddr)
{
    DPRINTF(FTBRAS, "doing push ");
    // update ssp and sctr first
    // meta has recorded their old value
    auto topAddr = getTop();
    if (retAddr == topAddr.retAddr && sctr < maxCtr) {
        sctr++;
    } else {
        ptrInc(ssp);
        sctr = 0;
        // do not update non-spec stack here
    }

    // push will always enter inflight queue
    RASInflightEntry t;
    t.data.retAddr = retAddr;
    t.data.ctr = sctr;
    t.nos = TOSR;
    inflightStack[TOSW] = t;
    TOSR = TOSW;
    inflightPtrInc(TOSW);
}

void
RAS::pop_stack()
{
    //if (ndepth) {
    auto tos = stack[nsp];
    if (tos.data.ctr > 0) {
        stack[nsp].data.ctr--;
    } else {
        ptrDec(nsp);
    }
    //--ndepth;
    //} else {
        // unmatched pop, do not move
    //}
    
}

void
RAS::pop()
{
    // DPRINTFR(FTBRAS, "doing pop ndepth = %d", ndepth);

    // pop may need to deal with committed stack
    if (inflightInRange(TOSR)) {
        DPRINTF(FTBRAS, "Select from inflight, addr %x\n", inflightStack[TOSR].data.retAddr);
        TOSR = inflightStack[TOSR].nos;
        if (sctr > 0) {
            sctr--; 
        } else {
            ptrDec(ssp);
            auto newTop = getTop();
            sctr = newTop.ctr;
        }
    } else /*if (ndepth)*/ {
        // TOSR not valid, operate on committed stack
        DPRINTF(FTBRAS, "in committed range\n");
        if (sctr > 0) {
            sctr--;
        } else {
            ptrDec(ssp);
            auto newTop = getTop();
            sctr = newTop.ctr;
        }
    }
    //else {
        // ssp should not move here
    //}
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

void
RAS::inflightPtrInc(int &ptr)
{
    ptr = (ptr + 1) % numInflightEntries;
}

void
RAS::inflightPtrDec(int &ptr)
{
    if (ptr > 0) {
        ptr--;
    } else {
        assert(ptr == 0);
        ptr = numInflightEntries - 1;
    }
}

int
RAS::inflightPtrPlus1(int ptr) {
    return (ptr + 1) % numInflightEntries;
}

bool
RAS::inflightInRange(int &ptr)
{
    if (TOSW > BOS) {
        return ptr >= BOS && ptr < TOSW;
    } else if (TOSW < BOS) {
        return ptr < TOSW || ptr >= BOS;
    } else {
        // empty inflight queue
        return false;
    }
}

RAS::RASEssential
RAS::getTop()
{
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(TOSR)) {
        // result come from inflight queue
        DPRINTF(FTBRAS, "Select from inflight, addr %x\n", inflightStack[TOSR].data.retAddr);
        // additional check: if nos is out of bound, check if commit stack top == inflight[nos]
        /*
        if (!inflightInRange(inflightStack[TOSR].nos)) {
            auto top = stack[nsp];
            if (top.data.retAddr != inflightStack[inflightStack[TOSR].nos].data.retAddr || top.data.ctr != inflightStack[inflightStack[TOSR].nos].data.ctr) {
                // inflight[nos] is not the same as stack[nsp]
                DPRINTF(FTBRAS, "Error: inflight[nos] is not the same as stack[nsp]\n");
                printStack("Error case stack dump");
            }
        }*/

        return inflightStack[TOSR].data;
    } else {
        // result come from commit queue
        DPRINTF(FTBRAS, "Select from stack, addr %x\n", stack[ssp].data.retAddr);
        return stack[ssp].data;
    }
}

RAS::RASEssential
RAS::getTop_meta() {
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(TOSR)) {
        // result come from inflight queue
        DPRINTF(FTBRAS, "Select from inflight, addr %x\n", inflightStack[TOSR].data.retAddr);
        meta.ssp = ssp;
        meta.sctr = sctr;
        meta.TOSR = TOSR;
        meta.TOSW = TOSW;
        meta.target = inflightStack[TOSR].data.retAddr;

        // additional check: if nos is out of bound, check if commit stack top == inflight[nos]
        /*
        if (!inflightInRange(inflightStack[TOSR].nos)) {
            auto top = stack[nsp];
            if (top.data.retAddr != inflightStack[inflightStack[TOSR].nos].data.retAddr || top.data.ctr != inflightStack[inflightStack[TOSR].nos].data.ctr) {
                // inflight[nos] is not the same as stack[nsp]
                DPRINTF(FTBRAS, "Error: inflight[nos] is not the same as stack[nsp]\n");
                printStack("Error case stack dump");
            }
        }*/

        return inflightStack[TOSR].data;
    } else {
        // result come from commit queue
        meta.ssp = ssp;
        meta.sctr = sctr;
        meta.TOSR = TOSR;
        meta.TOSW = TOSW;
        DPRINTF(FTBRAS, "Select from stack, addr %x\n", stack[ssp].data.retAddr);
        meta.target = stack[ssp].data.retAddr;
        return stack[ssp].data;
    }
}

void
RAS::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

Addr
RAS::getTopAddrFromMetas(const FetchStream &stream)
{
    auto meta_ptr = std::static_pointer_cast<RASMeta>(stream.predMetas[getComponentIdx()]);
    return meta_ptr->target;
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
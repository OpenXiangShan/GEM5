#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/ras.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

BTBRAS::BTBRAS(const Params &p)
    : TimedBaseBTBPredictor(p),
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
BTBRAS::checkCorrectness() {
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
        DPRINTF(RAS, "NSP and SSP check failed\n");
        printStack("checkCorrectness");
    }*/
}

void
BTBRAS::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullBTBPrediction> &stagePreds)
{
    assert(getDelay() < stagePreds.size());
    meta = std::make_shared<RASMeta>();
    DPRINTFR(RAS, "putPC startAddr %x", startAddr);
    // checkCorrectness();
    for (int i = getDelay(); i < stagePreds.size(); i++) {
        stagePreds[i].returnTarget = getTop_meta().retAddr; // stack[sp].retAddr;
    }
    /*
    if (stagePreds.back().btbEntry.slots[0].isCall || stagePreds.back().btbEntry.slots[0].isReturn || stagePreds.back().btbEntry.slots[1].isCall || stagePreds.back().btbEntry.slots[1].isReturn) {
        printStack("putPCHistory");
    }
    */
}

std::shared_ptr<void>
BTBRAS::getPredictionMeta()
{
    return meta;
}

void
BTBRAS::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    // do push & pops on prediction
    // pred.returnTarget = stack[sp].retAddr;
    auto takenEntry = pred.getTakenEntry();
    DPRINTFR(RAS, "Do specUpdate for PC %x pred target %x ", pred.bbStart, pred.returnTarget);

    if (takenEntry.isCall) {
        Addr retAddr = takenEntry.pc + takenEntry.size;
        push(retAddr);
    }
    if (takenEntry.isReturn) {
        // do pop
        pop();
    }
    if (takenEntry.isCall) {
        DPRINTFR(RAS, "IsCall spec PC %x\n", takenEntry.pc);
    }
    if (takenEntry.isReturn) {
        DPRINTFR(RAS, "IsRet spec PC %x\n", takenEntry.pc);
    }
    
    if (takenEntry.isCall || takenEntry.isReturn)
        printStack("after specUpdateHist");
    DPRINTFR(RAS, "meta TOSR %d TOSW %d\n", meta->TOSR, meta->TOSW);
}

void
BTBRAS::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    auto takenEntry = entry.exeBranchInfo;
    /*
    if (takenEntry.isCall || takenEntry.isReturn) {
        printStack("before recoverHist");
    }*/
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    DPRINTF(RAS, "recover called, meta TOSR %d TOSW %d ssp %d sctr %u entry PC %x end PC %x\n", meta_ptr->TOSR, meta_ptr->TOSW, meta_ptr->ssp, meta_ptr->sctr, entry.startPC, entry.predEndPC);

    TOSR = meta_ptr->TOSR;
    TOSW = meta_ptr->TOSW;
    ssp = meta_ptr->ssp;
    sctr = meta_ptr->sctr;
    Addr retAddr = takenEntry.pc + takenEntry.size;

    // do push & pops on control squash
    if (entry.exeTaken) {
        if (takenEntry.isCall) {
            push(retAddr);
        }
        if (takenEntry.isReturn) {
            pop();
            //TOSW = (TOSR + 1) % numInflightEntries;
        }
    }

    
    if (entry.exeTaken) {
        DPRINTF(RAS, "isCall %d, isRet %d\n", takenEntry.isCall, takenEntry.isReturn);
        if (takenEntry.isReturn) {
            DPRINTF(RAS, "IsRet expect target %llx, preded %llx, pred taken %d pred target %llx\n", takenEntry.target, meta_ptr->target, entry.predTaken, entry.predBranchInfo.target);
        }
        printStack("after recoverHist");
    }
    
}

void
BTBRAS::update(const FetchStream &entry)
{
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    auto takenEntry = entry.exeBranchInfo;
    if (entry.exeTaken) {
        if (meta_ptr->ssp != nsp || meta_ptr->sctr != stack[nsp].data.ctr) {
            DPRINTF(RAS, "ssp and nsp mismatch, recovering, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n", meta_ptr->ssp, meta_ptr->sctr, nsp, stack[nsp].data.ctr);
            nsp = meta_ptr->ssp;
        } else
            DPRINTF(RAS, "ssp and nsp match, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n", meta_ptr->ssp, meta_ptr->sctr, nsp, stack[nsp].data.ctr);
        if (takenEntry.isCall) {
            DPRINTF(RAS, "real update call BTB hit %d meta TOSR %d TOSW %d\n entry PC %x", entry.isHit, meta_ptr->TOSR, meta_ptr->TOSW, entry.startPC);
            Addr retAddr = takenEntry.pc + takenEntry.size;
            push_stack(retAddr);
            BOS = inflightPtrPlus1(meta_ptr->TOSW);
        }
        if (takenEntry.isReturn) {
            DPRINTF(RAS, "update ret entry PC %x\n", entry.startPC);
            pop_stack();
        }
    }
    if (takenEntry.isCall || takenEntry.isReturn) {
        printStack("after update(commit)");
    }
}

void
BTBRAS::push_stack(Addr retAddr)
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
BTBRAS::push(Addr retAddr)
{
    DPRINTF(RAS, "doing push ");
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
BTBRAS::pop_stack()
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
BTBRAS::pop()
{
    // DPRINTFR(RAS, "doing pop ndepth = %d", ndepth);

    // pop may need to deal with committed stack
    if (inflightInRange(TOSR)) {
        DPRINTF(RAS, "Select from inflight, addr %x\n", inflightStack[TOSR].data.retAddr);
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
        DPRINTF(RAS, "in committed range\n");
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
BTBRAS::ptrInc(int &ptr)
{
    ptr = (ptr + 1) % numEntries;
}

void
BTBRAS::ptrDec(int &ptr)
{
    if (ptr > 0) {
        ptr--;
    } else {
        assert(ptr == 0);
        ptr = numEntries - 1;
    }
}

void
BTBRAS::inflightPtrInc(int &ptr)
{
    ptr = (ptr + 1) % numInflightEntries;
}

void
BTBRAS::inflightPtrDec(int &ptr)
{
    if (ptr > 0) {
        ptr--;
    } else {
        assert(ptr == 0);
        ptr = numInflightEntries - 1;
    }
}

int
BTBRAS::inflightPtrPlus1(int ptr) {
    return (ptr + 1) % numInflightEntries;
}

bool
BTBRAS::inflightInRange(int &ptr)
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

BTBRAS::RASEssential
BTBRAS::getTop()
{
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(TOSR)) {
        // result come from inflight queue
        DPRINTF(RAS, "Select from inflight, addr %x\n", inflightStack[TOSR].data.retAddr);
        // additional check: if nos is out of bound, check if commit stack top == inflight[nos]
        /*
        if (!inflightInRange(inflightStack[TOSR].nos)) {
            auto top = stack[nsp];
            if (top.data.retAddr != inflightStack[inflightStack[TOSR].nos].data.retAddr || top.data.ctr != inflightStack[inflightStack[TOSR].nos].data.ctr) {
                // inflight[nos] is not the same as stack[nsp]
                DPRINTF(RAS, "Error: inflight[nos] is not the same as stack[nsp]\n");
                printStack("Error case stack dump");
            }
        }*/

        return inflightStack[TOSR].data;
    } else {
        // result come from commit queue
        DPRINTF(RAS, "Select from stack, addr %x\n", stack[ssp].data.retAddr);
        return stack[ssp].data;
    }
}

BTBRAS::RASEssential
BTBRAS::getTop_meta() {
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(TOSR)) {
        // result come from inflight queue
        DPRINTF(RAS, "Select from inflight, addr %x\n", inflightStack[TOSR].data.retAddr);
        meta->ssp = ssp;
        meta->sctr = sctr;
        meta->TOSR = TOSR;
        meta->TOSW = TOSW;
        meta->target = inflightStack[TOSR].data.retAddr;

        // additional check: if nos is out of bound, check if commit stack top == inflight[nos]
        /*
        if (!inflightInRange(inflightStack[TOSR].nos)) {
            auto top = stack[nsp];
            if (top.data.retAddr != inflightStack[inflightStack[TOSR].nos].data.retAddr || top.data.ctr != inflightStack[inflightStack[TOSR].nos].data.ctr) {
                // inflight[nos] is not the same as stack[nsp]
                DPRINTF(RAS, "Error: inflight[nos] is not the same as stack[nsp]\n");
                printStack("Error case stack dump");
            }
        }*/

        return inflightStack[TOSR].data;
    } else {
        // result come from commit queue
        meta->ssp = ssp;
        meta->sctr = sctr;
        meta->TOSR = TOSR;
        meta->TOSW = TOSW;
        meta->target = stack[ssp].data.retAddr;
        DPRINTF(RAS, "Select from stack, addr %x\n", stack[ssp].data.retAddr);
        return stack[ssp].data;
    }
}

void
BTBRAS::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

Addr
BTBRAS::getTopAddrFromMetas(const FetchStream &stream)
{
    auto meta_ptr = std::static_pointer_cast<RASMeta>(stream.predMetas[getComponentIdx()]);
    return meta_ptr->target;
}


}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
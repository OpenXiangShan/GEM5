#include "cpu/pred/btb/ras.hh"

// Additional conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "cpu/o3/dyn_inst.hh"
#endif

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

// Constructor implementations based on build mode
#ifdef UNIT_TEST
    namespace test {
        // Test constructor for unit testing mode
        BTBRAS::BTBRAS(unsigned numEntries, unsigned ctrWidth, unsigned numInflightEntries)
            : TimedBaseBTBPredictor(),
              numEntries(numEntries),
              ctrWidth(ctrWidth),
              numInflightEntries(numInflightEntries),
              maxCtr((1 << ctrWidth) - 1),
              numThreads(1),
              threadStates(numThreads)
        {
            for (auto &state : threadStates) {
                initThreadState(state);
            }
        }
#else
    // Production constructor
    BTBRAS::BTBRAS(const Params &p)
        : TimedBaseBTBPredictor(p),
          numEntries(p.numEntries),
          ctrWidth(p.ctrWidth),
          numInflightEntries(p.numInflightEntries),
          maxCtr((1 << ctrWidth) - 1),
          numThreads(p.numThreads),
          threadStates(numThreads),
          rasStats(this)
    {
        for (auto &state : threadStates) {
            initThreadState(state);
        }
    }
#endif

void
BTBRAS::initThreadState(ThreadRASState &state)
{
    state.TOSW = 0;
    state.TOSR = 0;
    inflightPtrDec(state.TOSR);
    state.BOS = 0;
    state.ssp = 0;
    state.nsp = 0;
    state.sctr = 0;
    state.meta.reset();

    state.stack.resize(numEntries);
    state.inflightStack.resize(numInflightEntries);

    for (auto &entry : state.stack) {
        entry.data.ctr = 0;
        entry.data.retAddr = 0x80000000L;
    }
    for (auto &entry : state.inflightStack) {
        entry.data.ctr = 0;
        entry.data.retAddr = 0x80000000L;
        entry.nos = 0;
    }
}

void
BTBRAS::checkCorrectness(ThreadID tid) {
    auto &state = threadStates[tid];
    /*
    auto tosr = state.TOSR;
    int checkssp = state.ssp;
    while (inflightInRange(state, tosr)) {
        if (!state.inflightStack[tosr].data.ctr) {
            checkssp = (checkssp - 1 + numEntries) % numEntries;
        } else {
            // just dec sctr, fixme here
        }
        tosr = state.inflightStack[tosr].nos;
    }
    if (checkssp != (state.nsp + numEntries - 1) % numEntries) {
        DPRINTF(RAS, "NSP and SSP check failed\n");
        printStack("checkCorrectness", tid);
    }*/
}

void
BTBRAS::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullBTBPrediction> &stagePreds)
{
    assert(getDelay() < stagePreds.size());
    const ThreadID tid = stagePreds.back().tid;
    assert(tid < numThreads);
    auto &state = threadStates[tid];
    state.meta = std::make_shared<RASMeta>();
    DPRINTFR(RAS, "putPC startAddr %lx", startAddr);
    // checkCorrectness(tid);
    auto top = getTop_meta(tid);
    for (int i = getDelay(); i < stagePreds.size(); i++) {
        stagePreds[i].returnTarget = top.retAddr;
    }
    /*
    if (stagePreds.back().btbEntry.slots[0].isCall || stagePreds.back().btbEntry.slots[0].isReturn || stagePreds.back().btbEntry.slots[1].isCall || stagePreds.back().btbEntry.slots[1].isReturn) {
        printStack("putPCHistory", tid);
    }
    */
}

std::shared_ptr<void>
BTBRAS::getPredictionMeta(ThreadID tid)
{
    if (tid >= threadStates.size()) {
        return nullptr;
    }
    return threadStates[tid].meta;
}

void
BTBRAS::specUpdateState(FullBTBPrediction &pred)
{
    const ThreadID tid = pred.tid;
    assert(tid < numThreads);
    auto &state = threadStates[tid];
    assert(state.meta);
    // do push & pops on prediction
    // pred.returnTarget = stack[sp].retAddr;
    auto takenEntry = pred.getTakenEntry();
    DPRINTFR(RAS, "Do specUpdate for PC %lx pred target %lx ", pred.bbStart, pred.returnTarget);

    if (takenEntry.isCall) {
        Addr retAddr = takenEntry.pc + takenEntry.size;
        push(tid, retAddr);
    }
    if (takenEntry.isReturn) {
        // do pop
        pop(tid);
    }
    if (takenEntry.isCall) {
        DPRINTFR(RAS, "IsCall spec PC %lx\n", takenEntry.pc);
    }
    if (takenEntry.isReturn) {
        DPRINTFR(RAS, "IsRet spec PC %lx\n", takenEntry.pc);
    }
    
    if (takenEntry.isCall || takenEntry.isReturn)
        printStack("after specUpdateState", tid);
    DPRINTFR(RAS, "meta TOSR %d TOSW %d\n", state.meta->TOSR, state.meta->TOSW);
}

void
BTBRAS::recoverState(
    const FetchTarget &entry,
    const ResolvedBranch &actual_branch)
{
    const ThreadID tid = entry.tid;
    assert(tid < numThreads);
    auto &state = threadStates[tid];
    /*
    if (actual_branch.isCall || actual_branch.isReturn) {
        printStack("before recoverState", tid);
    }*/
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    DPRINTF(RAS, "recover called, meta TOSR %d TOSW %d ssp %d sctr %u entry PC %lx end PC %lx\n",
        meta_ptr->TOSR, meta_ptr->TOSW, meta_ptr->ssp, meta_ptr->sctr, entry.startPC, entry.predEndPC);

    state.TOSR = meta_ptr->TOSR;
    state.TOSW = meta_ptr->TOSW;
    state.ssp = meta_ptr->ssp;
    state.sctr = meta_ptr->sctr;
    Addr retAddr = actual_branch.pc + actual_branch.size;

    // do push & pops on control squash
    if (actual_branch.taken) {
        if (actual_branch.isCall) {
            push(tid, retAddr);
        }
        if (actual_branch.isReturn) {
            pop(tid);
            //TOSW = (TOSR + 1) % numInflightEntries;
        }
    }

    
    if (actual_branch.taken) {
        DPRINTF(RAS, "isCall %d, isRet %d\n", actual_branch.isCall, actual_branch.isReturn);
        if (actual_branch.isReturn) {
            DPRINTF(RAS, "IsRet expect target %lx, preded %lx, pred taken %d pred target %lx\n",
                actual_branch.target, meta_ptr->target, entry.predTaken, entry.predBranchInfo.target);
        }
        printStack("after recoverState", tid);
    }

}

void
BTBRAS::updateWithBranchUpdateContext(
    const BranchUpdateContext &ctx,
    const std::vector<ResolvedBranch> &update_branches,
    const std::shared_ptr<void> &prediction_meta)
{
    const ThreadID tid = ctx.tid;
    assert(tid < numThreads);
    auto &state = threadStates[tid];
    auto meta_ptr = std::static_pointer_cast<RASMeta>(prediction_meta);
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);
    const bool actual_taken = summary_branch && summary_branch->taken;
    if (actual_taken) {
        if (meta_ptr->ssp != state.nsp || meta_ptr->sctr != state.stack[state.nsp].data.ctr) {
            DPRINTF(RAS, "ssp and nsp mismatch, recovering, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n",
                meta_ptr->ssp, meta_ptr->sctr, state.nsp, state.stack[state.nsp].data.ctr);
            state.nsp = meta_ptr->ssp;
        } else
            DPRINTF(RAS, "ssp and nsp match, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n",
                meta_ptr->ssp, meta_ptr->sctr, state.nsp, state.stack[state.nsp].data.ctr);
        if (summary_branch->isCall) {
            DPRINTF(RAS, "real update call meta TOSR %d TOSW %d\n entry PC %lx",
                meta_ptr->TOSR, meta_ptr->TOSW, ctx.startPC);
            Addr retAddr = summary_branch->pc + summary_branch->size;
            push_stack(tid, retAddr);
            state.BOS = inflightPtrPlus1(meta_ptr->TOSW);
        }
        if (summary_branch->isReturn) {
            DPRINTF(RAS, "update ret entry PC %lx\n", ctx.startPC);
            pop_stack(tid);
        }
    }
    if (summary_branch && (summary_branch->isCall || summary_branch->isReturn)) {
        printStack("after update(commit)", tid);
    }
}

void
BTBRAS::push_stack(ThreadID tid, Addr retAddr)
{
    auto &state = threadStates[tid];
    auto tos = state.stack[state.nsp];
    if (tos.data.retAddr == retAddr && tos.data.ctr < maxCtr) {
        state.stack[state.nsp].data.ctr++;
    } else {
        // push new entry
        ptrInc(state.nsp);
        state.stack[state.nsp].data.retAddr = retAddr;
        state.stack[state.nsp].data.ctr = 0;
    }
    // ++ndepth;
}

void
BTBRAS::push(ThreadID tid, Addr retAddr)
{
    auto &state = threadStates[tid];
    rasStats.Pushes++;
    DPRINTF(RAS, "doing push ");
    // update ssp and sctr first
    // meta has recorded their old value
    auto topAddr = getTop(tid);
    if (retAddr == topAddr.retAddr && state.sctr < maxCtr) {
        state.sctr++;
    } else {
        ptrInc(state.ssp);
        state.sctr = 0;
        // do not update non-spec stack here
    }

    // push will always enter inflight queue
    RASInflightEntry t;
    t.data.retAddr = retAddr;
    t.data.ctr = state.sctr;
    t.nos = state.TOSR;
    state.inflightStack[state.TOSW] = t;
    state.TOSR = state.TOSW;
    inflightPtrInc(state.TOSW);
}

void
BTBRAS::pop_stack(ThreadID tid)
{
    auto &state = threadStates[tid];
    //if (ndepth) {
    auto tos = state.stack[state.nsp];
    if (tos.data.ctr > 0) {
        state.stack[state.nsp].data.ctr--;
    } else {
        ptrDec(state.nsp);
    }
    //--ndepth;
    //} else {
        // unmatched pop, do not move
    //}
    
}

void
BTBRAS::pop(ThreadID tid)
{
    auto &state = threadStates[tid];
    // DPRINTFR(RAS, "doing pop ndepth = %d", ndepth);
    rasStats.Pops++;
    // pop may need to deal with committed stack
    if (inflightInRange(state, state.TOSR)) {
        DPRINTF(RAS, "Select from inflight, addr %lx\n", state.inflightStack[state.TOSR].data.retAddr);
        state.TOSR = state.inflightStack[state.TOSR].nos;
        if (state.sctr > 0) {
            state.sctr--;
        } else {
            ptrDec(state.ssp);
            auto newTop = getTop(tid);
            state.sctr = newTop.ctr;
        }
    } else /*if (ndepth)*/ {
        // TOSR not valid, operate on committed stack
        DPRINTF(RAS, "in committed range\n");
        if (state.sctr > 0) {
            state.sctr--;
        } else {
            ptrDec(state.ssp);
            auto newTop = getTop(tid);
            state.sctr = newTop.ctr;
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
BTBRAS::inflightInRange(const ThreadRASState &state, int ptr)
{
    if (state.TOSW > state.BOS) {
        return ptr >= state.BOS && ptr < state.TOSW;
    } else if (state.TOSW < state.BOS) {
        return ptr < state.TOSW || ptr >= state.BOS;
    } else {
        // empty inflight queue
        return false;
    }
}

BTBRAS::RASEssential
BTBRAS::getTop(ThreadID tid)
{
    auto &state = threadStates[tid];
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(state, state.TOSR)) {
        // result come from inflight queue
        DPRINTF(RAS, "Select from inflight, addr %lx\n",
                state.inflightStack[state.TOSR].data.retAddr);
        // additional check: if nos is out of bound, check if commit stack top == inflight[nos]
        /*
        if (!inflightInRange(state, state.inflightStack[state.TOSR].nos)) {
            auto top = state.stack[state.nsp];
            if (top.data.retAddr !=
                    state.inflightStack[
                        state.inflightStack[state.TOSR].nos].data.retAddr ||
                top.data.ctr !=
                    state.inflightStack[
                        state.inflightStack[state.TOSR].nos].data.ctr) {
                // inflight[nos] is not the same as stack[nsp]
                DPRINTF(RAS, "Error: inflight[nos] is not the same as stack[nsp]\n");
                printStack("Error case stack dump", tid);
            }
        }*/

        return state.inflightStack[state.TOSR].data;
    } else {
        // result come from commit queue
        DPRINTF(RAS, "Select from stack, addr %lx\n", state.stack[state.ssp].data.retAddr);
        return state.stack[state.ssp].data;
    }
}

BTBRAS::RASEssential
BTBRAS::getTop_meta(ThreadID tid) {
    auto &state = threadStates[tid];
    assert(state.meta);
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(state, state.TOSR)) {
        // result come from inflight queue
        DPRINTF(RAS, "Select from inflight, addr %lx\n",
                state.inflightStack[state.TOSR].data.retAddr);
        state.meta->ssp = state.ssp;
        state.meta->sctr = state.sctr;
        state.meta->TOSR = state.TOSR;
        state.meta->TOSW = state.TOSW;
        state.meta->target = state.inflightStack[state.TOSR].data.retAddr;

        // additional check: if nos is out of bound, check if commit stack top == inflight[nos]
        /*
        if (!inflightInRange(state, state.inflightStack[state.TOSR].nos)) {
            auto top = state.stack[state.nsp];
            if (top.data.retAddr !=
                    state.inflightStack[
                        state.inflightStack[state.TOSR].nos].data.retAddr ||
                top.data.ctr !=
                    state.inflightStack[
                        state.inflightStack[state.TOSR].nos].data.ctr) {
                // inflight[nos] is not the same as stack[nsp]
                DPRINTF(RAS, "Error: inflight[nos] is not the same as stack[nsp]\n");
                printStack("Error case stack dump", tid);
            }
        }*/

        return state.inflightStack[state.TOSR].data;
    } else {
        // result come from commit queue
        state.meta->ssp = state.ssp;
        state.meta->sctr = state.sctr;
        state.meta->TOSR = state.TOSR;
        state.meta->TOSW = state.TOSW;
        state.meta->target = state.stack[state.ssp].data.retAddr;
        DPRINTF(RAS, "Select from stack, addr %lx\n", state.stack[state.ssp].data.retAddr);
        return state.stack[state.ssp].data;
    }
}

Addr
BTBRAS::getTopAddrFromMeta(const std::shared_ptr<void> &prediction_meta)
{
    auto meta_ptr = std::static_pointer_cast<RASMeta>(prediction_meta);
    return meta_ptr->target;
}

void
BTBRAS::recordCommittedBranchStats(
    const ResolvedBranch &branch,
    const std::shared_ptr<void> &prediction_meta)
{
    if (!branch.isReturn || branch.isNop) {
        // ras only cares about return instructions
        return;
    }
    auto meta = std::static_pointer_cast<RASMeta>(prediction_meta);
    auto npc = branch.target;
    if (npc != meta->target) {
        rasStats.PredWrong++;
        if (meta->sctr) {
            rasStats.MispredWithSctr++;
        }
    } else {
        rasStats.PredCorrect++;
        if (meta->sctr) {
            rasStats.CorrectWithSctr++;
        }
    }
}

#ifndef UNIT_TEST
BTBRAS::RASStats::RASStats(statistics::Group *parent):
    statistics::Group(parent),
    ADD_STAT(PredWrong, statistics::units::Count::get(),"number of RAS mispredictions"),
    ADD_STAT(MispredWithSctr, statistics::units::Count::get(),"number of RAS mispredictions when sctr > 0"),
    ADD_STAT(PredCorrect, statistics::units::Count::get(),"number of RAS correct predictions"),
    ADD_STAT(CorrectWithSctr, statistics::units::Count::get(),"number of RAS correct predictions when sctr > 0"),

    ADD_STAT(Pushes, statistics::units::Count::get(),"number of RAS pushes"),
    ADD_STAT(Pops, statistics::units::Count::get(),"number of RAS pops")

{}

#endif
// Close conditional namespaces
#ifdef UNIT_TEST
    } // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

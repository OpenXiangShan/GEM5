#include "cpu/pred/btb/ras.hh"

#include <algorithm>

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
              threadStates(numThreads),
              rasStats()
        {
            assert(numInflightEntries >= 2);
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
        assert(numInflightEntries >= 2);
        for (auto &state : threadStates) {
            initThreadState(state);
        }
    }
#endif

void
BTBRAS::initThreadState(ThreadRASState &state)
{
    state.TOSW = 0;
    state.TOSR = -1;
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
BTBRAS::refreshPredictionMeta(Addr startAddr,
                              const boost::dynamic_bitset<> &history,
                              FullBTBPrediction &pred)
{
    (void)startAddr;
    (void)history;
    auto &state = threadStates[pred.tid];
    state.meta = std::make_shared<RASMeta>();
    getTop_meta(pred.tid);
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

    if ((takenEntry.isCall || takenEntry.isReturn) &&
        inflightNearOverflow(state)) {
        rasStats.SpecUpdatesBlockedNearOverflow++;
        DPRINTF(RAS, "Block speculative RAS update near inflight overflow\n");
        return;
    }

    // RISC-V JALR PopAndPush has both flags set; pop first to retain the new return address.
    if (takenEntry.isReturn) {
        // do pop
        pop(tid);
    }
    if (takenEntry.isCall) {
        Addr retAddr = takenEntry.pc + takenEntry.size;
        push(tid, retAddr);
    }
    if (takenEntry.isCall) {
        DPRINTFR(RAS, "IsCall spec PC %lx\n", takenEntry.pc);
    }
    if (takenEntry.isReturn) {
        DPRINTFR(RAS, "IsRet spec PC %lx\n", takenEntry.pc);
    }
    
    if (takenEntry.isCall || takenEntry.isReturn)
        printStack("after specUpdateState", tid);
    DPRINTFR(RAS, "meta TOSR %lld TOSW %lld\n",
             static_cast<long long>(state.meta->TOSR),
             static_cast<long long>(state.meta->TOSW));
}

void
BTBRAS::recoverState(const FetchTarget &entry)
{
    const ThreadID tid = entry.tid;
    assert(tid < numThreads);
    auto &state = threadStates[tid];
    auto takenEntry = entry.exeBranchInfo;
    /*
    if (takenEntry.isCall || takenEntry.isReturn) {
        printStack("before recoverState", tid);
    }*/
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    DPRINTF(RAS, "recover called, meta TOSR %lld TOSW %lld ssp %d sctr %u entry PC %lx end PC %lx\n",
        static_cast<long long>(meta_ptr->TOSR),
        static_cast<long long>(meta_ptr->TOSW), meta_ptr->ssp,
        meta_ptr->sctr, entry.startPC, entry.predEndPC);

    // RTL only accepts a redirect near overflow when it rolls the speculative
    // write pointer back. This prevents a redirect on the current queue head
    // from consuming the final ring entry.
    if (inflightNearOverflow(state) && meta_ptr->TOSW >= state.TOSW) {
        rasStats.RedirectsBlockedNearOverflow++;
        DPRINTF(RAS, "Block RAS redirect recovery near inflight overflow\n");
        return;
    }

    state.TOSR = meta_ptr->TOSR;
    state.TOSW = meta_ptr->TOSW;
    state.ssp = meta_ptr->ssp;
    state.sctr = meta_ptr->sctr;
    Addr retAddr = takenEntry.pc + takenEntry.size;

    // do push & pops on control squash
    if (entry.exeTaken) {
        // RISC-V JALR PopAndPush has both flags set; pop first to retain the new return address.
        if (takenEntry.isReturn) {
            pop(tid);
            //TOSW = (TOSR + 1) % numInflightEntries;
        }
        if (takenEntry.isCall) {
            push(tid, retAddr);
        }
    }

    
    if (entry.exeTaken) {
        DPRINTF(RAS, "isCall %d, isRet %d\n", takenEntry.isCall, takenEntry.isReturn);
        if (takenEntry.isReturn) {
            DPRINTF(RAS, "IsRet expect target %lx, preded %lx, pred taken %d pred target %lx\n",
                takenEntry.target, meta_ptr->target, entry.predTaken, entry.predBranchInfo.target);
        }
        printStack("after recoverState", tid);
    }

}

void
BTBRAS::update(const FetchTarget &entry)
{
    const ThreadID tid = entry.tid;
    assert(tid < numThreads);
    auto &state = threadStates[tid];
    auto meta_ptr = std::static_pointer_cast<RASMeta>(entry.predMetas[getComponentIdx()]);
    auto takenEntry = entry.exeBranchInfo;
    if (entry.exeTaken) {
        if (meta_ptr->ssp != state.nsp || meta_ptr->sctr != state.stack[state.nsp].data.ctr) {
            DPRINTF(RAS, "ssp and nsp mismatch, recovering, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n",
                meta_ptr->ssp, meta_ptr->sctr, state.nsp, state.stack[state.nsp].data.ctr);
            state.nsp = meta_ptr->ssp;
        } else
            DPRINTF(RAS, "ssp and nsp match, ssp = %d, sctr = %d, nsp = %d, nctr = %d\n",
                meta_ptr->ssp, meta_ptr->sctr, state.nsp, state.stack[state.nsp].data.ctr);
        // RISC-V JALR PopAndPush has both flags set; pop first to retain the new return address.
        if (takenEntry.isReturn) {
            DPRINTF(RAS, "update ret entry PC %lx\n", entry.startPC);
            pop_stack(tid);
        }
        if (takenEntry.isCall) {
            DPRINTF(RAS, "real update call BTB hit %d meta TOSR %lld TOSW %lld\n entry PC %lx",
                entry.isHit, static_cast<long long>(meta_ptr->TOSR),
                static_cast<long long>(meta_ptr->TOSW), entry.startPC);
            Addr retAddr = takenEntry.pc + takenEntry.size;
            push_stack(tid, retAddr);
        }
    }

    // Match the RTL inference-queue retirement window. A committed push stays
    // available as the oldest speculative entry because younger entries may
    // still name it as their parent. Other commits may reclaim all but one
    // predecessor once their prediction metadata has moved far enough ahead.
    if (entry.exeTaken && takenEntry.isCall) {
        state.BOS = std::max(state.BOS, meta_ptr->TOSW);
    } else if (meta_ptr->TOSW - state.BOS > 2) {
        state.BOS = std::max(state.BOS, meta_ptr->TOSW - 1);
    }
    if (takenEntry.isCall || takenEntry.isReturn) {
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
    state.inflightStack[inflightIndex(state.TOSW)] = t;
    state.TOSR = state.TOSW;
    state.TOSW++;
    recordInflightDepth(state);
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
        const auto top_idx = inflightIndex(state.TOSR);
        DPRINTF(RAS, "Select from inflight, addr %lx\n",
                state.inflightStack[top_idx].data.retAddr);
        state.TOSR = state.inflightStack[top_idx].nos;
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

unsigned
BTBRAS::inflightIndex(int64_t ptr) const
{
    assert(ptr >= 0);
    return ptr % numInflightEntries;
}

uint64_t
BTBRAS::inflightOccupancy(const ThreadRASState &state) const
{
    assert(state.TOSW >= state.BOS);
    return state.TOSW - state.BOS;
}

bool
BTBRAS::inflightNearOverflow(const ThreadRASState &state) const
{
    return inflightOccupancy(state) > numInflightEntries - 2;
}

bool
BTBRAS::inflightInRange(const ThreadRASState &state, int64_t ptr) const
{
    return ptr >= state.BOS && ptr < state.TOSW;
}

void
BTBRAS::recordInflightDepth(const ThreadRASState &state)
{
    const auto depth = inflightOccupancy(state);
#ifdef UNIT_TEST
    rasStats.MaxInflightDepth = std::max(rasStats.MaxInflightDepth, depth);
#else
    rasStats.MaxInflightDepth =
        std::max(rasStats.MaxInflightDepth.value(), static_cast<double>(depth));
#endif
}

BTBRAS::RASEssential
BTBRAS::getTop(ThreadID tid)
{
    auto &state = threadStates[tid];
    // results may come from two sources: inflight queue and committed stack
    if (inflightInRange(state, state.TOSR)) {
        // result come from inflight queue
        DPRINTF(RAS, "Select from inflight, addr %lx\n",
                state.inflightStack[inflightIndex(state.TOSR)].data.retAddr);
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

        return state.inflightStack[inflightIndex(state.TOSR)].data;
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
                state.inflightStack[inflightIndex(state.TOSR)].data.retAddr);
        state.meta->ssp = state.ssp;
        state.meta->sctr = state.sctr;
        state.meta->TOSR = state.TOSR;
        state.meta->TOSW = state.TOSW;
        state.meta->target =
            state.inflightStack[inflightIndex(state.TOSR)].data.retAddr;

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

        return state.inflightStack[inflightIndex(state.TOSR)].data;
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
BTBRAS::getTopAddrFromMetas(const FetchTarget &stream)
{
    auto meta_ptr = std::static_pointer_cast<RASMeta>(stream.predMetas[getComponentIdx()]);
    return meta_ptr->target;
}

#ifndef UNIT_TEST
void
BTBRAS::commitBranch(const FetchTarget &stream, const DynInstPtr &inst)
{
    if (!inst->isReturn() || inst->isNop()) {
        // ras only cares about return instructions
        return;
    }
    auto meta = std::static_pointer_cast<RASMeta>(stream.predMetas[getComponentIdx()]);
    auto npc = inst->getNPC();
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
#endif

#ifndef UNIT_TEST
BTBRAS::RASStats::RASStats(statistics::Group *parent):
    statistics::Group(parent),
    ADD_STAT(PredWrong, statistics::units::Count::get(),"number of RAS mispredictions"),
    ADD_STAT(MispredWithSctr, statistics::units::Count::get(),"number of RAS mispredictions when sctr > 0"),
    ADD_STAT(PredCorrect, statistics::units::Count::get(),"number of RAS correct predictions"),
    ADD_STAT(CorrectWithSctr, statistics::units::Count::get(),"number of RAS correct predictions when sctr > 0"),

    ADD_STAT(Pushes, statistics::units::Count::get(),"number of RAS pushes"),
    ADD_STAT(Pops, statistics::units::Count::get(),"number of RAS pops"),
    ADD_STAT(SpecUpdatesBlockedNearOverflow,
             statistics::units::Count::get(),
             "number of speculative RAS updates blocked near queue overflow"),
    ADD_STAT(RedirectsBlockedNearOverflow,
             statistics::units::Count::get(),
             "number of RAS redirect recoveries blocked near queue overflow"),
    ADD_STAT(MaxInflightDepth, statistics::units::Count::get(),
             "maximum number of occupied speculative RAS entries")

{}

#endif
// Close conditional namespaces
#ifdef UNIT_TEST
    } // namespace test
#endif

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

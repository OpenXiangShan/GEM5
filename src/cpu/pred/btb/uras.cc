#include "cpu/pred/btb/uras.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred {

BTBuRAS::BTBuRAS(const Params &p)
    : TimedBaseBTBPredictor(p),
    numEntries(p.numEntries),
    ctrWidth(p.ctrWidth)
{
    maxCtr = (1 << ctrWidth) - 1;
    // init spec stack
    specSp = 0;
    specStack.resize(numEntries);
    for (auto &entry : specStack) {
        entry.ctr = 0;
        entry.retAddr = 0x80000000L;
    }
    // init non-spec stack
    nonSpecSp = 0;
    nonSpecStack.resize(numEntries);
    for (auto &entry : nonSpecStack) {
        entry.ctr = 0;
        entry.retAddr = 0x80000000L;
    }
}

void
BTBuRAS::setTrace()
{
    if (enableDB) {
        // record every modification to the spec-stack
        std::vector<std::pair<std::string, DataType>> spec_fields_vec = {
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
        specRasTrace = _db->addAndGetTrace("SPECRASTRACE", spec_fields_vec);
        specRasTrace->init_table();

        // record every modification to the non-spec-stack, used as reference model
        std::vector<std::pair<std::string, DataType>> nonspec_fields_vec = {
            // real info
            std::make_pair("op", UINT64),
            std::make_pair("startPC", UINT64),
            std::make_pair("brPC", UINT64),
            std::make_pair("retAddr", UINT64),
            // prediction info
            std::make_pair("predSp", UINT64),
            std::make_pair("predTosAddr", UINT64),
            std::make_pair("predTosCtr", UINT64),
            // before op
            std::make_pair("sp", UINT64),
            std::make_pair("tosAddr", UINT64),
            std::make_pair("tosCtr", UINT64),
            std::make_pair("miss", UINT64)
        };
        nonSpecRasTrace = _db->addAndGetTrace("NONSPECRASTRACE", nonspec_fields_vec);
        nonSpecRasTrace->init_table();
    }
}

void
BTBuRAS::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullBTBPrediction> &stagePreds)
{
    auto &stack = specStack;
    auto &sp = specSp;
    assert(getDelay() < stagePreds.size());
    for (int i = getDelay(); i < stagePreds.size(); i++) {
        //stagePreds[i].returnTarget = stack[sp].retAddr;
    }
    meta.sp = sp;
    meta.tos = stack[sp];
    printStack("putPCHistory", stack, sp);
}

std::shared_ptr<void>
BTBuRAS::getPredictionMeta(ThreadID tid)
{
    (void)tid;
    std::shared_ptr<void> meta_void_ptr = std::make_shared<uRASMeta>(meta);
    return meta_void_ptr;
}

void
BTBuRAS::specUpdateState(FullBTBPrediction &pred)
{
    auto &stack = specStack;
    auto &sp = specSp;
    // do push & pops on prediction
    pred.returnTarget = stack[sp].retAddr;
    auto takenSlot = pred.getTakenEntry();
    if (takenSlot.isCall) {
        Addr retAddr = takenSlot.pc + takenSlot.size;
        if (enableDB) {
            SpecRASTrace rec(When::SPECULATIVE, RAS_OP::PUSH, pred.bbStart, takenSlot.pc,
                retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
            specRasTrace->write_record(rec);
        }
        DPRINTF(URAS, "spec stack push addr 0x%llx\n", retAddr);
        push(retAddr, stack, sp);
    }
    if (takenSlot.isReturn) {
        if (enableDB) {
            SpecRASTrace rec(When::SPECULATIVE, RAS_OP::POP, pred.bbStart, takenSlot.pc,
                stack[sp].retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
            specRasTrace->write_record(rec);
        }
        // do pop
        auto retAddr = stack[sp].retAddr;
        DPRINTF(URAS, "spec stack pop at pc 0x%llx target %llx\n", pred.bbStart, retAddr);
        pop(stack, sp);
    }
    printStack("after specUpdateState", stack, sp);
}

void
BTBuRAS::recoverState(
    const FetchTarget &entry,
    const ResolvedBranch *actual_branch)
{
    auto &stack = specStack;
    auto &sp = specSp;
    printStack("before recoverState", stack, sp);
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<uRASMeta>(entry.predMetas[getComponentIdx()]);
    if (enableDB) {
        SpecRASTrace rec(When::REDIRECT, RAS_OP::RECOVER,
                         entry.startPC,
                         actual_branch ? actual_branch->pc : 0, 0,
                         sp, stack[sp].retAddr, stack[sp].ctr);
        specRasTrace->write_record(rec);
    }
    sp = meta_ptr->sp;
    stack[sp] = meta_ptr->tos;

    if (actual_branch && actual_branch->taken) {
        // do push & pops on control squash
        if (actual_branch->isReturn) {
            if (enableDB) {
                SpecRASTrace rec(When::REDIRECT, RAS_OP::POP,
                                 entry.startPC, actual_branch->pc,
                                 stack[sp].retAddr, sp,
                                 stack[sp].retAddr, stack[sp].ctr);
                specRasTrace->write_record(rec);
            }
            DPRINTF(URAS, "recover stack pop at pc 0x%llx target %llx\n", entry.startPC, stack[sp].retAddr);
            pop(stack, sp);
        }
        if (actual_branch->isCall) {
            Addr retAddr = actual_branch->pc + actual_branch->size;
            if (enableDB) {
                SpecRASTrace rec(When::REDIRECT, RAS_OP::PUSH,
                                 entry.startPC, actual_branch->pc, retAddr,
                                 sp, stack[sp].retAddr, stack[sp].ctr);
                specRasTrace->write_record(rec);
            }
            DPRINTF(URAS, "recover stack push addr 0x%llx\n", retAddr);
            push(retAddr, stack, sp);
        }
    }
    printStack("after recoverState", stack, sp);
}

void
BTBuRAS::updateWithBranchUpdateContext(
    const BranchUpdateContext &ctx,
    const std::vector<ResolvedBranch> &update_branches,
    const std::shared_ptr<void> &prediction_meta)
{
    auto &stack = nonSpecStack;
    auto &sp = nonSpecSp;
    printStack("before update", stack, sp);
    const auto *taken_summary_branch =
        findTakenActualUpdateSummaryBranch(update_branches);
    if (taken_summary_branch &&
        isReturnStackActionBranch(*taken_summary_branch)) {
        auto meta_ptr = std::static_pointer_cast<uRASMeta>(prediction_meta);
        auto pred_sp = meta_ptr->sp;
        auto pred_tos = meta_ptr->tos;
        auto miss = taken_summary_branch->mispred;
        if (taken_summary_branch->isCall) {
            Addr retAddr =
                taken_summary_branch->pc + taken_summary_branch->size;
            if (enableDB) {
                NonSpecRASTrace rec(RAS_OP::PUSH, ctx.startPC, taken_summary_branch->pc, retAddr,
                    pred_sp, pred_tos.retAddr, pred_tos.ctr, sp, stack[sp].retAddr, stack[sp].ctr, miss);
                nonSpecRasTrace->write_record(rec);
            }
            push(retAddr, stack, sp);
        }
        if (taken_summary_branch->isReturn) {
            if (enableDB) {
                NonSpecRASTrace rec(RAS_OP::POP, ctx.startPC, taken_summary_branch->pc, taken_summary_branch->target,
                    pred_sp, pred_tos.retAddr, pred_tos.ctr, sp, stack[sp].retAddr, stack[sp].ctr, miss);
                nonSpecRasTrace->write_record(rec);
            }
            pop(stack, sp);
        }
    }
    printStack("after update", stack, sp);
}

void
BTBuRAS::push(Addr retAddr, std::vector<uRASEntry> &stack, int &sp)
{
    auto &tos = stack[sp];
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
BTBuRAS::pop(std::vector<uRASEntry> &stack, int &sp)
{
    auto &tos = stack[sp];
    if (tos.ctr > 0) {
        tos.ctr--;
    } else {
        ptrDec(sp);
    }
}

void
BTBuRAS::ptrInc(int &ptr)
{
    ptr = (ptr + 1) % numEntries;
}

void
BTBuRAS::ptrDec(int &ptr)
{
    if (ptr > 0) {
        ptr--;
    } else {
        assert(ptr == 0);
        ptr = numEntries - 1;
    }
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5

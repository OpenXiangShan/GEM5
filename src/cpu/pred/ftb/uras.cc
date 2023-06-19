#include "cpu/pred/ftb/uras.hh"

namespace gem5 {

namespace branch_prediction {

namespace ftb_pred {

uRAS::uRAS(const Params &p)
    : TimedBaseFTBPredictor(p),
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
    hasDB = true;
    dbName = std::string("ras");
}

void
uRAS::setTrace()
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
uRAS::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullFTBPrediction> &stagePreds)
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
uRAS::getPredictionMeta()
{
    std::shared_ptr<void> meta_void_ptr = std::make_shared<uRASMeta>(meta);
    return meta_void_ptr;
}

void
uRAS::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred)
{
    auto &stack = specStack;
    auto &sp = specSp;
    // do push & pops on prediction
    pred.returnTarget = stack[sp].retAddr;
    auto takenSlot = pred.getTakenSlot();
    if (takenSlot.isCall) {
        Addr retAddr = takenSlot.pc + takenSlot.size;
        if (enableDB) {
            SpecRASTrace rec(When::SPECULATIVE, RAS_OP::PUSH, pred.bbStart, takenSlot.pc,
                retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
            specRasTrace->write_record(rec);
        }
        DPRINTF(FTBuRAS, "spec stack push addr 0x%llx\n", retAddr);
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
        DPRINTF(FTBuRAS, "spec stack pop at pc 0x%llx target %llx\n", pred.bbStart, retAddr);
        pop(stack, sp);
    }
    printStack("after specUpdateHist", stack, sp);
}

void
uRAS::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    auto &stack = specStack;
    auto &sp = specSp;
    printStack("before recoverHist", stack, sp);
    // recover sp and tos first
    auto meta_ptr = std::static_pointer_cast<uRASMeta>(entry.predMetas[getComponentIdx()]);
    auto takenSlot = entry.exeBranchInfo;
    if (enableDB) {
        SpecRASTrace rec(When::REDIRECT, RAS_OP::RECOVER, entry.startPC, takenSlot.pc, 0, sp, stack[sp].retAddr, stack[sp].ctr);
        specRasTrace->write_record(rec);
    }
    sp = meta_ptr->sp;
    stack[sp] = meta_ptr->tos;

    if (entry.exeTaken) {
        // do push & pops on control squash
        if (takenSlot.isReturn) {
            if (enableDB) {
                SpecRASTrace rec(When::REDIRECT, RAS_OP::POP, entry.startPC, takenSlot.pc, stack[sp].retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
                specRasTrace->write_record(rec);
            }
            DPRINTF(FTBuRAS, "recover stack pop at pc 0x%llx target %llx\n", entry.startPC, stack[sp].retAddr);
            pop(stack, sp);
        }
        if (takenSlot.isCall) {
            Addr retAddr = takenSlot.pc + takenSlot.size;
            if (enableDB) {
                SpecRASTrace rec(When::REDIRECT, RAS_OP::PUSH, entry.startPC, takenSlot.pc, retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
                specRasTrace->write_record(rec);
            }
            DPRINTF(FTBuRAS, "recover stack push addr 0x%llx\n", retAddr);
            push(retAddr, stack, sp);
        }
    }
    printStack("after recoverHist", stack, sp);
}

void
uRAS::update(const FetchStream &entry)
{
    auto &stack = nonSpecStack;
    auto &sp = nonSpecSp;
    printStack("before update", stack, sp);
    auto takenSlot = entry.exeBranchInfo;
    if (entry.exeTaken && (takenSlot.isReturn || takenSlot.isCall)) {
        auto meta_ptr = std::static_pointer_cast<uRASMeta>(entry.predMetas[getComponentIdx()]);
        auto pred_sp = meta_ptr->sp;
        auto pred_tos = meta_ptr->tos;
        auto miss = entry.squashType == SQUASH_CTRL && entry.squashPC == entry.exeBranchInfo.pc;
        if (takenSlot.isCall) {
            Addr retAddr = takenSlot.pc + takenSlot.size;
            if (enableDB) {
                NonSpecRASTrace rec(RAS_OP::PUSH, entry.startPC, takenSlot.pc, retAddr,
                    pred_sp, pred_tos.retAddr, pred_tos.ctr, sp, stack[sp].retAddr, stack[sp].ctr, miss);
                nonSpecRasTrace->write_record(rec);
            }
            push(retAddr, stack, sp);
        }
        if (takenSlot.isReturn) {
            if (enableDB) {
                NonSpecRASTrace rec(RAS_OP::POP, entry.startPC, takenSlot.pc, takenSlot.target,
                    pred_sp, pred_tos.retAddr, pred_tos.ctr, sp, stack[sp].retAddr, stack[sp].ctr, miss);
                nonSpecRasTrace->write_record(rec);
            }
            pop(stack, sp);
        }
    }
    printStack("after update", stack, sp);
}

void
uRAS::push(Addr retAddr, std::vector<uRASEntry> &stack, int &sp)
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
uRAS::pop(std::vector<uRASEntry> &stack, int &sp)
{
    auto &tos = stack[sp];
    if (tos.ctr > 0) {
        tos.ctr--;
    } else {
        ptrDec(sp);
    }
}

void
uRAS::ptrInc(int &ptr)
{
    ptr = (ptr + 1) % numEntries;
}

void
uRAS::ptrDec(int &ptr)
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

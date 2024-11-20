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
    for (int i = getDelay(); i < stagePreds.size(); i++) {  // uras 没用了吧
        // stagePreds[i].returnTarget = stack[sp].retAddr;     // 更新返回地址到预测结果中
    }
    meta.sp = sp;  // 更新栈指针，存储预测前信息，只有specUpdateHist会更新sp！
    meta.tos = stack[sp]; // 更新栈顶
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
    printStack("before specUpdateHist", stack, sp);
    // do push & pops on prediction
    pred.returnTarget = stack[sp].retAddr; // 更新返回地址到预测结果中？
    auto takenSlot = pred.getTakenSlot();   // 获取预测的taken slot， ftb 项
    if (takenSlot.isCall) { // 如果taken slot是call
        Addr retAddr = takenSlot.pc + takenSlot.size; // 计算返回地址=当前PC+指令长度
        if (enableDB) {
            SpecRASTrace rec(When::SPECULATIVE, RAS_OP::PUSH, pred.bbStart, takenSlot.pc,
                retAddr, sp, stack[sp].retAddr, stack[sp].ctr); // 记录推测栈的push操作
            specRasTrace->write_record(rec);
        }
        DPRINTF(FTBuRAS, "spec stack push addr 0x%llx\n", retAddr);
        push(retAddr, stack, sp); // 推测栈push
    }
    if (takenSlot.isReturn) { // 如果taken slot是return
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
    // recover sp and tos first 先恢复栈指针和栈顶
    auto meta_ptr = std::static_pointer_cast<uRASMeta>(entry.predMetas[getComponentIdx()]);
    auto takenSlot = entry.exeBranchInfo; // 实际跳转的分支信息
    if (enableDB) {
        SpecRASTrace rec(When::REDIRECT, RAS_OP::RECOVER, entry.startPC, takenSlot.pc, 0, sp, stack[sp].retAddr, stack[sp].ctr);
        specRasTrace->write_record(rec);
    }
    sp = meta_ptr->sp; // 恢复栈指针
    stack[sp] = meta_ptr->tos; // 恢复栈顶

    if (entry.exeTaken) { // 如果实际跳转
        // do push & pops on control squash
        if (takenSlot.isReturn) { // 如果taken slot是return
            if (enableDB) {
                SpecRASTrace rec(When::REDIRECT, RAS_OP::POP, entry.startPC, takenSlot.pc, stack[sp].retAddr, sp, stack[sp].retAddr, stack[sp].ctr);
                specRasTrace->write_record(rec);
            }
            DPRINTF(FTBuRAS, "recover stack pop at pc 0x%llx target %llx\n", entry.startPC, stack[sp].retAddr);
            pop(stack, sp); // 推测栈pop
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
    auto &stack = nonSpecStack; // 非推测栈， 其实没有用了，应该要去更新非推测栈的！
    auto &sp = nonSpecSp; // 非推测栈指针
    printStack("nonspec before update", stack, sp);
    auto takenSlot = entry.exeBranchInfo; // 实际跳转的分支信息
    if (entry.exeTaken && (takenSlot.isReturn || takenSlot.isCall)) { // 如果实际跳转，并且是return或call
        auto meta_ptr = std::static_pointer_cast<uRASMeta>(entry.predMetas[getComponentIdx()]);
        auto pred_sp = meta_ptr->sp; // 预测的栈指针
        auto pred_tos = meta_ptr->tos; // 预测的栈顶
        auto miss = entry.squashType == SQUASH_CTRL && entry.squashPC == entry.exeBranchInfo.pc; // 是否是squash的控制流，并且squash的PC和实际跳转的PC相同，说明预测错误
        if (takenSlot.isCall) {
            Addr retAddr = takenSlot.pc + takenSlot.size;
            if (enableDB) {
                NonSpecRASTrace rec(RAS_OP::PUSH, entry.startPC, takenSlot.pc, retAddr,
                    pred_sp, pred_tos.retAddr, pred_tos.ctr, sp, stack[sp].retAddr, stack[sp].ctr, miss);
                nonSpecRasTrace->write_record(rec);
            }
            push(retAddr, stack, sp); // 非推测栈push
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
    printStack("nonspec after update", stack, sp);
}

void
uRAS::push(Addr retAddr, std::vector<uRASEntry> &stack, int &sp)
{
    auto &tos = stack[sp];
    if (tos.retAddr == retAddr && tos.ctr < maxCtr) { // 如果栈顶的返回地址和当前返回地址相同，并且计数器小于最大计数器
        tos.ctr++; // 计数器+1
    } else { // 否则， 更新栈顶
        // push new entry
        ptrInc(sp); // 栈指针+1
        stack[sp].retAddr = retAddr; // 更新栈顶的返回地址
        stack[sp].ctr = 0; // 更新栈顶的计数器
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
    ptr = (ptr + 1) % numEntries; // 栈指针+1
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

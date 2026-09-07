// src/cpu/pred/btb/test/uras_test.cc
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <boost/dynamic_bitset.hpp>
#include <vector>
#include "base/types.hh"
#include "cpu/pred/btb/common.hh"

namespace gem5 {
namespace branch_prediction {
namespace btb_pred {

// Mock class for uRAS entry
struct uRASEntry {
    Addr retAddr;   // return address
    unsigned ctr;   // counter
    uRASEntry(Addr retAddr = 0, unsigned ctr = 0) : retAddr(retAddr), ctr(ctr) {}
};

// Mock class for uRAS meta
struct uRASMeta {
    int sp;
    uRASEntry tos;
    uRASMeta(int sp = 0, uRASEntry tos = uRASEntry()) : sp(sp), tos(tos) {}
};

// Mock class for uRAS
class MockuRAS {
public:
    MockuRAS(unsigned numEntries = 16, unsigned ctrWidth = 2) 
        : numEntries(numEntries), maxCtr((1 << ctrWidth) - 1) {
        // Initialize stack
        specSp = 0;
        specStack.resize(numEntries);
        for (auto &entry : specStack) {
            entry.retAddr = 0x80000000L;
            entry.ctr = 0;
        }
    }

    void push(Addr retAddr, std::vector<uRASEntry> &stack, int &sp) {
        auto &tos = stack[sp]; // top of stack
        if (tos.retAddr == retAddr && tos.ctr < maxCtr) {
            tos.ctr++;
        } else {
            // push new entry
            ptrInc(sp);
            stack[sp].retAddr = retAddr;
            stack[sp].ctr = 0;
        }
    }

    void pop(std::vector<uRASEntry> &stack, int &sp) {
        auto &tos = stack[sp];
        if (tos.ctr > 0) {
            tos.ctr--;
        } else {
            ptrDec(sp);
        }
    }

    int getDelay() {
        return 0;
    }

    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                     std::vector<FullBTBPrediction> &stagePreds) {
        auto &stack = specStack;
        auto &sp = specSp;
        assert(getDelay() < stagePreds.size());
        for (int i = getDelay(); i < stagePreds.size(); i++) {
            stagePreds[i].returnTarget = stack[sp].retAddr;
        }
        meta.sp = sp;
        meta.tos = stack[sp];
    }

    void specUpdateState(FullBTBPrediction &pred)
    {
        auto &stack = specStack;
        auto &sp = specSp;
        // do push & pops on prediction
        pred.returnTarget = stack[sp].retAddr;
        auto takenSlot = pred.getTakenEntry();
        if (takenSlot.isReturn) { // return inst, pop retAddr from spec stack
            // do pop
            auto retAddr = stack[sp].retAddr;
            pop(stack, sp);
        }
        if (takenSlot.isCall) { // call inst, push retAddr to spec stack
            Addr retAddr = takenSlot.pc + takenSlot.size;
            push(retAddr, stack, sp);
        }
    }

    // Recover state from prediction metadata, then apply explicit actual
    // branch facts for a control squash.
    // used when branch prediction error
    // two steps:
    // 1. recover sp and tos from context.predMetas[0]
    // 2. do push & pops on control squash based on the actual branch type (call/return)
    void recoverState(const HistoryRecoveryContext &context,
                      const BranchInfo &actualBranch,
                      bool actuallyTaken)
    {
        auto &stack = specStack;
        auto &sp = specSp;
        // recover sp and tos first
        auto meta_ptr = std::static_pointer_cast<uRASMeta>(
            context.predMetas[0]);
        sp = meta_ptr->sp;
        stack[sp] = meta_ptr->tos;

        if (actuallyTaken) {
            // do push & pops on control squash
            if (actualBranch.isReturn) {
                pop(stack, sp);
            }
            if (actualBranch.isCall) {
                Addr retAddr = actualBranch.pc + actualBranch.size;
                push(retAddr, stack, sp);
            }
        }
    }

    std::shared_ptr<void> getPredictionMeta() {
        return std::make_shared<uRASMeta>(meta);
    }

    // Helper methods for testing
    std::vector<uRASEntry>& getSpecStack() { return specStack; }
    int& getSpecSp() { return specSp; }

private:
    void ptrInc(int &ptr) {
        ptr = (ptr + 1) % numEntries;
    }

    void ptrDec(int &ptr) {
        if (ptr > 0) {
            ptr--;
        } else {
            ptr = numEntries - 1;
        }
    }

    unsigned numEntries;   // number of entries in the stack
    unsigned maxCtr;      // maximum counter value
    int specSp;           // stack pointer
    std::vector<uRASEntry> specStack; // stack
    uRASMeta meta;
};

// Test fixture for MockuRAS
class URASTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create mock uRAS instance
        uras = new MockuRAS(16, 2);  // 16 entries, 2-bit counter
    }
    
    void TearDown() override {
        delete uras;
    }

    BranchInfo createBranch(Addr pc, bool isCall, bool isReturn,
                            unsigned size = 4) {
        BranchInfo branch;
        branch.pc = pc;
        branch.isIndirect = isReturn;
        branch.isDirect = !branch.isIndirect;
        branch.isCall = isCall;
        branch.isReturn = isReturn;
        branch.size = size;
        return branch;
    }
    
    MockuRAS* uras;
};

// Test basic push operation
TEST_F(URASTest, BasicPush) {
    Addr retAddr = 0x1000;
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // Check initial state
    EXPECT_EQ(sp, 0);
    EXPECT_EQ(stack[sp].retAddr, 0x80000000L);
    EXPECT_EQ(stack[sp].ctr, 0);
    
    // Perform push operation
    uras->push(retAddr, stack, sp); // push retAddr = 0x1000
    
    // Verify state after push
    EXPECT_EQ(sp, 1);
    EXPECT_EQ(stack[sp].retAddr, retAddr);
    EXPECT_EQ(stack[sp].ctr, 0);
}

// Test basic pop operation
TEST_F(URASTest, BasicPop) {
    Addr retAddr = 0x1000;
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // First push an address
    uras->push(retAddr, stack, sp); // push retAddr = 0x1000
    EXPECT_EQ(sp, 1);
    
    // Perform pop operation
    uras->pop(stack, sp); // pop retAddr = 0x1000
    
    // Verify state after pop
    EXPECT_EQ(sp, 0);
    EXPECT_EQ(stack[sp].retAddr, 0x80000000L);
    EXPECT_EQ(stack[sp].ctr, 0);
}

// Test counter behavior
TEST_F(URASTest, CounterBehavior) {
    Addr retAddr = 0x1000;
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // Push same address twice
    uras->push(retAddr, stack, sp); // ctr: 0 -> 0 (new entry)
    uras->push(retAddr, stack, sp); // ctr: 0 -> 1 (same entry)
    uras->push(retAddr, stack, sp); // ctr: 1 -> 2 (same entry)
    
    // Verify counter incremented
    EXPECT_EQ(stack[sp].ctr, 2);
    EXPECT_EQ(stack[sp].retAddr, retAddr);
    
    // Pop twice
    uras->pop(stack, sp); // ctr: 2 -> 1 (same entry)
    uras->pop(stack, sp); // ctr: 1 -> 0 (same entry)
    
    // Verify counter decremented
    EXPECT_EQ(stack[sp].ctr, 0);
    EXPECT_EQ(stack[sp].retAddr, retAddr);

    // pop once more
    uras->pop(stack, sp); // ctr: 0 -> 0 (same entry)
    EXPECT_EQ(stack[sp].ctr, 0);
    EXPECT_EQ(stack[sp].retAddr, 0x80000000L);
}

// Test stack full behavior
TEST_F(URASTest, StackFull) {
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // Fill up stack
    for (int i = 0; i < 16; i++) {
        uras->push(0x1000 + i, stack, sp); // push retAddr = 0x1000 + i
    }
    
    // Verify stack full behavior
    EXPECT_EQ(sp, 0);  // Stack size is 16, 16 % 16 = 0
    uras->push(0x2000, stack, sp); // push retAddr = 0x2000
    EXPECT_EQ(sp, 1);  // Should wrap around to start
}

// basic putPCHistory test
TEST_F(URASTest, PutPCHistoryBasic) {
    Addr startAddr = 0x1000;
    boost::dynamic_bitset<> history(8, 0);  // 8-bit history, all 0s
    std::vector<FullBTBPrediction> stagePreds(4);  // 4 stages
    
    // set initial state
    Addr retAddr = 0x2000;
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // push an address to the stack
    uras->push(retAddr, stack, sp);
    
    // call putPCHistory
    uras->putPCHistory(startAddr, history, stagePreds);

    // test return target for all stages
    for (int i = 0; i < stagePreds.size(); i++) {
        EXPECT_EQ(stagePreds[i].returnTarget, retAddr);
    }
    
    // verify meta is correctly saved
    auto meta = std::static_pointer_cast<uRASMeta>(uras->getPredictionMeta());
    EXPECT_EQ(meta->sp, sp);
    EXPECT_EQ(meta->tos.retAddr, retAddr);
    EXPECT_EQ(meta->tos.ctr, 0);
}

// test specUpdateState for call
TEST_F(URASTest, SpecUpdateStateCall) {
    boost::dynamic_bitset<> history(8, 0);
    FullBTBPrediction pred;
    pred.bbStart = 0x1000;
    
    // Setup a call instruction in BTBEntry
    BTBEntry callEntry;
    callEntry.valid = true;
    callEntry.pc = 0x1000;
    callEntry.isCall = true;
    callEntry.size = 4;
    callEntry.target = 0x2000;  // 目标地址
    pred.btbEntries.push_back(callEntry);
    
    // 初始状态检查
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    EXPECT_EQ(sp, 0);
    EXPECT_EQ(stack[sp].retAddr, 0x80000000L);
    
    // 执行 specUpdateState
    uras->specUpdateState(pred);

    // 验证：
    // 1. 返回地址应该是call指令的下一条指令地址
    EXPECT_EQ(stack[sp].retAddr, 0x1004);  // pc + size
    // 2. sp应该增加
    EXPECT_EQ(sp, 1);
}

TEST_F(URASTest, SpecUpdateStateReturn) {
    boost::dynamic_bitset<> history(8, 0);
    FullBTBPrediction pred;
    pred.bbStart = 0x1000;
    
    // 先push一个返回地址
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    uras->push(0x2000, stack, sp);
    
    // Setup a return instruction in BTBEntry
    BTBEntry retEntry;
    retEntry.valid = true;
    retEntry.pc = 0x1000;
    retEntry.isReturn = true;
    retEntry.target = 0x2000;
    pred.btbEntries.push_back(retEntry);
    
    // 执行 specUpdateState
    uras->specUpdateState(pred);

    // 验证：
    // 1. returnTarget应该是栈顶的地址
    EXPECT_EQ(pred.returnTarget, 0x2000);
    // 2. sp应该减少
    EXPECT_EQ(sp, 0);
}

TEST_F(URASTest, SpecUpdateStateCallReturn) {
    boost::dynamic_bitset<> history(8, 0);
    
    // First prediction with call
    FullBTBPrediction pred1;
    pred1.bbStart = 0x1000;
    
    BTBEntry callEntry;
    callEntry.valid = true;
    callEntry.pc = 0x1000;
    callEntry.isCall = true;
    callEntry.size = 4;
    callEntry.target = 0x2000;
    pred1.btbEntries.push_back(callEntry);
    
    // 执行 call 的 specUpdateState
    uras->specUpdateState(pred1);

    // Second prediction with return
    FullBTBPrediction pred2;
    pred2.bbStart = 0x2000;
    
    BTBEntry retEntry;
    retEntry.valid = true;
    retEntry.pc = 0x2000;
    retEntry.isReturn = true;
    retEntry.target = 0x1004;  // 返回到call的下一条指令
    pred2.btbEntries.push_back(retEntry);
    
    // 执行 return 的 specUpdateState
    uras->specUpdateState(pred2);

    // 验证：
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    // 1. sp应该回到初始位置
    EXPECT_EQ(sp, 0);
    // 2. returnTarget应该匹配call的下一条指令
    EXPECT_EQ(pred2.returnTarget, 0x1004);
}

TEST_F(URASTest, SpecUpdateStatePopThenPush) {
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    uras->push(0x1004, stack, sp);
    uras->push(0x2004, stack, sp);

    FullBTBPrediction pred;
    pred.bbStart = 0x3000;
    BTBEntry entry;
    entry.valid = true;
    entry.pc = 0x3000;
    entry.size = 4;
    entry.isCall = true;
    entry.isReturn = true;
    pred.btbEntries.push_back(entry);

    uras->specUpdateState(pred);

    EXPECT_EQ(pred.returnTarget, 0x2004);
    EXPECT_EQ(sp, 2);
    EXPECT_EQ(stack[sp].retAddr, 0x3004);
}

// Test basic recovery functionality
TEST_F(URASTest, RecoverStateBasic) {
    boost::dynamic_bitset<> history(8, 0);
    FetchTarget entry;

    // initial state
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    uras->push(0x1000, stack, sp);  // initial state, push an address
    
    // create meta data
    uRASMeta meta;
    meta.sp = 0;  // restore to initial sp
    meta.tos = uRASEntry(0x2000);  // set different tos
    entry.predMetas[0] = std::make_shared<uRASMeta>(meta);
    
    // recover
    BranchInfo actualBranch;
    uras->recoverState(
        HistoryRecoveryContext(entry), actualBranch, false);

    // verify recovery result
    EXPECT_EQ(sp, 0);  // sp should be restored
    EXPECT_EQ(stack[sp].retAddr, 0x2000);  // tos should be restored
}

// Test recovery with return instruction
TEST_F(URASTest, RecoverStateReturn) {
    boost::dynamic_bitset<> history(8, 0);
    FetchTarget entry;

    // 设置初始状态
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    uras->push(0x1000, stack, sp);
    
    // 设置meta数据
    uRASMeta meta;
    meta.sp = 1;
    meta.tos = uRASEntry(0x1000);
    entry.predMetas[0] = std::make_shared<uRASMeta>(meta);
    
    // 设置return指令信息
    auto actualBranch = createBranch(0x2000, false, true);

    // 执行恢复
    uras->recoverState(
        HistoryRecoveryContext(entry), actualBranch, true);

    // 验证：应该先恢复sp和tos，然后执行pop
    EXPECT_EQ(sp, 0);  // 1(恢复) -> 0(pop)
    EXPECT_EQ(stack[sp].retAddr, 0x80000000L);  // 初始值
}

// Test recovery with call instruction
TEST_F(URASTest, RecoverStateCall) {
    boost::dynamic_bitset<> history(8, 0);
    FetchTarget entry;

    // 设置初始状态
    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // 设置meta数据
    uRASMeta meta;
    meta.sp = 0;
    meta.tos = uRASEntry(0x80000000L);
    entry.predMetas[0] = std::make_shared<uRASMeta>(meta);
    
    // 设置call指令信息
    auto actualBranch = createBranch(0x1000, true, false);

    // 执行恢复
    uras->recoverState(
        HistoryRecoveryContext(entry), actualBranch, true);

    // 验证：应该先恢复sp和tos，然后执行push
    EXPECT_EQ(sp, 1);  // 0(恢复) -> 1(push)
    EXPECT_EQ(stack[sp].retAddr, 0x1004);  // pc + size
}

// Test recovery with call-return sequence
TEST_F(URASTest, RecoverStateCallReturn) {
    boost::dynamic_bitset<> history(8, 0);
    FetchTarget entry1, entry2;

    auto& stack = uras->getSpecStack();
    auto& sp = uras->getSpecSp();
    
    // 第一步：call指令恢复
    uRASMeta meta1;
    meta1.sp = 0;
    meta1.tos = uRASEntry(0x80000000L);
    entry1.predMetas[0] = std::make_shared<uRASMeta>(meta1);
    
    auto actualCall = createBranch(0x1000, true, false);

    uras->recoverState(
        HistoryRecoveryContext(entry1), actualCall, true);

    // 验证call的结果
    EXPECT_EQ(sp, 1);
    EXPECT_EQ(stack[sp].retAddr, 0x1004);
    
    // 第二步：return指令恢复
    uRASMeta meta2;
    meta2.sp = 1;
    meta2.tos = uRASEntry(0x1004);
    entry2.predMetas[0] = std::make_shared<uRASMeta>(meta2);
    
    auto actualReturn = createBranch(0x2000, false, true);

    uras->recoverState(
        HistoryRecoveryContext(entry2), actualReturn, true);

    // 验证return的结果
    EXPECT_EQ(sp, 0);
    EXPECT_EQ(stack[sp].retAddr, 0x80000000L);
}

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

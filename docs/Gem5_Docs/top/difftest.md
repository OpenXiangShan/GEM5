# GEM5 Difftest 流程分析

## 概述

GEM5的difftest（差分测试）是一个验证CPU正确性的机制，通过与参考模型（如NEMU或Spike）逐周期比较所有架构状态来检测实现错误。本文档详细分析difftest的工作流程，特别关注异常处理和RISC-V CSR不一致的处理机制。

## 架构组件

### 1. 代理模型（Proxy）
```cpp
// src/cpu/difftest.hh
class RefProxy {
    void (*memcpy)(paddr_t nemu_addr, void *dut_buf, size_t n, bool direction);
    void (*regcpy)(void *dut, bool direction);
    void (*csrcpy)(void *dut, bool direction);
    void (*exec)(uint64_t n);
    vaddr_t (*guided_exec)(void *disambiguate_para);
    void (*raise_intr)(uint64_t no);
    void (*isa_reg_display)();
};
```

支持两种参考模型：
- **NemuProxy**: 使用NEMU作为参考模型，支持多核
- **SpikeProxy**: 使用Spike作为参考模型，仅支持单核

### 2. 寄存器文件结构
```cpp
struct riscv64_CPU_regfile {
    union { uint64_t _64; } gpr[32];      // 通用寄存器
    union { uint64_t _64; } fpr[32];      // 浮点寄存器
    
    // CSR寄存器
    uint64_t mode;                        // 特权模式
    uint64_t mstatus, sstatus;           // 状态寄存器
    uint64_t mepc, sepc;                 // 异常PC
    uint64_t mtval, stval;               // 异常值
    uint64_t mcause, scause;             // 异常原因
    uint64_t satp, mip, mie;             // 地址转换和中断
    
    // 虚拟化扩展
    uint64_t mtval2, mtinst, hstatus;
    uint64_t hideleg, hedeleg;
    
    // 向量扩展
    union { uint64_t _64[VENUM64]; } vr[32];
    uint64_t vtype, vl, vstart;
};
```

## 主要工作流程

### 1. 初始化阶段
```cpp
// src/cpu/base.cc:209
if (enableDifftest) {
    // 选择参考模型
    if (params().difftest_ref_so.find("spike") != std::string::npos) {
        diffAllStates->proxy = new SpikeProxy(...);
    } else {
        diffAllStates->proxy = new NemuProxy(...);
    }
    
    // 初始化参考模型状态
    diffAllStates->proxy->regcpy(&(diffAllStates->gem5RegFile), REF_TO_DUT);
}
```

### 2. 指令执行与状态收集
在O3 CPU的commit阶段，每条指令都会调用`diffInst`收集执行信息：

```cpp
// src/cpu/o3/commit.cc:1429
void Commit::diffInst(ThreadID tid, const DynInstPtr &inst) {
    // 收集指令基本信息
    cpu->diffInfo.inst = inst->staticInst;
    cpu->diffInfo.pc = &inst->pcState();
    
    // 收集目标寄存器值
    for (int i = 0; i < inst->numDestRegs(); i++) {
        const auto &dest = inst->destRegIdx(i);
        if ((dest.isFloatReg() || dest.isIntReg()) && !dest.isZeroReg()) {
            cpu->diffInfo.scalarResults.at(i) = cpu->getArchReg(dest, tid);
        } else if (dest.isVecReg()) {
            cpu->getArchReg(dest, &(cpu->diffInfo.vecResult), tid);
        }
    }
    
    // 收集内存访问信息
    cpu->diffInfo.physEffAddr = inst->physEffAddr;
    cpu->diffInfo.effSize = inst->effSize;
    
    // 执行difftest步骤
    cpu->difftestStep(tid, inst->seqNum);
}
```

### 3. Difftest执行逻辑
```cpp
// src/cpu/base.cc:1431
void BaseCPU::difftestStep(ThreadID tid, InstSeqNum seq) {
    // 判断哪些指令需要进行difftest
    bool fence_should_diff = is_fence && !diffInfo.inst->isMicroop();
    bool lr_should_diff = diffInfo.inst->isLoadReserved();
    bool amo_should_diff = diffInfo.inst->isAtomic() && diffInfo.inst->numDestRegs() > 0;
    bool is_sc = diffInfo.inst->isStoreConditional() && diffInfo.inst->isDelayedCommit();
    bool other_should_diff = !diffInfo.inst->isAtomic() && !is_fence && !is_sc &&
                             (!diffInfo.inst->isMicroop() || diffInfo.inst->isLastMicroop());

    if (should_diff) {
        // 首次执行时初始化内存和寄存器状态
        if (!diffAllStates->hasCommit && diffInfo.pc->instAddr() == 0x80000000u) {
            initializeDifftest();
        }
        
        // 执行difftest比较
        auto [diff_at, npc_match] = diffWithNEMU(tid, seq);
        handleDiffResult(diff_at, npc_match, tid, seq);
    }
}
```

## 异常处理机制

### 1. 异常引导执行
当GEM5遇到异常时，需要通过引导执行让参考模型也产生相同的异常：

```cpp
// src/cpu/base.cc:1639
void BaseCPU::setExceptionGuideExecInfo(uint64_t exception_num, uint64_t mtval, 
                                       uint64_t stval, bool force_set_jump_target,
                                       uint64_t jump_target, ThreadID tid) {
    auto &gd = diffAllStates->diff.guide;
    
    // 设置异常信息
    gd.force_raise_exception = true;
    gd.exception_num = exception_num;
    gd.mtval = mtval;
    gd.stval = stval;
    
    // 虚拟化扩展相关异常值
    gd.mtval2 = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL2, tid);
    gd.htval = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTVAL, tid);
    gd.vstval = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVAL, tid);
    
    // 执行引导执行，让参考模型产生相同异常
    diffAllStates->proxy->guided_exec(&(diffAllStates->diff.guide));
    
    // 同步参考模型状态
    diffAllStates->proxy->regcpy(diffAllStates->diff.nemu_reg, REF_TO_DIFFTEST);
    diffAllStates->diff.nemu_this_pc = diffAllStates->diff.nemu_reg->pc;
}
```

### 2. 异常提交处理
在commit阶段发现异常时的处理：

```cpp
// src/cpu/o3/commit.cc:1571
if (cpu->difftestEnabled() && inst_fault->isFromISA()) {
    auto priv = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_PRV, tid);
    RegVal cause = 0;
    
    // 根据特权级别读取异常原因
    if (priv == RiscvISA::PRV_M) {
        cause = cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MCAUSE, tid);
    } else if (priv == RiscvISA::PRV_S) {
        cause = cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_SCAUSE, tid);
    } else {
        cause = cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_UCAUSE, tid);
    }
    
    // 设置异常引导信息
    cpu->setExceptionGuideExecInfo(
        exception_no, 
        cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL, tid),
        cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_STVAL, tid), 
        false, 0, tid);
}
```

## CSR状态比较与处理

### 1. 跳过性能计数器CSR
某些CSR在不同实现中表现不一致，需要跳过比较：

```cpp
// src/cpu/difftest.cc:47
void skipPerfCntCsr() {
    // 跳过周期和指令计数器
    skipCSRs.push_back(GetCSROPInstCode(gem5::RiscvISA::CSR_MCYCLE));
    skipCSRs.push_back(GetCSROPInstCode(gem5::RiscvISA::CSR_MINSTRET));
    
    // 跳过性能监控计数器
    for (uint64_t counter = gem5::RiscvISA::CSR_MMHPMCOUNTER3;
                  counter <= gem5::RiscvISA::CSR_MMHPMCOUNTER31; counter++) {
        skipCSRs.push_back(GetCSROPInstCode(counter));
    }
}
```

### 2. CSR值比较
在`diffWithNEMU`中逐一比较所有重要的CSR：

```cpp
// src/cpu/base.cc:1041
std::pair<int, bool> BaseCPU::diffWithNEMU(ThreadID tid, InstSeqNum seq) {
    // 比较基本CSR
    
    // 1. mstatus比较
    auto gem5_val = readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, tid);
    auto ref_val = diffAllStates->referenceRegFile.mstatus;
    if (gem5_val != ref_val) {
        csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mstatus, 
                      diffAllStates->gem5RegFile.mstatus, seq, "mstatus", diff_at);
    }
    
    // 2. 异常相关CSR
    // mtval, stval, mcause, scause, mepc, sepc
    
    // 3. 地址转换CSR
    // satp
    
    // 4. 中断相关CSR  
    // mip, mie
    
    // 5. 虚拟化扩展CSR (如果启用)
    if (enableRVHDIFF) {
        // mtval2, mtinst, hstatus, hideleg, hedeleg等
    }
    
    // 6. 向量扩展CSR (如果启用)
    if (enableRVV) {
        // vtype, vl, vstart, vcsr等
    }
}
```

### 3. CSR差异报告
发现CSR不一致时的报告机制：

```cpp
// src/cpu/base.cc:859
void BaseCPU::csrDiffMessage(uint64_t gem5_val, uint64_t ref_val, int error_num, 
                            uint64_t &error_reg, InstSeqNum seq,
                            std::string error_csr_name, int &diff_at) {
    DPRINTF(DiffValue, "Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
    DPRINTF(DiffValue, "Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, "
                      "GEM5 value: \033[31m%#lx\033[0m\n",
            error_csr_name, ref_val, gem5_val);
    
    // 标记错误CSR
    diffInfo.errorCsrsValue[error_num] = 1;
    error_reg = gem5_val;
    if (!diff_at)
        diff_at = ValueDiff;
}
```

## 特殊情况处理

### 1. PC不匹配的恢复机制
```cpp
// src/cpu/base.cc:1472
if (diff_at != NoneDiff) {
    if (npc_match && diff_at == PCDiff) {
        // PC不匹配但下一PC匹配，让NEMU再执行一条指令
        std::tie(diff_at, npc_match) = diffWithNEMU(tid, 0);
        if (diff_at != NoneDiff) {
            reportDiffMismatch(tid, seq);
            panic("Difftest failed again!\n");
        } else {
            clearDiffMismatch(tid, seq);
            DPRINTF(Diff, "Difftest matched again, NEMU seems to commit the failed mem instruction\n");
        }
    } else {
        reportDiffMismatch(tid, seq);
        panic("Difftest failed!\n");
    }
}
```

### 2. MMIO指令处理
对于MMIO访问，跳过参考模型执行：

```cpp
// src/cpu/base.cc:883
if (is_mmio) {
    DPRINTF(Diff, "Skip step NEMU due to mmio access\n");
    // 直接更新参考模型的PC和寄存器，不执行指令
    diffAllStates->referenceRegFile.pc = diffInfo.pc->as<RiscvISA::PCState>().npc();
    if (diffInfo.inst->numDestRegs() > 0) {
        const auto &dest = diffInfo.inst->destRegIdx(0);
        unsigned index = dest.index() + (dest.isFloatReg() ? FPRegIndexBase : IntRegIndexBase);
        diffAllStates->referenceRegFile[index] = diffInfo.scalarResults[0];
    }
    diffAllStates->proxy->regcpy(&(diffAllStates->referenceRegFile), DUT_TO_REF);
    return std::make_pair(NoneDiff, true);
}
```

### 3. 性能计数器CSR跳过
在寄存器比较时检查是否为需要跳过的CSR指令：

```cpp
// src/cpu/base.cc:1360
bool skipCSR = false;
for (auto iter : skipCSRs) {
    if ((machInst & 0xfff00073) == iter) {
        skipCSR = true;
        DPRINTF(Diff, "This is an csr instruction, skip!\n");
        // 强制同步参考模型的值
        diffAllStates->referenceRegFile[dest_tag] = gem5_val;
        diffAllStates->proxy->regcpy(&(diffAllStates->referenceRegFile), DUT_TO_REF);
        break;
    }
}
```

## 错误报告与调试

### 1. 错误报告函数
```cpp
// src/cpu/base.cc:1405
void BaseCPU::reportDiffMismatch(ThreadID tid, InstSeqNum seq) {
    warn("%s", diffMsg.str());                    // 输出差异信息
    diffAllStates->proxy->isa_reg_display();      // 显示参考模型寄存器
    displayGem5Regs();                            // 显示GEM5寄存器
    
    // 输出最近提交的指令历史
    warn("start dump last %lu committed msg\n", diffInfo.lastCommittedMsg.size());
    while (diffInfo.lastCommittedMsg.size()) {
        auto &inst = diffInfo.lastCommittedMsg.front();
        warn("V %s\n", inst->genDisassembly());
        diffInfo.lastCommittedMsg.pop();
    }
}
```

### 2. 状态清理
```cpp
// src/cpu/base.cc:1399
void BaseCPU::clearDiffMismatch(ThreadID tid, InstSeqNum seq) {
    diffMsg.str(std::string());                                    // 清空消息缓冲
    memset(diffInfo.errorRegsValue, 0, sizeof(diffInfo.errorRegsValue));  // 清空寄存器错误标记
    memset(diffInfo.errorCsrsValue, 0, sizeof(diffInfo.errorCsrsValue));  // 清空CSR错误标记
    diffInfo.errorPcValue = 0;                                     // 清空PC错误标记
}
```

## 总结

GEM5的difftest机制通过以下关键特性确保CPU实现的正确性：

1. **逐指令比较**: 每条提交的指令都与参考模型进行状态比较
2. **异常同步**: 通过引导执行机制确保异常在两个模型中一致产生
3. **CSR精确比较**: 重点关注架构状态CSR，跳过实现相关的性能计数器
4. **错误恢复**: 针对某些特殊情况（如PC不匹配）提供恢复机制
5. **详细调试**: 提供丰富的错误报告和状态显示功能

这套机制为RISC-V处理器的验证提供了强有力的保障，特别是在异常处理和特权级状态管理方面。

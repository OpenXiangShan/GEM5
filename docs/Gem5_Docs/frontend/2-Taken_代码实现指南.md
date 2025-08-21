# 2-Taken 分支预测器代码实现指南

## 目录
1. [核心数据结构](#核心数据结构)
2. [预测流程实现](#预测流程实现)
3. [训练逻辑实现](#训练逻辑实现)
4. [流水线集成](#流水线集成)
5. [高级特性](#高级特性)
6. [Bug修复](#bug修复)
7. [代码变更清单](#代码变更清单)

---

## 核心数据结构

### 扩展的uBTB表项结构

**文件**: `src/cpu/pred/btb/btb_ubtb.hh`

```cpp
typedef struct TickedUBTBEntry : public BTBEntry {
    unsigned uctr;           // 2位饱和计数器，用于替换策略
    uint64_t tick;           // MRU替换的时间戳
    int numNTConds;          // taken分支前的条件分支数量
    bool valid_2nd;          // 第二个取指块是否存在
    bool pt_2nd;             // 第二个FB是否预测taken（true=有分支，false=顺序执行）
    BranchInfo branch_info_2nd; // 第二个分支的属性信息（仅当pt_2nd=true时有效）

    TickedUBTBEntry() : BTBEntry(), uctr(0), tick(0), numNTConds(0), 
                        valid_2nd(false), pt_2nd(false), branch_info_2nd() {}
} TickedUBTBEntry;
```

**关键点**:
- `valid_2nd`: 控制是否有第二个预测
- `pt_2nd`: 区分第二个FB是否包含分支（true）或仅为顺序执行（false）
- `branch_info_2nd`: 仅在`pt_2nd=true`时使用

### DFF缓冲区用于跨周期训练

**文件**: `src/cpu/pred/btb/decoupled_bpred.hh`

```cpp
struct PredictionDFF {
    bool valid{false};
    FullBTBPrediction prevS3Pred;     // 前一周期的S3最终预测结果
    int prevUbtbHitIndex{-1};         // 前一周期的命中索引，用于训练

    void reset() {
        valid = false;
        prevUbtbHitIndex = -1;
    }

    void storePrediction(const FullBTBPrediction& s3_pred, int hit_index) {
        prevS3Pred = s3_pred;
        prevUbtbHitIndex = hit_index;
        valid = true;
    }
};
```

### BPU状态机

```cpp
enum class BpuState {
    IDLE,                   // 等待开始新预测
    PREDS_READY,            // 1-2个预测已完成，等待入队
    WAITING_FOR_SECOND_ENQ  // 第一个预测已入队，第二个等待FSQ空间
};
```

---

## 预测流程实现

### 核心预测函数：putPCHistory2Taken

**文件**: `src/cpu/pred/btb/btb_ubtb.cc`

```cpp
std::pair<int, bool> UBTB::putPCHistory2Taken(
    Addr startAddr, 
    const boost::dynamic_bitset<> &history,
    std::vector<FullBTBPrediction> &stagePreds,
    FullBTBPrediction &secondPrediction)
{
    // 清理之前的MBTB meta
    mbtbSecondPredMeta = nullptr;
    
    // 执行标准uBTB查找
    int hit_index = lookup(startAddr);
    bool hit_found = (hit_index != -1);
    
    if (hit_found) {
        auto& entry = entries[hit_index];
        // 更新时间戳和历史
        updateTimestampAndHistory(hit_index, history, stagePreds);
        
        // 检查是否有第二个预测
        if (entry.valid_2nd) {
            if (entry.pt_2nd) {
                // 情况1：第二个FB有taken分支
                fillSecondPrediction(secondPrediction, entry.branch_info_2nd);
                
                // 范围检查
                if (isSecondPredictionInRange(stagePreds[0], secondPrediction)) {
                    createSecondPredictionMetaForMBTB(entry.branch_info_2nd);
                    ubtbStats.twotaken_pt_true++;
                    return {hit_index, true};
                } else {
                    ubtbStats.twotaken_range_check_failed++;
                }
            } else {
                // 情况2：第二个FB无分支，顺序执行
                Addr secondFBStart = stagePreds[0].getTarget(predictWidth);
                fillSecondPredictionFallthrough(secondPrediction, secondFBStart);
                
                // 为MBTB创建空meta保持一致性
                mbtbSecondPredMeta = std::make_shared<DefaultBTB::BTBMeta>();
                ubtbStats.twotaken_pt_false++;
                return {hit_index, true};
            }
        }
    } else {
        // Miss处理：创建第一个预测但标记为miss
        createFirstPredictionOnMiss(startAddr, stagePreds);
    }
    
    return {hit_index, false};
}
```

### 第二个预测的构造

**情况1：pt_2nd=true（有分支）**
```cpp
void UBTB::fillSecondPrediction(FullBTBPrediction& secondPred, 
                                const BranchInfo& branch_info_2nd) {
    secondPred.bbStart = /* 第一个预测的目标 */;
    secondPred.predSource = 0;  // uBTB预测
    
    // 从BranchInfo构造BTBEntry
    BTBEntry btbEntry(branch_info_2nd);
    secondPred.btbEntries.push_back(btbEntry);
    
    DPRINTF(UBTB, "构造第二个预测（有分支）: PC=%#lx, target=%#lx\n", 
            btbEntry.pc, btbEntry.target);
}
```

**情况2：pt_2nd=false（顺序执行）**
```cpp
void UBTB::fillSecondPredictionFallthrough(FullBTBPrediction& secondPred, 
                                           Addr secondFBStart) {
    secondPred.bbStart = secondFBStart;
    secondPred.predSource = 0;
    secondPred.btbEntries.clear(); // 无分支
    
    DPRINTF(UBTB, "构造第二个预测（顺序）: bbStart=%#lx\n", secondFBStart);
}
```

### BPU中的预测请求

**文件**: `src/cpu/pred/btb/decoupled_bpred.cc`

```cpp
void DecoupledBPUWithBTB::requestNewPrediction() {
    // 初始化状态
    hasSecondPrediction = false;
    ubtbHitIndex = -1;
    
    // 对各个组件进行预测
    for (int i = 0; i < numComponents; i++) {
        if (components[i] == ubtb) {
            // uBTB使用2-taken接口
            auto [hit_index, has_second] = ubtb->putPCHistory2Taken(
                s0PC, s0History, predsOfEachStage, secondPrediction);
            
            ubtbHitIndex = hit_index;
            hasSecondPrediction = has_second;
            
            if (has_second) {
                DPRINTF(DecoupleBP, "获得第二个预测: target=%#lx\n", 
                        secondPrediction.bbStart);
            }
        } else {
            // 其他组件使用标准接口
            components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
        }
    }
    
    // ABTB兼容性：如果有第二个预测，需要预加载维护队列
    if (hasSecondPrediction && abtb && abtb->getAheadPipelinedStages() > 0) {
        abtb->preloadBlock(secondPrediction.bbStart);
        DPRINTF(DecoupleBP, "为ABTB预加载第二个块: %#lx\n", 
                secondPrediction.bbStart);
    }
}
```

---

## 训练逻辑实现

### 2-taken条件检查

**文件**: `src/cpu/pred/btb/btb_ubtb.cc`

```cpp
bool UBTB::check2TakenConditions(FullBTBPrediction& dff, 
                                 const FullBTBPrediction& s3Pred) {
    assert(dff.getTarget(predictWidth) == s3Pred.bbStart);
    ubtbStats.twoTakenConditionChecks++;

    // 1. 第一个预测必须至少有一个分支
    if (dff.btbEntries.empty()) {
        ubtbStats.twoTakenFailEmptyPreds++;
        return false;
    }

    auto firstBr = dff.getTakenEntry();
    
    // 2. 第一个分支必须taken才能形成2-taken序列
    if (!dff.isTaken()) {
        ubtbStats.twoTakenFailFirstNotTaken++;
        return false;
    }

    // 3. 第一个分支不能是多目标间接跳转
    if (firstBr.isIndirect) {
        ubtbStats.twoTakenFailFirstIndirect++;
        return false;
    }

    // 4. 处理pt_2nd=false情况：第二个FB无分支（顺序执行）
    if (s3Pred.btbEntries.empty()) {
        ubtbStats.twoTakenAcceptFallthrough++;
        return true;  // pt_2nd=false情况总是允许
    }

    // 5. pt_2nd=true情况：两个FB都有分支 - 应用兼容性规则
    auto& secondBr = s3Pred.btbEntries[0];

    // 第二个分支不能是多目标间接跳转
    if (secondBr.isIndirect) {
        ubtbStats.twoTakenFailSecondIndirect++;
        return false;
    }

    // 第二个分支不能是条件分支，除非是alwaysTaken
    if (secondBr.isCond && !secondBr.alwaysTaken) {
        ubtbStats.twoTakenFailSecondCond++;
        return false;
    }

    // 不允许ret->ret（避免多次RAS读取）
    if (firstBr.isReturn && secondBr.isReturn) {
        ubtbStats.twoTakenFailRetRet++;
        return false;
    }

    // 不允许call->call（避免多次RAS写入）
    if (firstBr.isCall && secondBr.isCall) {
        ubtbStats.twoTakenFailCallCall++;
        return false;
    }

    ubtbStats.twoTakenConditionPassed++;
    return true;
}
```

### 统一训练函数

```cpp
void UBTB::trainCommon(int entry_index, FullBTBPrediction& pred, 
                       FullBTBPrediction* secondPred) {
    if (entry_index == -1) {
        // Miss情况：查找替换受害者
        entry_index = findVictimEntry(pred.bbStart);
        DPRINTF(UBTB, "Miss训练，使用受害者索引: %d\n", entry_index);
        
        // 安装新表项
        replaceEntry(entry_index, pred);
        
        // 如果有第二个预测，添加到表项
        if (secondPred != nullptr) {
            addSecondPredictionToEntry(entry_index, secondPred);
        }
    } else {
        // Hit情况：更新现有表项
        auto& entry = entries[entry_index];
        
        if (entry.match(pred)) {
            // 命中且匹配：更新UCtr，可能添加第二个预测
            entry.uctr = std::min(3U, entry.uctr + 1);
            updateMRUPosition(entry_index);
            
            if (secondPred != nullptr && !entry.valid_2nd) {
                addSecondPredictionToEntry(entry_index, secondPred);
                DPRINTF(UBTB, "为现有表项添加第二个预测\n");
            }
        } else {
            // 命中但不匹配：替换表项
            if (entry.uctr > 0) {
                entry.uctr--;
                DPRINTF(UBTB, "UCtr递减到: %d\n", entry.uctr);
            } else {
                replaceEntry(entry_index, pred);
                if (secondPred != nullptr) {
                    addSecondPredictionToEntry(entry_index, secondPred);
                }
            }
        }
    }
}
```

### 2-taken训练主函数

```cpp
void UBTB::train2Taken(FullBTBPrediction &dff_pred, 
                       FullBTBPrediction &s3_pred, int hit_index) {
    // 验证连续FB条件
    if (dff_pred.getTarget(predictWidth) != s3_pred.bbStart) {
        // 回退到1-taken训练
        trainCommon(hit_index, dff_pred, nullptr);
        DPRINTF(UBTB, "FB不连续，回退到1-taken训练\n");
        return;
    }
    
    // 检查2-taken条件
    if (!check2TakenConditions(dff_pred, s3_pred)) {
        // 回退到1-taken训练
        trainCommon(hit_index, dff_pred, nullptr);
        DPRINTF(UBTB, "2-taken条件不满足，回退到1-taken训练\n");
        return;
    }
    
    // 作为2-taken训练：传递s3_pred作为第二个预测
    trainCommon(hit_index, dff_pred, &s3_pred);
    DPRINTF(UBTB, "2-taken训练成功\n");
}
```

### 添加第二个预测到表项

```cpp
void UBTB::addSecondPredictionToEntry(int entryIndex, FullBTBPrediction* secondPred) {
    assert(entryIndex >= 0 && entryIndex < numEntries);
    assert(secondPred != nullptr);
    
    auto& entry = entries[entryIndex];
    
    // 根据第二个FB是否有分支确定pt_2nd
    bool pt_2nd_value = shouldSetPtSecond(*secondPred);
    
    if (pt_2nd_value) {
        // 情况1：第二个FB有taken分支
        if (!secondPred->btbEntries.empty()) {
            auto& btbEntry = secondPred->btbEntries[0];
            entry.branch_info_2nd = BranchInfo(btbEntry);
            entry.valid_2nd = true;
            entry.pt_2nd = true;
            
            ubtbStats.twotaken_pt_true_trained++;
            DPRINTF(UBTB, "添加第二个预测（有分支）: PC=%#lx\n", btbEntry.pc);
        }
    } else {
        // 情况2：第二个FB无分支（仅顺序执行）
        entry.valid_2nd = true;
        entry.pt_2nd = false;
        // branch_info_2nd在此情况下无关
        
        ubtbStats.twotaken_pt_false_trained++;
        DPRINTF(UBTB, "添加第二个预测（顺序）: bbStart=%#lx\n", 
                secondPred->bbStart);
    }
}
```

---

## 流水线集成

### 增强的tick()函数

**文件**: `src/cpu/pred/btb/decoupled_bpred.cc`

```cpp
void DecoupledBPUWithBTB::tick() {
    DPRINTF(Override, "DecoupledBPUWithBTB::tick()\n");

    // 1. 请求预测，完成训练，准备入队
    if (bpuState == BpuState::IDLE && !streamQueueFull()) {
        requestNewPrediction();

        // 训练逻辑基于前一周期的DFF状态
        trainUbtbFor2Taken();
        numOverrideBubbles = generateFinalPredAndCreateBubbles();
        
        // 检查第二个预测在override后是否仍然有效
        validateSecondFBPrediction();

        // 为下一周期更新DFF
        predDFF.storePrediction(finalPred, ubtbHitIndex);

        bpuState = BpuState::PREDS_READY;
        
        // 清理预测器输出
        for (int i = 0; i < numStages; i++) {
            predsOfEachStage[i].btbEntries.clear();
        }
    }

    // 2. 入队预测（如果没有气泡）
    
    // 尝试入队第一个（或唯一的）预测
    if (bpuState == BpuState::PREDS_READY && validateFSQEnqueue()) {
        makeNewPrediction(true, false); // 第一个预测

        if (hasSecondPrediction) {
            // 有第二个预测需要处理
            finalPred = secondPrediction;
            hasSecondPrediction = false;
            bpuState = BpuState::WAITING_FOR_SECOND_ENQ;
        } else {
            // 只有一个预测，回到空闲状态
            bpuState = BpuState::IDLE;
        }
    }
    
    // 如果在等待第二个预测入队，尝试入队
    if (bpuState == BpuState::WAITING_FOR_SECOND_ENQ && validateFSQEnqueue()) {
        makeNewPrediction(true, true); // 第二个预测
        bpuState = BpuState::IDLE;
    }

    // 递减override气泡计数
    if (numOverrideBubbles > 0) {
        numOverrideBubbles--;
        dbpBtbStats.overrideBubbleNum++;
    }
}
```

### 训练协调

```cpp
void DecoupledBPUWithBTB::trainUbtbFor2Taken() {
    auto& s3_pred = predsOfEachStage[numStages-1];

    if (enable2Taken) {
        if (predDFF.valid) {
            // 2-taken训练：使用DFF中的前一周期预测
            ubtb->train2Taken(predDFF.prevS3Pred, s3_pred, predDFF.prevUbtbHitIndex);
            DPRINTF(DecoupleBP, "执行2-taken训练\n");
        } else {
            DPRINTF(DecoupleBP, "DFF无效，跳过2-taken训练\n");
        }
    } else {
        // 1-taken训练
        ubtb->train1Taken(s3_pred);
        DPRINTF(DecoupleBP, "执行1-taken训练\n");
    }
}
```

### 第二个预测验证

```cpp
void DecoupledBPUWithBTB::validateSecondFBPrediction() {
    if (!hasSecondPrediction) {
        return;
    }

    // 仅当第一个预测来自uBTB（阶段0）且未被覆盖时，第二个预测才有效
    if (finalPred.predSource != 0) {
        DPRINTF(UBTB, "uBTB1预测被覆盖（finalPred来源是阶段%d），" 
                      "使第二个FB预测无效\n", finalPred.predSource);
        hasSecondPrediction = false;
        secondPrediction.btbEntries.clear();
    }
}
```

---

## 高级特性

### AlwaysTaken条件分支支持

**问题**：第二个预测位置的alwaysTaken条件分支在变为双向时性能下降。

**解决方案**：为第二个预测选择性更新MBTB

**实现**：

1. **Meta存储**（在uBTB中）：
```cpp
// src/cpu/pred/btb/btb_ubtb.cc
void UBTB::createSecondPredictionMetaForMBTB(const BranchInfo& branch_info_2nd) {
    // 为MBTB创建标准BTBMeta
    mbtbSecondPredMeta = std::make_shared<DefaultBTB::BTBMeta>();
    
    // 将BranchInfo转换为BTBEntry
    BTBEntry btb_entry(branch_info_2nd);
    mbtbSecondPredMeta->hit_entries.push_back(btb_entry);
    
    DPRINTF(UBTB, "为第二个预测创建MBTB meta: PC=%#lx\n", btb_entry.pc);
}

// 公共检索函数
std::shared_ptr<void> UBTB::getMBTBSecondPredictionMeta() const {
    return mbtbSecondPredMeta;
}
```

2. **Meta集成**（在DecoupledBPU中）：
```cpp
// src/cpu/pred/btb/decoupled_bpred.cc
FetchStream DecoupledBPUWithBTB::createFetchStreamEntry(bool is_second_pred) {
    // ... 现有逻辑 ...
    
    // 保存预测器metadata
    for (int i = 0; i < numComponents; i++) {
        if (is_second_pred) {
            if (components[i] == btb) {
                // 对于MBTB，获取uBTB在getTwoTakenPrediction期间创建的meta
                entry.predMetas[i] = ubtb->getMBTBSecondPredictionMeta();
            } else {
                entry.predMetas[i] = components[i]->getSecondPredictionMeta();
            }
        } else {
            entry.predMetas[i] = components[i]->getPredictionMeta();
        }
    }
    
    return entry;
}
```

3. **选择性更新**：
```cpp
void DecoupledBPUWithBTB::updateSecondPredictionComponents(FetchStream &stream) {
    // RAS始终需要更新以保持正确的状态跟踪
    ras->update(stream);
    
    // MBTB需要更新以管理alwaysTaken标志
    stream.setUpdateInstEndPC(predictWidth);
    btb->update(stream);
    
    DPRINTF(DecoupleBP, "为第二个预测更新MBTB，PC=%#lx\n", stream.startPC);
}

// 在主更新函数中
void DecoupledBPUWithBTB::update(/* 参数 */) {
    // ...
    if (!stream.isSecondFBPred) {
        updatePredictorComponents(stream);
    } else {
        // 对第二个预测选择性更新特定组件
        updateSecondPredictionComponents(stream);
    }
    // ...
}
```

### pt_2nd支持（顺序执行增强）

**扩展2-taken从连续taken分支到包含顺序执行情况**

**关键实现**：

1. **条件简化**：
```cpp
bool UBTB::check2TakenConditions(FullBTBPrediction& dff, 
                                 const FullBTBPrediction& s3Pred) {
    // ... 现有检查 ...
    
    // 4. 处理pt_2nd=false情况：第二个FB无分支
    if (s3Pred.btbEntries.empty()) {
        ubtbStats.twoTakenAcceptFallthrough++;
        return true;  // pt_2nd=false情况总是允许
    }
    
    // ... pt_2nd=true的其他规则 ...
}
```

2. **动态pt_2nd设置**：
```cpp
bool UBTB::shouldSetPtSecond(const FullBTBPrediction& secondPred) {
    // pt_2nd=true如果第二个FB有任何分支
    // pt_2nd=false如果第二个FB无分支（纯顺序执行）
    return !secondPred.btbEntries.empty();
}
```

---

## Bug修复

### ABTB兼容性修复

**问题**：ABTB期望每个连续取指块调用一次`putPCHistory()`。2-taken返回块A和B时，ABTB看到A→C序列，破坏ahead-pipeline队列。

**解决方案**：队列填充策略

**实现**：

1. **新ABTB API**：
```cpp
// src/cpu/pred/btb/btb.cc
void DefaultBTB::preloadBlock(Addr pc) {
    // 仅执行数据数组读取+队列推送，无标签比较
    if (aheadPipelinedStages > 0) {
        // 克隆lookupSingleBlock()的前半部分到push操作
        auto entries = lookupDataArray(pc);
        aheadReadBtbEntries.push(entries);
        
        DPRINTF(BTB, "预加载块到ahead队列: PC=%#lx\n", pc);
        // 立即返回，不做标签比较
    }
}
```

2. **集成到预测流程**：
```cpp
// 在requestNewPrediction()中，在uBTB 2-taken逻辑之后
if (hasSecondPrediction && abtb && abtb->getAheadPipelinedStages() > 0) {
    abtb->preloadBlock(secondPrediction.bbStart); // 推送B，无比较
}
```

### 元数据检查点

我们的2nd FB在提交后不需要发到BPU进行训练，因为高级预测器没有与它对应的meta信息，然而，
我们的2nd FB在发生重定向后恢复时需要触发bpu内部状态的恢复，这里只要求meta里存恢复相关的信息，比如TAGE的折叠历史，换句话说，2nd FB的meta里不存训练相关的信息，但是存恢复相关的信息

**为所有需要历史状态的组件实现`getSecondPredictionMeta()`**：

**TAGE**：
```cpp
// src/cpu/pred/btb/btb_tage.cc
std::shared_ptr<void> BTBTAGE::getSecondPredictionMeta() {
    auto second_meta = std::make_shared<TageMeta>();
    second_meta->tagFoldedHist = tagFoldedHist;
    second_meta->altTagFoldedHist = altTagFoldedHist;
    second_meta->indexFoldedHist = indexFoldedHist;
    return second_meta;
}
```

**RAS**：
```cpp
// src/cpu/pred/btb/ras.cc
std::shared_ptr<void> BTBRAS::getSecondPredictionMeta() {
    auto second_meta = std::make_shared<RASMeta>();
    second_meta->ssp = ssp;
    second_meta->sctr = sctr;
    second_meta->TOSR = TOSR;
    second_meta->TOSW = TOSW;
    second_meta->target = getTop().retAddr;
    return second_meta;
}
```

---

## 代码变更清单

### 配置文件
- **src/cpu/pred/BranchPredictor.py**: 添加`enable2Taken`参数
- **configs/example/xiangshan.py**: 默认启用2-taken
- **util/xs_scripts/Options.py**: 添加`--disable-2taken`选项

### 核心BTB基础设施
- **src/cpu/pred/btb/btb.hh/.cc**: 添加`preloadBlock()`方法
- **src/cpu/pred/btb/timed_base_pred.hh**: 添加虚拟`getSecondPredictionMeta()`接口

### BTB组件更新
- **src/cpu/pred/btb/btb_tage.hh/.cc**: TAGE历史检查点实现
- **src/cpu/pred/btb/btb_mgsc.hh/.cc**: MGSC历史检查点实现
- **src/cpu/pred/btb/btb_ittage.hh/.cc**: ITTAGE历史检查点实现
- **src/cpu/pred/btb/ras.hh/.cc**: RAS状态检查点实现

### 核心uBTB实现
- **src/cpu/pred/btb/btb_ubtb.hh**: 2-taken数据结构和函数声明
- **src/cpu/pred/btb/btb_ubtb.cc**: 完整的2-taken预测和训练逻辑

### 主BPU逻辑
- **src/cpu/pred/btb/decoupled_bpred.hh**: 2-taken状态管理
- **src/cpu/pred/btb/decoupled_bpred.cc**: BPU流水线集成

### 流接口
- **src/cpu/pred/btb/stream_struct.hh**: 添加`isSecondFBPred`标志

### 测试脚本
- **util/xs_scripts/kmh_v3_btb.sh**: 更新测试选项
- **util/xs_scripts/xs-DecoupledBPU-ideal-kmhv3.sh**: 新的2-taken评估脚本

### 关键统计信息

**预测统计**：
```cpp
Stats::Scalar twotaken_pt_true;              // pt_2nd=true预测成功
Stats::Scalar twotaken_pt_false;             // pt_2nd=false预测
Stats::Scalar twotaken_range_check_failed;   // 范围检查失败
Stats::Scalar secondPredHit, secondPredMiss; // 第二个预测准确性
```

**训练统计**：
```cpp
Stats::Scalar twotaken_pt_true_trained;      // 创建pt_2nd=true表项
Stats::Scalar twotaken_pt_false_trained;     // 创建pt_2nd=false表项
Stats::Scalar twoTakenConditionPassed;       // 条件检查通过
Stats::Scalar twoTakenAcceptFallthrough;     // 接受pt_2nd=false情况
```

**性能比率**：
```cpp
// 公式统计用于分析
secondPredHitRatio = secondPredHit / (secondPredHit + secondPredMiss)
twoTakenUtilization = (twotaken_pt_true + twotaken_pt_false) / totalPredictions
```

---

## 总结

这个2-taken实现通过以下关键创新实现了性能提升：

1. **单uBTB架构**：相比双uBTB减少50%硬件复杂度
2. **pt_2nd支持**：扩展到顺序执行情况，大幅增加适用性  
3. **统一训练逻辑**：`trainCommon()`函数处理所有训练场景
4. **ABTB兼容**：`preloadBlock()`保持ahead-pipeline不变性
5. **选择性更新**：针对第二个预测的精确组件更新
6. **完整的元数据管理**：所有组件的正确squash恢复

**硬件开销**：每个uBTB表项增加约25%空间
**性能收益**：在适用场景下获得高达2倍的取指带宽

这个实现为未来的多预测研究奠定了坚实的基础，并提供了学术和工业环境中2-taken分支预测的参考实现。

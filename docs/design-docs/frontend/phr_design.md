# Kunminghu PHR 设计说明

本文档面向设计说明，但保留必要的关键实现，回答四个问题：

1. 为什么当前 BTB 顶层选择 PHR，而不是继续把块内所有方向位压进 GHR
2. PHR 的更新算法是什么，`branchPC` 和 `target` 如何参与
3. 哪些分支会更新 PHR
4. 为什么 folded history 可以等效表示完整 PHR 的历史信息，并且更适合时序

相关代码入口：

- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/folded_hist.hh`
- `src/cpu/pred/btb/folded_hist.cc`
- `src/cpu/pred/BranchPredictor.py`

## 1. 设计动机

传统 GHR 的语义是方向历史。假设一个 fetch block 内顺序看到 4 个条件分支，真实方向是 `NNNT`，那么 GHR 的自然更新就是把 `0001` 移入历史。

这套语义在单分支更新里很直接，但在当前 BTB block predictor 里，顶层每拍最多可能面对多条分支。如果继续沿用传统 GHR 形态，RTL 更新路径就要支持：

- 一次更新向历史移入 `0..8 bit`
- 把块内方向模式按顺序拼成诸如 `NNNT -> 0001` 这样的位串

这条路径功能上可以实现，但更新逻辑、可变移位网络、后续 predictor 的消费路径都会更重。

因此当前设计做了一个工程取舍：

- GHR 继续表达方向相关性
- PHR 不再记录块内逐位方向
- PHR 只记录“最终真正改变控制流的 taken path”

也就是说，PHR 关注的是：

- 哪个分支改变了控制流
- 这个分支的 `branchPC`
- 这个分支的 `target`

而不是块内每一条分支各自是 `T` 还是 `N`。

## 2. 顶层 PHR 是什么

线程级顶层状态在 `DecoupledBPUWithBTB` 中：

```cpp
boost::dynamic_bitset<> s0History;   // GHR
boost::dynamic_bitset<> s0PHistory;  // PHR
boost::dynamic_bitset<> s0BwHistory;
std::vector<boost::dynamic_bitset<>> s0LHistory;
```

初始化时：

```cpp
thread.s0History.resize(historyBits, 0);
thread.s0PHistory.resize(historyBits, 0);
thread.s0BwHistory.resize(historyBits, 0);
```

这里 `historyBits` 取自 `DecoupledBPUWithBTB.maxHistLen`，默认是 `970`（只是最大这么多，实际TAGE 只用了397，看后文）。所以顶层完整 PHR 是一个很长的 bit vector，但 predictor 真正在关键路径上消费的不是它本身，而是各自维护的 folded PHR。

## 3. 哪些分支会更新 PHR

顶层并不会对 block 内所有分支都更新 PHR。它只对“当前 block 最终选中的那次 taken 控制流”更新一次。

决定更新来源的逻辑在 `FullBTBPrediction::getPHistUpdate()`：

```cpp
PathHistoryUpdate getPHistUpdate()
{
    PathHistoryUpdate update;
    const auto &entry = getTakenEntry();
    if (entry.valid) {
        update.taken = true;
        update.pc = entry.pc;
        update.target = getEntryTarget(entry);
    }
    return update;
}
```

这段逻辑的设计语义很明确：

- 如果 block 内有条件分支被预测为 taken，选择按程序顺序出现的第一个 taken conditional branch
- 如果没有 taken conditional，但有有效的 uncond branch，则选择第一个 uncond branch
- 如果整个 block 最终没有 taken 控制流，则 `taken = false`，本次不更新 PHR

因此可以把当前 PHR 理解为：

- 它记录的是 fetch stream 真正走出去的那条 path
- 它不是 block 内所有方向事件的流水账

## 4. PHR 更新算法

### 4.1 `branchPC` 和 `target` 如何进入 PHR

首先，代码定义了一个 `pathHash()`：

```cpp
constexpr static uint64_t pathHashLength = 15;

inline uint64_t
pathHash(const Addr branchPC, const Addr targetPC)
{
    uint64_t hash =
        ((((branchPC >> 1) & ((1ULL << 9) - 1)) << 4) ^
         ((targetPC >> 2) & ((1ULL << 15) - 1)));
    hash &= ((1ULL << pathHashLength) - 1);
    return hash;
}
```

也就是说，PHR 不会直接把 PC 或 target 原样写进去，而是先把两者压成一个 15 bit 的路径指纹。

### 4.2 顶层如何推进 PHR

顶层更新函数是 `pHistShiftIn()`：

```cpp
void
DecoupledBPUWithBTB::pHistShiftIn(
    int shamt, bool taken, boost::dynamic_bitset<> &history, Addr pc, Addr target)
{
    if (shamt == 0) {
        return;
    }
    if (taken) {
        uint64_t hash = pathHash(pc, target);
        history <<= shamt;
        for (auto i = 0; i < pathHashLength && i < history.size(); i++) {
            history[i] = (hash & 1) ^ history[i];
            hash >>= 1;
        }
    }
}
```

当前实现里的关键事实：

- 只有 `taken == true` 才更新 PHR
- 顶层调用时固定使用 `shamt = 2`
- 更新过程是“先左移 2 bit，再把 15 bit path hash XOR 到低位”

因此当前 PHR 的设计不是“每次移入 2 bit 信息”，而是：

- 推进宽度固定是 2 bit
- 但每次 taken 事件都会把一个 15 bit 的 path 指纹混入低位

这正是当前设计的核心折中：

- 放弃“块内逐位方向历史”
- 保留“taken path 的强区分信息”
- 同时把顶层更新逻辑控制在一个比较轻的形态上

## 5. 顶层更新顺序为什么要先 folded、后完整 PHR

预测成功后，顶层不会先改 `s0PHistory`，而是先让各 predictor 基于“旧 PHR”更新 folded history，再推进顶层完整 PHR。

关键顺序在 `updateHistoryForPrediction()`：

```cpp
components[i]->specUpdateGHist(s0History, finalPred, ghist_update);
components[i]->specUpdatePHist(s0PHistory, finalPred, phist_update);
...
histShiftIn(shamt, taken, s0History);
pHistShiftIn(2, p_taken, s0PHistory, p_pc, p_target);
```

这不是实现偶然，而是 folded history 增量更新所必须的顺序。

原因是：

- folded history 需要知道“旧历史窗口最高位有哪些 bit 将被移出”
- 如果先改完整 PHR，再去更新 folded history，这部分信息就丢了

恢复路径也是同样的思想：

- 先恢复 `entry.phistory` 这个完整 PHR 快照
- 再恢复 predictor 内部 folded PHR 快照
- 最后用真实结果重放一次更新

这样才能保证 squash 后的状态等价于“当初就用真实结果更新”。

## 6. 为什么 folded history 能等效完整 PHR

### 6.1 folded history 的定义

`FoldedHistBase::check()` 里给出了它的数学语义：folded history 等价于把完整历史按 `foldedLen` 分块后逐块 XOR。

代码中的检查逻辑本质上就是：

```cpp
expected = history[0:foldedLen-1]
         ^ history[foldedLen:2*foldedLen-1]
         ^ history[2*foldedLen:3*foldedLen-1]
         ^ ...
```

然后比较：

- `expected`
- 当前 predictor 内维护的 `_folded`

如果两者不相等，就说明增量更新错了。

因此，“folded history 等效完整 PHR”的含义不是说它保留了完整历史的全部信息，而是：

- 对于当前采用的 fold 定义来说
- predictor 内维护的 `_folded`
- 必须始终等于“从完整 PHR 重新 fold 一遍”的结果

### 6.2 为什么可以不用每次从完整 PHR 重算

如果每次都从接近 1000 bit 的完整 PHR 重新做一次 fold，再去生成 index/tag，预测路径会很重。

所以当前实现用了增量更新：

- 完整 PHR 仍然作为 ground truth 保留
- 每次 taken 更新时，只把这次移出窗口的历史位和这次新进来的 path hash 增量地折进 `_folded`

这样就能做到：

- 功能上等价于“重新从完整 PHR fold 一遍”
- 时序上却只需要维护一个短小的 folded 值

### 6.3 `PathFoldedHist::update()` 做了什么

路径历史的增量更新在 `PathFoldedHist::update()` 里：

```cpp
for (int i = 0; i < shamt; i++) {
    temp ^= (ghr[posHighestBitsInGhr[i]] << posHighestBitsInOldFoldedHist[i]);
}

temp <<= shamt;

for (int i = 0; i < shamt; i++) {
    uint64_t highBit = (temp >> (foldedLen + i)) & 1;
    temp |= (highBit << i);
}

temp ^= foldHash(effectiveHash, foldedLen);
temp &= foldedMask;
```

这几步可以概括成：

1. 先处理“旧历史中即将被移出窗口的那些 bit”
2. 再做 folded 值本身的移位
3. 把移出 folded 窗口的高位折回低位
4. 再把本次新的 `pathHash(branchPC, target)` 折叠后 XOR 进去

因此，folded history 并不是另一个“拍脑袋压缩值”，而是被维护成：

- 始终等于完整 PHR 在当前 fold 规则下的精确压缩结果

代码里的 `check()` 正是在验证这一点。

## 7. 为什么 folded history 更适合时序

顶层完整 PHR 默认长度是 `970`，显然不适合直接放到每张表的 index/tag 关键路径上。

folded history 的作用非常直接：

- 把超长 PHR 压缩成每张表自己需要的短位宽
- 预测时直接消费这个短 folded 值
- 不必在每拍重新从完整历史做大规模 XOR 压缩

所以 folded history 既是一个功能上严格受约束的压缩表示，也是一个明确为了时序而存在的实现手段。

## 8. 哪些 predictor 在使用 PHR

当前 BTB 顶层里，真正消费 PHR 的主要组件是：

- `BTBTAGE`
- `BTBITTAGE`
- `MicroTAGE`
- `BTBMGSC`

其中：

- `BTBTAGE / BTBITTAGE / MicroTAGE` 使用 `PathFoldedHist` 参与 index/tag 生成
- `BTBMGSC` 同时使用 GHR / PHR / BWHR / LHR / IMLI，其中 PHR 用于 path table

像 `UBTB`、`ABTB`、`MBTB`、`RAS` 这些组件则不把 PHR 作为主要历史输入。

## 9. `BranchPredictor.py` 中的 PHR 长度示例

这一节只给典型配置，帮助建立量级感。

### 9.1 BTBTAGE

默认 `histLengths`：

```python
[4, 9, 17, 29, 56, 109, 211, 397]
```

也就是说，8 张表分别使用从 4 bit 到 397 bit 的 path-history window。

### 9.2 MicroTAGE

默认 `histLengths`：

```python
[5, 9, 17, 27]
```

### 9.3 BTBITTAGE

默认 `histLengths`：

```python
[4, 8, 13, 16, 32]
```

### 9.4 MGSC 的 P 表

默认 `pHistLen`：

```python
[8, 16]
```

这些都说明同一件事：

- 顶层完整 PHR 很长
- 但每个 predictor、每张表只取自己需要的历史窗口
- 然后再折叠成更短的 folded value，用于关键路径上的 index/tag

## 10. 结论

当前 BTB 顶层的 PHR 设计可以概括成三句话：

- 不再记录块内所有方向位，而是只记录最终 taken path
- 顶层采用“固定移入 2 bit + 混入 15 bit path hash”的轻量更新算法
- predictor 侧通过 folded history 等效维护完整 PHR 的压缩表示，以换取可接受的时序

这就是当前设计在“路径信息强度”和“实现代价”之间做出的平衡。

## 11. 实现锚点

- `src/cpu/pred/btb/decoupled_bpred.cc`
- `src/cpu/pred/btb/common.hh`
- `src/cpu/pred/btb/folded_hist.hh`
- `src/cpu/pred/btb/folded_hist.cc`
- `src/cpu/pred/BranchPredictor.py`

## 12. 参考资料

- `docs/design-docs/frontend/README.md`
- `docs/design-docs/frontend/bpu_top_level.md`
- `docs/design-docs/frontend/btb_tage_design.md`
- `docs/design-docs/frontend/microtage_design.md`
- `docs/design-docs/frontend/mgsc_design.md`
